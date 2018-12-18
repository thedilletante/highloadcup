#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <list>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <csignal>
#include <type_traits>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <httpxx/BufferedMessage.hpp>
#include <httpxx/Request.hpp>
#include <httpxx/ResponseBuilder.hpp>

#include <httpxx/http-parser/http_parser.h>
#include <memory>
#include <chrono>

#include <rapidjson/rapidjson.h>
#include <rapidjson/reader.h>
#include <rapidjson/stringbuffer.h>
#include <atomic>
#include <thread>

#include <boost/variant.hpp>
#include <boost/optional.hpp>
#include <boost/thread.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/signal_set.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include <gsl/string_span>

namespace utils {

  template <class T>
  class simple_allocator {
  public:
    using storage = typename std::aligned_storage<sizeof(T), alignof(T)>::type;

    explicit simple_allocator(size_t capacity)
      : slots(capacity)
      , arena(new storage[capacity]) {
      for (size_t i = 0; i < capacity; ++i) {
        slots[i] = arena.get() + i;
      }
    }

    template <class U = T, typename ...Args>
    T* allocate(Args&& ...args) {
      auto position_value = position.load(std::memory_order_acquire);
      if (position_value == slots.size())
        return nullptr;

      auto place = slots[position_value];
      position.store(position_value + 1, std::memory_order_release);
      return new (place) U (std::forward<Args>(args)...);
    }

    void deallocate(void* p) {
      auto object = reinterpret_cast<T*>(p);
      object->~T();
      slots[position.fetch_sub(1, std::memory_order_acquire) - 1] = reinterpret_cast<storage*>(p);
    }

  private:
    std::vector<storage*> slots;
    std::atomic_size_t position { 0 };
    std::unique_ptr<storage[]> arena;
  };


  template <class T>
  struct intrusive_list_item {
    T* prev = nullptr;
    T* next = nullptr;

    // returns new head element
    static T* push_front(T* elem, T* old_head) {
      elem->prev = nullptr;
      elem->next = old_head;
      if (old_head != nullptr) {
        old_head->prev = elem;
      }
      return elem;
    }

    // returns element next to deleted
    static T* remove(T* elem, T*& head) {
      auto prev = elem->prev;
      auto next = elem->next;

      if (prev) {
        prev->next = next;
      } else {
        head = next;
      }

      if (next) {
        next->prev = prev;
      }

      return next;
    }
  };

  template <class T, size_t CAPACITY>
  class circular_fifo {
  public:

    bool push(T t) {
      auto head_value = head.load(std::memory_order_relaxed);
      const auto tail_value = tail.load(std::memory_order_acquire);

      if (head_value == tail_value)
        return false;

      buffer[head_value] = t;
      circle_increment(head_value);
      head.store(head_value, std::memory_order_release);
      return true;
    }

    bool load(T* t) {
      const auto head_value = head.load(std::memory_order_acquire);
      auto tail_value = tail.load(std::memory_order_relaxed);

      if (head_value == tail_value + 1 || (head_value == 0 && tail_value == CAPACITY))
        return false;

      circle_increment(tail_value);
      *t = buffer[tail_value];
      tail.store(tail_value, std::memory_order_release);
      return true;
    }

  private:
    void circle_increment(size_t& value) {
      value += 1;
      value %= (CAPACITY + 1);
    }

    std::atomic_size_t head { 1 };
    std::atomic_size_t tail { 0 };
    T buffer[CAPACITY + 1];
  };

}

namespace tcp {

  template <size_t BUFFER_SIZE>
  class connection
    : public utils::intrusive_list_item<connection<BUFFER_SIZE>> {
  public:
    virtual ~connection() = default;

    virtual void on_data(ssize_t from, ssize_t size) = 0;

    template <class Container>
    size_t send(const Container& data) {
      inactive_ticks = 0;
      size_t rest = data.size();

      while (rest > 0) {
        const auto sent = ::send(socket->native_handle(), data.data() + data.size() - rest, rest, 0);
        if (sent == -1) {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;

          break;
        }
        rest -= sent;
      }

      return data.size() - rest;
    }

    void close() {
      closed = true;
    }


    bool closed = false;
    boost::asio::ip::tcp::socket* socket = nullptr;

    std::array<uint8_t, BUFFER_SIZE> buffer;
    size_t position = 0;

    uint32_t inactive_ticks = 0;
  };

  template <size_t BUFFER_SIZE>
  class connection_allocator {
  public:
    virtual ~connection_allocator() = default;

    virtual connection<BUFFER_SIZE>* allocate_and_build() = 0;
    virtual void deallocate(connection<BUFFER_SIZE>* c) = 0;
  };

template <size_t BUFFER_SIZE, size_t CONNECTIONS_SIZE>
class worker {
public:

  explicit worker(
    utils::circular_fifo<boost::asio::ip::tcp::socket*, CONNECTIONS_SIZE>* connections_queue,
    utils::circular_fifo<boost::asio::ip::tcp::socket*, CONNECTIONS_SIZE>* released_connections,
    std::unique_ptr<connection_allocator<BUFFER_SIZE>> allocator
  ) : connections_queue(connections_queue)
    , released_connections(released_connections)
    , allocator(std::move(allocator)) {}

  size_t pool() {
    boost::asio::ip::tcp::socket* socket = nullptr;
    if (connections_queue->load(&socket)) {
      if (auto client = allocator->allocate_and_build()) {
        client->socket = socket;
        clients_head = utils::intrusive_list_item<connection<BUFFER_SIZE>>::push_front(client, clients_head);
        ++clients_num;
      } else {
        released_connections->push(socket);
      }
    }

    for (auto client = clients_head; client != nullptr;) {
      if (client->closed || client->position == client->buffer.size()) {
        boost::system::error_code err;
        client->socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, err);
        client->socket->close(err);
        client = remove(client);
      } else {
        const auto recv_result = ::recv(client->socket->native_handle(), client->buffer.data() + client->position, client->buffer.size() - client->position, MSG_DONTWAIT);
        if (recv_result > 0) {
          const auto from = client->position;
          client->inactive_ticks = 0;
          client->position += recv_result;
          client->on_data(from, recv_result);
          client = client->next;
        } else {
          // approximately 7 seconds with 1 client connected
          static const auto max_ticks = std::numeric_limits<decltype(client->inactive_ticks)>::max() / (1024);
          if ((errno != EWOULDBLOCK && errno != EAGAIN) || (client->inactive_ticks > max_ticks - clients_num)) {
            boost::system::error_code err;
            client->socket->close(err);
            client = remove(client);
          } else {
            client->inactive_ticks += clients_num;
            client = client->next;
          }
        }
      }
    }

    return clients_num;

  }

private:
  connection<BUFFER_SIZE>* remove(connection<BUFFER_SIZE>* item) {
    released_connections->push(item->socket);
    auto to_deallocate = item;
    auto ret = utils::intrusive_list_item<connection<BUFFER_SIZE>>::remove(item, clients_head);
    allocator->deallocate(to_deallocate);
    --clients_num;
    return ret;
  }

private:
  utils::circular_fifo<boost::asio::ip::tcp::socket*, CONNECTIONS_SIZE>* connections_queue = nullptr;
  utils::circular_fifo<boost::asio::ip::tcp::socket*, CONNECTIONS_SIZE>* released_connections = nullptr;
  std::unique_ptr<connection_allocator<BUFFER_SIZE>> allocator;
  connection<BUFFER_SIZE>* clients_head = nullptr;
  uint64_t clients_num = 0;
};

template <size_t BUFFER_SIZE>
class connection_allocator_creator {
public:
  virtual ~connection_allocator_creator() = default;
  virtual std::unique_ptr<connection_allocator<BUFFER_SIZE>> create() = 0;
};


template <size_t BUFFER_SIZE, size_t CONCURRENT_CONNECTIONS>
class worker_pool_server {
public:

  explicit worker_pool_server(
    size_t pool_size,
    const char* ip,
    uint16_t port,
    size_t backlog,
    std::unique_ptr<connection_allocator_creator<BUFFER_SIZE>> creator
  ) : acceptor(service, boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string(ip), port), true)
    , creator(std::move(creator))
    , socket_allocator(backlog) {

    for (size_t i = 0; i < pool_size; ++i) {
      connection_queues.emplace_back();
      released_connections_queue.emplace_back();

      auto& queue = connection_queues.back();
      auto& release_queue = released_connections_queue.back();

      thread_group.create_thread([this, &queue, &release_queue]{
        worker<BUFFER_SIZE, CONCURRENT_CONNECTIONS> tcp_worker(&queue, &release_queue, this->creator->create());

        while (!finish.load(std::memory_order_acquire)) {
          tcp_worker.pool();
        }

        while (tcp_worker.pool());
      });
    }

    current_queue = connection_queues.begin();
  }

  ~worker_pool_server() {
    if (thread_group.size() > 0) {
      finish.store(true, std::memory_order_release);
      thread_group.join_all();
    }
  }

  void run() {
    if (const auto workers = thread_group.size()) {
      post_accept();
      service.run();
    }
  }

  void stop() {
    service.post([this]{
      stopped = true;
      boost::system::error_code err;
      acceptor.cancel(err);
    });
    work.reset();
  }

  boost::asio::io_service& io_service() {
    return service;
  }

private:
  void post_accept() {
    auto client = socket_allocator.allocate(service);
    acceptor.async_accept(*client, [this, client](const boost::system::error_code& error) {
      if (stopped || error)
        return;

      boost::system::error_code err;
      client->native_non_blocking(true, err);
      client->non_blocking(true, err);

      const auto workers = thread_group.size();
      bool pushed = false;
      for (size_t i = 0; i < workers; ++i) {
        pushed = current_queue->push(client);

        ++debug_queue_number;
        debug_queue_number %= thread_group.size();
        if (++current_queue == connection_queues.end()) {
          current_queue = connection_queues.begin();
        }

        if (pushed)
          break;
      }

      if (!pushed) {
        socket_allocator.deallocate(client);
      }


      size_t i = 0;
      for (auto& released_queue : released_connections_queue) {
        boost::asio::ip::tcp::socket* client = nullptr;
        while (released_queue.load(&client)) {
          socket_allocator.deallocate(client);
        }

        ++i;
        i %= thread_group.size();
      }

      post_accept();
    });
  }


  boost::asio::io_service service;
  boost::asio::ip::tcp::acceptor acceptor;
  boost::optional<boost::asio::io_service::work> work { boost::asio::io_service::work { service } };

  using connection_queues_list = std::list<utils::circular_fifo<boost::asio::ip::tcp::socket*, CONCURRENT_CONNECTIONS>>;
  connection_queues_list connection_queues;
  typename connection_queues_list::iterator current_queue;
  size_t debug_queue_number = 0;

  connection_queues_list released_connections_queue;

  boost::thread_group thread_group;
  std::atomic_bool finish { false };
  bool stopped = false;
  std::unique_ptr<connection_allocator_creator<BUFFER_SIZE>> creator;

  utils::simple_allocator<boost::asio::ip::tcp::socket> socket_allocator;
};

} // namespace tcp

namespace http {

  template <size_t BUFFER_SIZE>
  class parser
    : public tcp::connection<BUFFER_SIZE>  {
  public:
    using connection = tcp::connection<BUFFER_SIZE>;

    virtual void on_url(const char* at, size_t length) = 0;
    virtual void on_body(const char* at, size_t length) = 0;
    virtual void on_message_complete() = 0;

  private:
    static int on_url(http_parser* p, const char* at, size_t length) {
      reinterpret_cast<parser*>(p->data)->on_url(at, length);
      return 0;
    }

    static int on_body(http_parser* p, const char* at, size_t length) {
      reinterpret_cast<parser*>(p->data)->on_body(at, length);
      return 0;
    }
    static int on_message_complete(http_parser* p) {
      reinterpret_cast<parser*>(p->data)->on_message_complete();
      return 0;
    }

  public:
    parser() {
      p.data = this;
      http_parser_init(&p, HTTP_REQUEST);
      std::memset(&settings, 0, sizeof(settings));
      settings.on_url = &parser::on_url;
      settings.on_body = &parser::on_body;
      settings.on_message_complete = &parser::on_message_complete;
      lastAccess = std::chrono::high_resolution_clock::now();
    }

    void on_data(ssize_t from, ssize_t size) override {
      http_parser_execute(&p, &settings, reinterpret_cast<const char*>(connection::buffer.data() + from), size);
      lastAccess = std::chrono::high_resolution_clock::now();
    }

  private:
    http_parser p;
    http_parser_settings settings;

    std::chrono::high_resolution_clock::time_point lastAccess;
  };

}

namespace application {

  namespace requests {
    struct filter {

    };

    struct group {

    };

    struct recommended {

    };

    struct suggest {

    };

    struct update {

    };

    struct add {

    };

    struct like {

    };
  }

  using request = boost::variant<
    requests::filter,
    requests::group,
    requests::recommended,
    requests::suggest,
    requests::update,
    requests::add,
    requests::like
  >;

  struct response {

  };

  template <size_t BUFFER_SIZE>
  class tcp_pool_request_handler
    : public http::parser<BUFFER_SIZE> {
  public:
    void on_url(const char *at, size_t length) override {
      if (length >= 9 && 0 == ::strncmp(at, "/accounts", std::min<size_t>(length, 9))) {
        req = requests::filter();
      }
    }

    void on_body(const char *at, size_t length) override {}

    void on_message_complete() override {

      static const std::string end = "\r\n";

      http::ResponseBuilder builder;
      rapidjson::Document json;
      json.SetObject();

      rapidjson::StringBuffer strbuf;
      rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);

      builder.set_major_version(1);
      builder.set_minor_version(1);

      if (req) {
        json.AddMember("data", "some_data", json.GetAllocator());
        builder.set_status(200);
      } else {
        builder.set_status(404);
      }

      json.Accept(writer);

      builder.headers()["Connection-Type"] = "application/json";
      builder.headers()["Connection-Length"] = std::to_string(strbuf.GetSize());
      builder.headers()["Connection"] = "close";

      http::parser<BUFFER_SIZE>::send(builder.to_string());
      http::parser<BUFFER_SIZE>::send(gsl::make_span(strbuf.GetString(), strbuf.GetSize()));
      http::parser<BUFFER_SIZE>::send(end);
      http::parser<BUFFER_SIZE>::close();
    }

  private:
    boost::optional<request> req;
  };

  template <size_t BUFFER_SIZE>
  class tcp_pool_request_handler_allocator
    : public tcp::connection_allocator<BUFFER_SIZE> {
  public:
    explicit tcp_pool_request_handler_allocator(size_t connections_per_thread)
      : allocator(connections_per_thread) {}

    tcp::connection<BUFFER_SIZE> *allocate_and_build() override {
      return allocator.allocate();
    }

    void deallocate(tcp::connection<BUFFER_SIZE> *c) override {
      allocator.deallocate(c);
    }

  private:
    utils::simple_allocator<tcp_pool_request_handler<BUFFER_SIZE>> allocator;
  };

  template <size_t BUFFER_SIZE>
  class tcp_pool_request_hander_allocator_creator
    : public tcp::connection_allocator_creator<BUFFER_SIZE> {
  public:
    explicit tcp_pool_request_hander_allocator_creator(size_t connections_per_thread)
      : connections_per_thread(connections_per_thread) {}

    std::unique_ptr<tcp::connection_allocator<BUFFER_SIZE>> create() override {
      return std::unique_ptr<tcp::connection_allocator<BUFFER_SIZE>>(new tcp_pool_request_handler_allocator<BUFFER_SIZE>(connections_per_thread));
    }

  private:
    const size_t connections_per_thread;
  };

}

void run_tcp_pool_server() {
  uint16_t port = 8080;

  if (auto envPortValue = getenv("HIGHLOADCUP_PORT")) {
    if (auto portCandidate = std::stoi(envPortValue)) {
      if (portCandidate > 0) {
        port = portCandidate;
      }
    }
  }

  try {
    constexpr size_t buffer_size = 8192;
    constexpr size_t backlog = 8192;
    constexpr size_t workers_num = 4;
    constexpr size_t connections_per_thread = 1024;

    tcp::worker_pool_server<buffer_size, connections_per_thread> server(
      workers_num,
      "0.0.0.0",
      port,
      backlog,
      std::unique_ptr<tcp::connection_allocator_creator<buffer_size>>(
        new application::tcp_pool_request_hander_allocator_creator<buffer_size>(connections_per_thread)
      )
    );

    boost::asio::signal_set set(server.io_service(), SIGINT);
    set.async_wait([&server](const boost::system::error_code&, int signal_number){
      if (signal_number == SIGINT) {
        server.stop();
      }
    });
    server.run();
  } catch (const std::exception& e) {
    std::cout << "ERROR: " << e.what() << std::endl;
  }
}

void fifo_test() {
  utils::circular_fifo<int, 2> fifo;

  int i1 = 1;
  int i2 = 2;
  int i3 = 3;

  int load;

  // first load must fail
  assert(fifo.load(&load) == false);

  // first push/load must exchange the same element
  assert(fifo.push(i1) == true);
  assert(fifo.load(&load) == true);
  assert(load == i1);

  // fifo must be empty after first push/load
  assert(fifo.load(&load) == false);

  // push more than capacity must fail
  assert(fifo.push(i3) == true);
  assert(fifo.push(i2) == true);
  assert(fifo.push(i1) == false);
  assert(fifo.push(i1) == false);

  // load must exchange in the same order
  assert(fifo.load(&load) == true);
  assert(load == i3);

  assert(fifo.load(&load) == true);
  assert(load == i2);

  assert(fifo.load(&load) == false);

  // push between
  assert(fifo.push(i3) == true);
  assert(fifo.push(i2) == true);
  assert(fifo.push(i1) == false);

  assert(fifo.load(&load) == true);
  assert(load == i3);

  assert(fifo.push(i1) == true);

  assert(fifo.load(&load) == true);
  assert(load == i2);

  assert(fifo.load(&load) == true);
  assert(load == i1);

  assert(fifo.load(&load) == false);
  assert(fifo.load(&load) == false);
}


int main(int argc, char* argv[]) {
//  fifo_test();
  run_tcp_pool_server();
  return 0;
}