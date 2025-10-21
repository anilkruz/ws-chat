// beast_ws_chat_server.cpp
// Compile:
// g++ -std=c++17 beast_ws_chat_server.cpp -O2 -lboost_system -lpthread -o ws_chat_server
// Requires Boost.Beast/Asio and nlohmann/json.hpp (header-only)

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>

#include <nlohmann/json.hpp> // https://github.com/nlohmann/json

#include <iostream>
#include <memory>
#include <set>
#include <mutex>
#include <deque>
#include <string>
#include <vector>
#include <cstdlib>
#include <random>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using error_code = boost::system::error_code;
using json = nlohmann::json;

using Message = std::string;
using MessageQueue = std::deque<Message>;

static constexpr std::size_t MAX_MSG_LEN = 256;

static std::string make_id(size_t len = 12) {
    static const char* hex = "0123456789abcdef";
    static thread_local std::mt19937_64 gen(std::random_device{}());
    static thread_local std::uniform_int_distribution<int> dist(0, 15);
    std::string s; s.reserve(len);
    for (size_t i = 0; i < len; ++i) s.push_back(hex[dist(gen)]);
    return s;
}

class Session;
class WSChatServer {
public:
    WSChatServer(asio::io_context& ioc, tcp::endpoint endpoint);
    ~WSChatServer();

    void join(std::shared_ptr<Session> s);
    void leave(std::shared_ptr<Session> s);

    void broadcast_chat(const std::string& user, const std::string& msg, std::shared_ptr<Session> sender);
    void broadcast_system(const std::string& text);
    void send_presence_to_all();
    void send_presence_to(std::shared_ptr<Session> s);
    std::vector<std::string> snapshot_usernames_locked();

private:
    void do_accept();

    tcp::acceptor acceptor_;
    tcp::socket socket_;
    std::set<std::weak_ptr<Session>, std::owner_less<std::weak_ptr<Session>>> sessions_;
    std::mutex mutex_;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, WSChatServer& server)
        : ws_(std::move(socket)), server_(server), session_id_(make_id()) {}

    void start();
    void deliver(const Message& m);

    const std::string& username() const { return username_; }
    const std::string& session_id() const { return session_id_; }

private:
    void on_accept(error_code ec);
    void do_read();
    void on_read(error_code ec, std::size_t);
    void do_write();
    void on_write(error_code ec, std::size_t);
    void close_socket_graceful();

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer read_buf_;
    MessageQueue write_queue_;
    WSChatServer& server_;
    std::string username_;
    std::string session_id_;
};

//////////////////////////////////////////////////////////////////////////
// WSChatServer implementation
//////////////////////////////////////////////////////////////////////////

WSChatServer::WSChatServer(asio::io_context& ioc, tcp::endpoint endpoint)
    : acceptor_(ioc), socket_(ioc)
{
    error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) std::cerr << "acceptor open error: " << ec.message() << "\n";
    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    acceptor_.bind(endpoint, ec);
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);

    std::cout << "WebSocket chat server running on ws://0.0.0.0:" << endpoint.port() << "\n";
    do_accept();
}

WSChatServer::~WSChatServer() {
    error_code ec;
    acceptor_.close(ec);
}

void WSChatServer::do_accept() {
    acceptor_.async_accept(socket_, [this](error_code ec) {
        if (!ec) {
            auto session = std::make_shared<Session>(std::move(socket_), *this);
            session->start();
        } else {
            std::cerr << "accept error: " << ec.message() << "\n";
        }
        do_accept();
    });
}

void WSChatServer::join(std::shared_ptr<Session> s) {
    std::lock_guard<std::mutex> g(mutex_);
    sessions_.insert(s);
    send_presence_to(s);
}

void WSChatServer::leave(std::shared_ptr<Session> s) {
    std::string left_user;
    {
        std::lock_guard<std::mutex> g(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->expired()) it = sessions_.erase(it);
            else {
                if (it->lock() == s) {
                    if (!s->username().empty()) left_user = s->username();
                    it = sessions_.erase(it);
                } else ++it;
            }
        }
    }
    if (!left_user.empty()) {
        broadcast_system(">> " + left_user + " left");
        send_presence_to_all();
    }
}

void WSChatServer::broadcast_chat(const std::string& user, const std::string& msg, std::shared_ptr<Session> sender) {
    json j;
    j["type"] = "chat";
    j["user"] = user;
    j["msg"] = msg;
    std::string out = j.dump();

    std::lock_guard<std::mutex> g(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (auto sp = it->lock()) {
            if (sp != sender) sp->deliver(out);
            ++it;
        } else it = sessions_.erase(it);
    }
}

void WSChatServer::broadcast_system(const std::string& text) {
    json j;
    j["type"] = "system";
    j["text"] = text;
    std::string out = j.dump();

    std::lock_guard<std::mutex> g(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (auto sp = it->lock()) {
            sp->deliver(out);
            ++it;
        } else it = sessions_.erase(it);
    }
}

std::vector<std::string> WSChatServer::snapshot_usernames_locked() {
    std::vector<std::string> users;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (auto sp = it->lock()) {
            const auto &u = sp->username();
            if (!u.empty()) users.push_back(u);
            ++it;
        } else it = sessions_.erase(it);
    }
    return users;
}

void WSChatServer::send_presence_to_all() {
    json j;
    j["type"] = "presence";
    j["users"] = snapshot_usernames_locked();
    std::string out = j.dump();

    std::lock_guard<std::mutex> g(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (auto sp = it->lock()) {
            sp->deliver(out);
            ++it;
        } else it = sessions_.erase(it);
    }
}

void WSChatServer::send_presence_to(std::shared_ptr<Session> s) {
    json j;
    j["type"] = "presence";
    j["users"] = snapshot_usernames_locked();
    s->deliver(j.dump());
}

//////////////////////////////////////////////////////////////////////////
// Session implementation
//////////////////////////////////////////////////////////////////////////

void Session::start() {
    auto self = shared_from_this();
    ws_.async_accept(asio::bind_executor(ws_.get_executor(),
        [this, self](error_code ec) { on_accept(ec); }));
}

void Session::on_accept(error_code ec) {
    if (ec) {
        std::cerr << "accept failed: " << ec.message() << "\n";
        close_socket_graceful();
        return;
    }

    // Force text mode (we are sending JSON text frames)
    ws_.text(true);

    std::cout << "session accepted: id=" << session_id_ << "\n";
    server_.join(shared_from_this());
    do_read();
}

void Session::do_read() {
    auto self = shared_from_this();
    ws_.async_read(read_buf_, asio::bind_executor(ws_.get_executor(),
        [this, self](error_code ec, std::size_t bytes_transferred){ on_read(ec, bytes_transferred); }));
}

void Session::on_read(error_code ec, std::size_t) {
    if (ec == websocket::error::closed) {
        std::cout << "websocket closed: id=" << session_id_ << "\n";
        server_.leave(shared_from_this());
        close_socket_graceful();
        return;
    }
    if (ec) {
        std::cerr << "read error (" << session_id_ << "): " << ec.message() << "\n";
        server_.leave(shared_from_this());
        close_socket_graceful();
        return;
    }

    std::string raw = beast::buffers_to_string(read_buf_.data());
    read_buf_.consume(read_buf_.size());

    json incoming;
    try {
        incoming = json::parse(raw);
    } catch (...) {
        json err; err["type"]="system"; err["text"]="Invalid JSON";
        deliver(err.dump());
        do_read();
        return;
    }

    std::string typ = incoming.value("type", "chat");

    if (typ == "join") {
        std::string user = incoming.value("user", "");
        if (user.empty()) user = "Anonymous";
        bool first_set = username_.empty();
        username_ = user;
        if (first_set) {
            server_.broadcast_system(">> " + username_ + " joined");
            server_.send_presence_to_all();
        } else {
            server_.send_presence_to_all();
        }
    } else if (typ == "chat") {
        std::string text = incoming.value("msg", "");
        if (text.size() > MAX_MSG_LEN) {
            text = text.substr(0, MAX_MSG_LEN);
            json note; note["type"]="system"; note["text"]="Your message was truncated";
            deliver(note.dump());
        }
        if (username_.empty()) {
            std::string maybe = incoming.value("user", "");
            username_ = maybe.empty() ? std::string("Anonymous") : maybe;
            server_.broadcast_system(">> " + username_ + " joined");
            server_.send_presence_to_all();
        }
        server_.broadcast_chat(username_, text, shared_from_this());
    } else if (typ == "request_presence") {
        server_.send_presence_to(shared_from_this());
    } else {
        json err; err["type"]="system"; err["text"]="Unknown message type";
        deliver(err.dump());
    }

    do_read();
}

void Session::deliver(const Message& m) {
    auto self = shared_from_this();
    asio::post(ws_.get_executor(), [this, self, m]() {
        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(m);
        if (!write_in_progress) do_write();
    });
}

void Session::do_write() {
    auto self = shared_from_this();
    // use buffer referencing write_queue_.front() which lives in the deque
    ws_.async_write(asio::buffer(write_queue_.front()), asio::bind_executor(ws_.get_executor(),
        [this, self](error_code ec, std::size_t bytes_transferred){ on_write(ec, bytes_transferred); }));
}

void Session::on_write(error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "write error (" << session_id_ << "): " << ec.message() << "\n";
        server_.leave(shared_from_this());
        close_socket_graceful();
        return;
    }
    write_queue_.pop_front();
    if (!write_queue_.empty()) do_write();
}

void Session::close_socket_graceful() {
    error_code ignored;
    // try to close websocket properly first
    if (ws_.is_open()) {
        ws_.close(websocket::close_code::normal, ignored);
    }
    // then close underlying socket
    try {
        ws_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
        ws_.next_layer().close(ignored);
    } catch (...) {}
}

//////////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[]) {
    try {
        unsigned short port = 8080;
        const char* p = std::getenv("PORT");
        if (p && *p) port = static_cast<unsigned short>(std::stoi(p));
        else if (argc > 1) port = static_cast<unsigned short>(std::stoi(argv[1]));

        asio::io_context ioc{1};
        tcp::endpoint endpoint(tcp::v4(), port);
        WSChatServer server(ioc, endpoint);
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

