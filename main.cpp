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
//
//#include <rapidjson/rapidjson.h>
//#include <rapidjson/reader.h>
#include <atomic>
#include <thread>

#include <boost/variant.hpp>
#include <boost/thread.hpp>

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
    }

    bool load(T* t) {
      const auto head_value = head.load(std::memory_order_acquire);
      auto tail_value = tail.load(std::memory_order_relaxed);

      if (head_value == tail_value + 1 || (head_value == 0 && tail_value == CAPACITY))
        return false;

      circle_increment(tail_value);
      *t = buffer[tail_value];
      tail.store(tail_value, std::memory_order_release);
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
    virtual void on_idle(const std::chrono::high_resolution_clock::time_point& tp) {};

    template <class Container>
    size_t send(const Container& data) {
      size_t rest = data.size();

      while (rest > 0) {
        const auto sent = ::send(socket, data.data() + data.size() - rest, rest, 0);
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
      ::shutdown(socket, SHUT_WR);
      ::close(socket);
      closed = true;
    }


    bool closed = false;
    int socket = -1;

    std::array<uint8_t, BUFFER_SIZE> buffer;
    size_t position = 0;
  };

  template <size_t BUFFER_SIZE>
  class connection_allocator {
  public:
    virtual ~connection_allocator() = default;

    virtual connection<BUFFER_SIZE>* allocate_and_build() = 0;
    virtual void deallocate(connection<BUFFER_SIZE>* c) = 0;
  };

//template <size_t BUFFER_SIZE>
//class server {
//public:
//
//  explicit server(const char* ip, uint16_t port, size_t backlog)
//    : server_socket { ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0) } {
//    if (server_socket == -1)
//      throw std::runtime_error("failed to create server socket: " + std::string(::strerror(errno)));
//
//    ::fcntl(server_socket, F_SETFL, O_NONBLOCK);
//
//    int value = 1;
//    ::setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
//    ::setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
//
//    sockaddr_in sa;
//
//    sa.sin_family = AF_INET;
//    sa.sin_port = htons(port);
//    if (1 != ::inet_pton(AF_INET, ip, &(sa.sin_addr)))
//      throw std::runtime_error("failed to convert " + std::string(ip) + " to sockaddr_in: " + std::string(::strerror(errno)));
//
//    if (0 != ::bind(server_socket, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)))
//      throw std::runtime_error("failed to bind server socket: " + std::string(::strerror(errno)));
//
//    if (0 != ::listen(server_socket, backlog))
//      throw std::runtime_error("failed to listen to server socket: " + std::string(::strerror(errno)));
//  }
//
//  ~server() {
//    for (auto client = clients_head; client != nullptr; client = client->next) {
//      ::close(client->socket);
//    }
//    ::close(server_socket);
//  }
//
//  // returns number of managed endpoints
//  size_t pool(connection_allocator<BUFFER_SIZE>& allocator, bool accepting = true) {
//    if (accepting)
//      try_accept(allocator);
//
//    const auto now = std::chrono::high_resolution_clock::now();
//    for (auto client = clients_head; client != nullptr;) {
//      if (client->closed) {
//        auto to_deallocate = client;
//        client = utils::intrusive_list_item<connection<BUFFER_SIZE>>::remove(client, clients_head);
//        allocator.deallocate(to_deallocate);
//        --clients_num;
//      } else {
//        const auto recv_result = ::recv(client->socket, client->buffer.data() + client->position, client->buffer.size() - client->position, MSG_DONTWAIT);
//
//        if (recv_result < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
//          client->on_idle(now);
//          client = client->next;
//        } else if (recv_result > 0) {
//          const auto from = client->position;
//          client->position += recv_result;
//          client->on_data(from, recv_result);
//          client = client->next;
//        } else {
//          ::close(client->socket);
//          auto to_deallocate = client;
//          client = utils::intrusive_list_item<connection<BUFFER_SIZE>>::remove(client, clients_head);
//          allocator.deallocate(to_deallocate);
//          --clients_num;
//        }
//      }
//
//    }
//
//    return clients_num;
//  }
//
//private:
//  void try_accept(connection_allocator<BUFFER_SIZE>& allocator)  {
//    const auto candidate = accept(server_socket, nullptr, nullptr);
//    if (candidate < 0)
//      return;
//
//    if (::fcntl(candidate, F_SETFL, O_NONBLOCK) == 0) {
//      if (auto client = allocator.allocate_and_build()) {
//        client->socket = candidate;
//        clients_head = utils::intrusive_list_item<connection<BUFFER_SIZE>>::push_front(client, clients_head);
//        ++clients_num;
//        return;
//      }
//    }
//
//    ::close(candidate);
//    return;
//  }
//
//private:
//  int server_socket = -1;
//  // without dynamic allocation
//
//  connection<BUFFER_SIZE>* clients_head = nullptr;
//  uint64_t clients_num = 0;
//};


template <size_t BUFFER_SIZE>
class worker {
public:

  explicit worker(
    utils::circular_fifo<int, 1>* server_socket_in,
    utils::circular_fifo<int, 1>* server_socket_out,
    std::unique_ptr<connection_allocator<BUFFER_SIZE>> allocator
  ) : server_socket_in(server_socket_in)
    , server_socket_out(server_socket_out)
    , allocator(std::move(allocator)) {}

  size_t pool(bool accepting = true) {

    if (accepting) {
      if (server_socket != -1 || server_socket_in->load(&server_socket)) {
        const auto clients_num_before = clients_num;
        try_accept();
        if (clients_num_before != clients_num) {
          server_socket_out->push(server_socket);
          server_socket = -1;
        }
      }
    }

    const auto now = std::chrono::high_resolution_clock::now();
    for (auto client = clients_head; client != nullptr;) {
      if (client->closed) {
        client = remove(client);
      } else if (client->position != client->buffer.size()) {
        const auto recv_result = ::recv(client->socket, client->buffer.data() + client->position, client->buffer.size() - client->position, MSG_DONTWAIT);

        if (recv_result < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
          client->on_idle(now);
          client = client->next;
        } else if (recv_result > 0) {
          const auto from = client->position;
          client->position += recv_result;
          client->on_data(from, recv_result);
          client = client->next;
        } else {
          ::close(client->socket);
          client = remove(client);
        }
      } else {
        client->on_idle(now);
        client = client->next;
      }
    }

    return clients_num;

  }

private:
  connection<BUFFER_SIZE>* remove(connection<BUFFER_SIZE>* item) {
    auto to_deallocate = item;
    auto ret = utils::intrusive_list_item<connection<BUFFER_SIZE>>::remove(item, clients_head);
    allocator->deallocate(to_deallocate);
    --clients_num;
    return ret;
  }

  void try_accept()  {
    const auto candidate = accept(server_socket, nullptr, nullptr);
    if (candidate < 0)
      return;

    if (::fcntl(candidate, F_SETFL, O_NONBLOCK) == 0) {
      if (auto client = allocator->allocate_and_build()) {
        client->socket = candidate;
        clients_head = utils::intrusive_list_item<connection<BUFFER_SIZE>>::push_front(client, clients_head);
        ++clients_num;
        return;
      }
    }

    ::close(candidate);
    return;
  }

private:
  int server_socket = -1;
  utils::circular_fifo<int, 1>* server_socket_in = nullptr;
  utils::circular_fifo<int, 1>* server_socket_out = nullptr;
  std::unique_ptr<connection_allocator<BUFFER_SIZE>> allocator;
  connection<BUFFER_SIZE>* clients_head = nullptr;
  uint64_t clients_num = 0;
};

class socket_holder {
public:
  explicit socket_holder()
    : socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) {}

  ~socket_holder() {
    if (socket != -1) {
      ::close(socket);
    }
  }

  socket_holder(const socket_holder&) = delete;
  socket_holder& operator=(const socket_holder&) = delete;


  int handle() const {
    return socket;
  }

private:
  int socket = -1;
};

template <size_t BUFFER_SIZE>
class connection_allocator_creator {
public:
  virtual ~connection_allocator_creator() = default;
  virtual std::unique_ptr<connection_allocator<BUFFER_SIZE>> create() = 0;
};


template <size_t BUFFER_SIZE>
class worker_pool_server {
public:

  explicit worker_pool_server(
    size_t pool_size,
    const char* ip,
    uint16_t port,
    size_t backlog,
    std::unique_ptr<connection_allocator_creator<BUFFER_SIZE>> creator
  ) : creator(std::move(creator)) {
    if (server_socket.handle() == -1)
      throw std::runtime_error("failed to create server socket: " + std::string(::strerror(errno)));

    ::fcntl(server_socket.handle(), F_SETFL, O_NONBLOCK);

    int value = 1;
    ::setsockopt(server_socket.handle(), SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
    ::setsockopt(server_socket.handle(), SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));

    sockaddr_in sa;

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (1 != ::inet_pton(AF_INET, ip, &(sa.sin_addr)))
      throw std::runtime_error("failed to convert " + std::string(ip) + " to sockaddr_in: " + std::string(::strerror(errno)));

    if (0 != ::bind(server_socket.handle(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)))
      throw std::runtime_error("failed to bind server socket: " + std::string(::strerror(errno)));

    if (0 != ::listen(server_socket.handle(), backlog))
      throw std::runtime_error("failed to listen to server socket: " + std::string(::strerror(errno)));

    for (size_t i = 0; i < pool_size; ++i) {
      server_socket_queues.emplace_back();
    }

    auto server_socket_iter = server_socket_queues.begin();
    server_socket_iter->push(server_socket.handle());
    for (size_t i = 0; i < pool_size; ++i) {
      auto in_iter = server_socket_iter;
      ++server_socket_iter;
      if (server_socket_iter == server_socket_queues.end())
        server_socket_iter = server_socket_queues.begin();
      auto out_iter = server_socket_iter;
      thread_group.create_thread([this, i, pool_size, in_iter, out_iter]{
        worker<BUFFER_SIZE> tcp_worker(&*in_iter, &*out_iter, this->creator->create());

        while (!finish.load(std::memory_order_acquire)) {
          tcp_worker.pool();
        }

        while (tcp_worker.pool(false));
      });
    }
  }

  ~worker_pool_server() {
    finish.store(true, std::memory_order_release);
    thread_group.join_all();
  }

private:
  std::list<utils::circular_fifo<int, 1>> server_socket_queues;
  boost::thread_group thread_group;
  socket_holder server_socket;
  std::atomic_bool finish { false };
  std::unique_ptr<connection_allocator_creator<BUFFER_SIZE>> creator;
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

    void on_idle(const std::chrono::high_resolution_clock::time_point& tp) override {
      if (tp - lastAccess > std::chrono::seconds(5)) {
        connection::close();
      }
    }

  private:
    http_parser p;
    http_parser_settings settings;

    std::chrono::high_resolution_clock::time_point lastAccess;
  };

}

namespace application {

  std::string someResponse() {

    static constexpr const char *end = "\r\n";

    static const std::string data{R"json({"data":"some_data"})json"};

    http::ResponseBuilder builder;
    builder.set_status(200);
    builder.set_major_version(1);
    builder.set_minor_version(1);

    builder.headers()["Connection-Type"] = "application/json";
    builder.headers()["Connection-Length"] = std::to_string(data.size());
    builder.headers()["Connection"] = "close";

    return builder.to_string() + data + end + end;
  }


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

//
//  struct processing_info {
//    using done_callback = void(void*);
//
//    const request* req;
//    response* resp;
//    done_callback* callback;
//    void* data;
//  };
//
//  template <size_t FIFO_SIZE>
//  class worker {
//  public:
//    void pool() {
//      processing_info info;
//      if (queue.load(&info)) {
//        // process it;
//        info.callback(info.data);
//      }
//    }
//
//
//    void process_response(const request* req, response* resp, processing_info::done_callback callback, void* data) {
//      queue.push(processing_info{
//        .req = req,
//        .resp = resp,
//        .callback = callback,
//        .data = data
//      });
//    }
//
//  private:
//    utils::circular_fifo<processing_info, FIFO_SIZE> queue;
//  };
//
//  template <size_t BUFFER_SIZE, size_t FIFO_SIZE>
//  class request_handler
//    : public http::parser<BUFFER_SIZE> {
//  public:
//    using parent = http::parser<BUFFER_SIZE>;
//
//    void on_processing_done() {
//      resp_ready.store(true, std::memory_order_release);
//      allocator.deallocate(this);
//    }
//
//    static void on_processing_done(void* data) {
//      reinterpret_cast<request_handler<BUFFER_SIZE, FIFO_SIZE>*>(data)->on_processing_done();
//    }
//
//  public:
//    explicit request_handler(tcp::connection_allocator<BUFFER_SIZE>& allocator, worker<FIFO_SIZE>& w)
//      : allocator(allocator)
//      , processing_worker(w) {}
//
//    void on_url(const char *at, size_t length) override {}
//    void on_body(const char *at, size_t length) override {}
//
//    void on_message_complete() override {
//      pin();
//      processing_worker.process_response(&req, &resp, &request_handler<BUFFER_SIZE, FIFO_SIZE>::on_processing_done, this);
//    }
//
//    void on_idle(const std::chrono::high_resolution_clock::time_point& tp) override {
//      if (resp_ready.load(std::memory_order_acquire)) {
//        static const auto message = someResponse();
//        parent::connection::send(message);
//        parent::connection::close();
//      } else if (!executing) {
//        parent::on_idle(tp);
//      }
//    }
//
//    bool unpin() {
//      return ref_cnt.fetch_sub(1) == 1;
//    }
//
//  private:
//    void pin() {
//      ++ref_cnt;
//    }
//
//  private:
//    tcp::connection_allocator<BUFFER_SIZE>& allocator;
//    worker<FIFO_SIZE>& processing_worker;
//
//    request req;
//    response resp;
//
//    std::atomic_bool resp_ready { false };
//    bool executing = false;
//    std::atomic<uint8_t> ref_cnt { 1 };
//  };
//
//  template <size_t BUFFER_SIZE, size_t WORKERS_NUM, size_t FIFO_SIZE>
//  class simple_server
//    : public tcp::connection_allocator<BUFFER_SIZE> {
//  public:
//
//    explicit simple_server(const char* ip, uint16_t port, size_t concurrent_connections)
//      : tcp_server(ip, port, concurrent_connections)
//      , allocator(concurrent_connections) {
//      for (auto& w : workers) {
//        std::thread([&w] { while(true) w.pool(); }).detach();
//      }
//    }
//
//    void run() {
//      tcp_server.pool(*this);
//    }
//
//    void shutdown() {
//      while (tcp_server.pool(*this, false));
//    }
//
//    tcp::connection<BUFFER_SIZE>* allocate_and_build() override {
//      auto result = allocator.allocate(*this, workers[current_worker]);
//      ++current_worker;
//      current_worker %= WORKERS_NUM;
//      return result;
//    }
//
//    void deallocate(tcp::connection<BUFFER_SIZE>* c) override {
//      if (reinterpret_cast<request_handler<BUFFER_SIZE, FIFO_SIZE>*>(c)->unpin())
//        allocator.deallocate(c);
//    }
//
//  private:
//    tcp::server<BUFFER_SIZE> tcp_server;
//    utils::simple_allocator<request_handler<BUFFER_SIZE, FIFO_SIZE>> allocator;
//    worker<FIFO_SIZE> workers[WORKERS_NUM];
//    size_t current_worker = 0;
//  };



  template <size_t BUFFER_SIZE>
  class tcp_pool_request_handler
    : public http::parser<BUFFER_SIZE> {
  public:
    void on_url(const char *at, size_t length) override {}

    void on_body(const char *at, size_t length) override {}

    void on_message_complete() override {
      static const auto response = someResponse();
      http::parser<BUFFER_SIZE>::send(response);
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




bool done = false;
void handle_sigint(int sig)
{
  done = true;
}

std::promise<void> sigint_signal;
void set_sigint(int sig) {
  sigint_signal.set_value();
}

void run_tcp_pool_server() {
  uint16_t port = 80;

  if (auto envPortValue = getenv("HIGHLOADCUP_PORT")) {
    if (auto portCandidate = std::stoi(envPortValue)) {
      if (portCandidate > 0) {
        port = portCandidate;
      }
    }
  }

  ::signal(SIGINT, set_sigint);
  try {
    constexpr size_t buffer_size = 8192;
    constexpr size_t backlog = 2048;
    constexpr size_t workers_num = 3;
    constexpr size_t connections_per_thread = 1024;

    tcp::worker_pool_server<buffer_size> server(
      workers_num,
      "0.0.0.0",
      port,
      backlog,
      std::unique_ptr<tcp::connection_allocator_creator<buffer_size>>(new application::tcp_pool_request_hander_allocator_creator<buffer_size>(connections_per_thread))
    );

    sigint_signal.get_future().wait();
  } catch (const std::exception& e) {
    std::cout << "ERROR: " << e.what() << std::endl;
  }
}


//void run_server() {
//  uint16_t port = 80;
//
//  if (auto envPortValue = getenv("HIGHLOADCUP_PORT")) {
//    if (auto portCandidate = std::stoi(envPortValue)) {
//      if (portCandidate > 0) {
//        port = portCandidate;
//      }
//    }
//  }
//
//  ::signal(SIGINT, handle_sigint);
//  try {
//    constexpr size_t buffer_size = 8192;
//    constexpr size_t backlog = 2048;
//    constexpr size_t workers_num = 3;
//    constexpr size_t worker_queue_size = 1000;
//
//    application::simple_server<buffer_size, workers_num, worker_queue_size> server("0.0.0.0", port, backlog);
//
//    while (!done) {
//      server.run();
//    }
//    server.shutdown();
//  } catch (const std::exception& e) {
//    std::cout << "ERROR: " << e.what() << std::endl;
//  }
//}

//void fifo_test() {
//  utils::circular_fifo<int, 2> fifo;
//
//  int i1 = 1;
//  int i2 = 2;
//  int i3 = 3;
//
//  int load;
//
//  // first load must fail
//  assert(fifo.load(&load) == false);
//
//  // first push/load must exchange the same element
//  assert(fifo.push(i1) == true);
//  assert(fifo.load(&load) == true);
//  assert(load == i1);
//
//  // fifo must be empty after first push/load
//  assert(fifo.load(&load) == false);
//
//  // push more than capacity must fail
//  assert(fifo.push(i3) == true);
//  assert(fifo.push(i2) == true);
//  assert(fifo.push(i1) == false);
//  assert(fifo.push(i1) == false);
//
//  // load must exchange in the same order
//  assert(fifo.load(&load) == true);
//  assert(load == i3);
//
//  assert(fifo.load(&load) == true);
//  assert(load == i2);
//
//  assert(fifo.load(&load) == false);
//
//  // push between
//  assert(fifo.push(i3) == true);
//  assert(fifo.push(i2) == true);
//  assert(fifo.push(i1) == false);
//
//  assert(fifo.load(&load) == true);
//  assert(load == i3);
//
//  assert(fifo.push(i1) == true);
//
//  assert(fifo.load(&load) == true);
//  assert(load == i2);
//
//  assert(fifo.load(&load) == true);
//  assert(load == i1);
//
//  assert(fifo.load(&load) == false);
//  assert(fifo.load(&load) == false);
//}


int main(int argc, char* argv[]) {
//  fifo_test();
//  run_server();
  run_tcp_pool_server();
  return 0;
}