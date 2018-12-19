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

#define BOOST_RESULT_OF_USE_TR1 1
#include <boost/spirit/include/qi.hpp>
#include <boost/fusion/include/std_pair.hpp>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include <gsl/string_span>

extern "C" {
#include <yuarel.h>
};

#include <SQLiteCpp/Database.h>

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

    void on_url(const char* at, size_t length) {
      if (url_position == nullptr) {
        url_position = const_cast<char*>(at);
      }
      url_length += length;
    }

    void on_body(const char* at, size_t length) {
      if (body_position == nullptr) {
        body_position = const_cast<char*>(at);
      }
      body_length += length;
    }

    void on_message_complete() {
      method = static_cast<http_method>(p.method);
      path = url_position;
      path_length = url_length;
      url_position[url_length] = '\0';

      if (path != nullptr && path_length != 0) {
        if (auto question_pos = reinterpret_cast<char*>(memchr(path, '?', path_length))) {
          path_length = (question_pos - path);

          if (path_length + 1 < url_length) {
            params_length = yuarel_parse_query(question_pos + 1, '&', query_params.data(), query_params.size());
          }
        }
      }

      on_request_ready();
    }

    void on_header_field(const char* at, size_t length) {
      if (headers_length >= headers.size())
        return;

      if (headers_started) {
        ++headers_length;
      } else {
        headers_started = true;
      }

      auto& current_header = headers[headers_length];
      if (current_header.key == nullptr) {
        current_header.key = const_cast<char*>(at);
      }
      current_header.key_length += length;
    }

    void on_header_value(const char* at, size_t length) {
      if (headers_length >= headers.size())
        return;

      auto& current_header = headers[headers_length];
      if (current_header.value == nullptr) {
        current_header.value = const_cast<char*>(at);
      }
      current_header.value_length += length;
    }

    virtual void on_request_ready() = 0;

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

    static int on_header_field(http_parser* p, const char* at, size_t length) {
      reinterpret_cast<parser*>(p->data)->on_header_field(at, length);
      return 0;
    }

    static int on_header_value(http_parser* p, const char* at, size_t length) {
      reinterpret_cast<parser*>(p->data)->on_header_value(at, length);
      return 0;
    }

  public:
    parser() {
      p.data = this;
      http_parser_init(&p, HTTP_REQUEST);
      std::memset(&settings, 0, sizeof(settings));
      settings.on_url = &parser::on_url;
      settings.on_header_field = &parser::on_header_field;
      settings.on_header_value = &parser::on_header_value;
      settings.on_body = &parser::on_body;
      settings.on_message_complete = &parser::on_message_complete;
    }

    void on_data(ssize_t from, ssize_t size) override {
      const auto handled = http_parser_execute(&p, &settings, reinterpret_cast<const char*>(connection::buffer.data() + from), size);
      if (handled != size) {
        connection::close();
      }
    }

  private:
    http_parser p;
    http_parser_settings settings;

    bool headers_started = false;

  protected:
    http_method method;

    char* url_position = nullptr;
    size_t url_length = 0;

    char* path = nullptr;
    size_t path_length = 0;

    std::array<yuarel_param, 30> query_params;
    size_t params_length = 0;

    struct header {
      char* key = nullptr;
      size_t key_length = 0;

      char* value = nullptr;
      size_t value_length = 0;
    };

    std::array<header, 30> headers;
    size_t headers_length = 0;

    char* body_position = nullptr;
    size_t body_length = 0;
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

    using parsed = http::parser<BUFFER_SIZE>;

    void on_request_ready() override {
      static const std::string end = "\r\n";

      http::ResponseBuilder builder;
      rapidjson::Document json;
      json.SetObject();

      rapidjson::StringBuffer strbuf;
      rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);

      builder.set_major_version(1);
      builder.set_minor_version(1);

      builder.set_status(200);

      json.AddMember("method", rapidjson::Value(parsed::method), json.GetAllocator());

      if (parsed::path != nullptr) {
        json.AddMember("path", rapidjson::Value(parsed::path, parsed::path_length), json.GetAllocator());
      }

      if (parsed::headers_length > 0) {
        rapidjson::Value headers(rapidjson::kArrayType);
        for (size_t i = 0; i < parsed::headers_length; ++i) {
          rapidjson::Value header(rapidjson::kObjectType);
          header.AddMember(
            rapidjson::Value(parsed::headers[i].key, parsed::headers[i].key_length),
            rapidjson::Value(parsed::headers[i].value, parsed::headers[i].value_length),
            json.GetAllocator());
          headers.PushBack(header.Move(), json.GetAllocator());
        }
        json.AddMember("headers", headers.Move(), json.GetAllocator());
      }

      if (parsed::params_length > 0) {
        rapidjson::Value params(rapidjson::kArrayType);
        for (size_t i = 0; i < parsed::params_length; ++i) {
          rapidjson::Value param(rapidjson::kObjectType);
          param.AddMember(
            rapidjson::Value(parsed::query_params[i].key, json.GetAllocator()).Move(),
            parsed::query_params[i].val ? rapidjson::Value(parsed::query_params[i].val, json.GetAllocator()).Move() : rapidjson::Value().Move(),
            json.GetAllocator()
          );
          params.PushBack(param.Move(), json.GetAllocator());
        }
        json.AddMember("params", params.Move(), json.GetAllocator());
      }

      if (parsed::body_position != nullptr) {
        json.AddMember("body", rapidjson::Value(parsed::body_position, parsed::body_length), json.GetAllocator());
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

#include <sqlite3.h>

void db_test() {
  SQLite::Database db("example.db3", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLITE_OPEN_NOMUTEX);

  db.exec("DROP TABLE IF EXISTS test");
  db.exec("CREATE TABLE test (value TEXT)");

  const std::list<std::string> texts = {
    "hello",
    "ahfdskjz",
    "aksjbdflkabsf"
  };

  for (auto& text : texts) {
    SQLite::Statement q(db, "INSERT INTO test VALUES(:text)");
    q.bind(":text", text);
    q.exec();
  }

  SQLite::Statement s(db, "SELECT value FROM test");
  while (s.executeStep()) {
    std::cout << "text is: " << s.getColumn(0).getString() << std::endl;
  }
}

int main(int argc, char* argv[]) {

  db_test();
 for (auto i = 1; i < argc; ++i) {
   fprintf(stdout, "file provided: %s\n", argv[i]);
 }
//  fifo_test();
 run_tcp_pool_server();
  return 0;
}