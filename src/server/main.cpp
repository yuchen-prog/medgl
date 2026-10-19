#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef BOOST_ERROR_CODE_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#endif

#include "protocol.hpp"
#include "medgl/dicom.hpp"
#include "medgl/image_codec.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")
#endif

namespace medgl::server {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace fs = std::filesystem;
using Tcp = asio::ip::tcp;
using ErrorCode = boost::system::error_code;
using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

constexpr std::size_t ConnectionLimit = 64;
constexpr std::size_t SessionLimit = 128;
constexpr std::uint64_t StaticFileLimit = 64 * 1024 * 1024;

struct Config {
    unsigned short port = 8080;
    fs::path dataRoot = "data";
    fs::path webRoot = "web/dist";
    std::optional<fs::path> fixtureRoot;
    bool allowInstanceUidMismatch = false;
    PngCompression pngCompression = PngCompression::Default;
};

std::uint64_t Decimal(const std::string& text, std::uint64_t maximum) {
    if (text.empty() || text.size() > 20) {
        throw InputError("Invalid integer parameter.");
    }
    std::uint64_t result = 0;
    for (const auto character : text) {
        if (character < '0' || character > '9') {
            throw InputError("Invalid integer parameter.");
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (digit > maximum || result > (maximum - digit) / 10) {
            throw InputError("Integer parameter is outside its allowed range.");
        }
        result = result * 10 + digit;
    }
    return result;
}

Config ParseConfig(int argumentCount, char** arguments) {
    Config config;
    for (int index = 1; index < argumentCount; ++index) {
        const std::string option = arguments[index];
        if (option == "--allow-instance-uid-mismatch") {
            config.allowInstanceUidMismatch = true;
            continue;
        }
        if (option == "--help") {
            std::cout << "MedGL server: --port 8080 --data-root data --web-root web/dist "
                         "[--fixture-root directory] [--allow-instance-uid-mismatch] "
                         "[--png-compression default|fast]\n";
            std::exit(EXIT_SUCCESS);
        }
        if (option != "--port" && option != "--data-root" && option != "--web-root" &&
            option != "--fixture-root" && option != "--png-compression") {
            throw InputError("Unknown command-line option.");
        }
        if (++index >= argumentCount) {
            throw InputError("A command-line option requires a value.");
        }
        const std::string value = arguments[index];
        if (option == "--port") {
            const auto port = Decimal(value, 65535);
            if (port == 0) {
                throw InputError("Port must be between 1 and 65535.");
            }
            config.port = static_cast<unsigned short>(port);
        } else if (option == "--data-root") {
            config.dataRoot = fs::u8path(value);
        } else if (option == "--web-root") {
            config.webRoot = fs::u8path(value);
        } else if (option == "--fixture-root") {
            config.fixtureRoot = fs::u8path(value);
        } else if (option == "--png-compression") {
            if (value != "default" && value != "fast") throw InputError("Invalid PNG compression mode.");
            config.pngCompression = value == "fast" ? PngCompression::Fast : PngCompression::Default;
        }
        if (value.empty()) {
            throw InputError("Command-line values must not be empty.");
        }
    }
    return config;
}

int HexDigit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    throw InputError("Invalid percent escape in request target.");
}

std::string Decode(const std::string& text, bool query) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        auto character = static_cast<unsigned char>(text[index]);
        if (character == '%') {
            if (text.size() - index < 3) {
                throw InputError("Invalid percent escape in request target.");
            }
            character = static_cast<unsigned char>(
                HexDigit(text[index + 1]) * 16 + HexDigit(text[index + 2]));
            index += 2;
        } else if (query && character == '+') {
            character = ' ';
        }
        if (character < 32 || character == 127 || character == '\\') {
            throw InputError("Invalid character in request target.");
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

struct Target {
    std::string path;
    std::map<std::string, std::string> query;
};

Target ParseTarget(beast::string_view raw) {
    if (raw.empty() || raw.front() != '/' || raw.size() > 4096 ||
        raw.find('#') != beast::string_view::npos) {
        throw InputError("Invalid request target.");
    }
    const std::string text(raw);
    const auto separator = text.find('?');
    Target result{Decode(text.substr(0, separator), false), {}};
    if (separator == std::string::npos) {
        return result;
    }
    std::size_t offset = separator + 1;
    while (offset < text.size()) {
        const auto end = text.find('&', offset);
        const auto pair = text.substr(offset, end == std::string::npos ? end : end - offset);
        const auto equals = pair.find('=');
        if (equals == std::string::npos || equals == 0 || result.query.size() >= 16) {
            throw InputError("Invalid query parameters.");
        }
        auto key = Decode(pair.substr(0, equals), true);
        auto value = Decode(pair.substr(equals + 1), true);
        if (!result.query.emplace(std::move(key), std::move(value)).second) {
            throw InputError("Duplicate query parameter.");
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
        if (offset == text.size()) {
            throw InputError("Invalid query parameters.");
        }
    }
    return result;
}

std::vector<std::string> PathParts(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t offset = 1;
    while (offset < path.size()) {
        const auto end = path.find('/', offset);
        auto part = path.substr(offset, end == std::string::npos ? end : end - offset);
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos ||
            part.back() == '.' || part.back() == ' ') {
            throw InputError("Invalid path component.");
        }
        parts.push_back(std::move(part));
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
    }
    return parts;
}

bool WithinRoot(const fs::path& root, const fs::path& path) {
    auto candidate = path.begin();
    for (auto component = root.begin(); component != root.end(); ++component, ++candidate) {
        if (candidate == path.end() ||
            !beast::iequals(component->u8string(), candidate->u8string())) {
            return false;
        }
    }
    return candidate != path.end();
}

const char* MimeType(const fs::path& path) {
    const auto extension = path.extension().u8string();
    if (beast::iequals(extension, ".html")) return "text/html; charset=utf-8";
    if (beast::iequals(extension, ".js") || beast::iequals(extension, ".mjs"))
        return "text/javascript; charset=utf-8";
    if (beast::iequals(extension, ".css")) return "text/css; charset=utf-8";
    if (beast::iequals(extension, ".json") || beast::iequals(extension, ".map"))
        return "application/json; charset=utf-8";
    if (beast::iequals(extension, ".png")) return "image/png";
    if (beast::iequals(extension, ".jpg") || beast::iequals(extension, ".jpeg"))
        return "image/jpeg";
    if (beast::iequals(extension, ".svg")) return "image/svg+xml";
    if (beast::iequals(extension, ".webp")) return "image/webp";
    if (beast::iequals(extension, ".ico")) return "image/x-icon";
    if (beast::iequals(extension, ".woff")) return "font/woff";
    if (beast::iequals(extension, ".woff2")) return "font/woff2";
    if (beast::iequals(extension, ".wasm")) return "application/wasm";
    return "application/octet-stream";
}

enum class Lifecycle { Created, Loading, Ready, Failed, Closing, Closed };

const char* StateName(Lifecycle state) {
    switch (state) {
    case Lifecycle::Created: return "created";
    case Lifecycle::Loading: return "loading";
    case Lifecycle::Ready: return "ready";
    case Lifecycle::Failed: return "failed";
    case Lifecycle::Closing: return "closing";
    case Lifecycle::Closed: return "closed";
    }
    return "failed";
}

struct Frame {
    std::uint64_t seq = 0;
    std::uint64_t revision = 0;
    std::size_t payloadOffset = 0;
    std::vector<std::uint8_t> wire;
};

class Server;
class HttpConnection;
class WsConnection;

struct Session {
    Session(asio::io_context& io, std::string identifier, std::string series)
        : id(std::move(identifier)), seriesId(std::move(series)), timer(io) {}

    std::string id;
    std::string seriesId;
    Lifecycle state = Lifecycle::Created;
    std::atomic<bool> closing{false};
    std::unique_ptr<VolumeRenderer> renderer;
    Json volumeInfo = Json::object();
    asio::steady_timer timer;
    bool socketReserved = false;
    std::weak_ptr<WsConnection> socket;
    bool renderBusy = false;
    bool hasSequence = false;
    std::uint64_t latestSeq = 0;
    std::uint64_t latestRevision = 0;
    std::optional<RenderRequest> pending;
    std::shared_ptr<Frame> finalFrame;
};

class Server : public std::enable_shared_from_this<Server> {
public:
    Server(asio::io_context& io, asio::thread_pool& worker, Config config,
           std::unique_ptr<DicomCatalog> catalog, std::vector<SeriesInfo> series)
        : io_(io), worker_(worker), config_(std::move(config)),
          catalog_(std::move(catalog)), series_(std::move(series)), acceptor_(io),
          signals_(io, SIGINT, SIGTERM), guard_(asio::make_work_guard(io)) {}

    void Start();
    void Shutdown();
    void Remove(const std::shared_ptr<HttpConnection>& connection);
    bool Trusted(const http::request<http::string_body>& request) const;
    std::string AllowedOrigin(const http::request<http::string_body>& request) const;
    void Route(const std::shared_ptr<HttpConnection>& connection,
               http::request<http::string_body> request);
    void Disconnected(const std::shared_ptr<Session>& session, const WsConnection* socket);
    void Submit(const std::shared_ptr<Session>& session, RenderRequest request);
    bool Stopping() const { return stopping_; }

private:
    void Accept();
    void BeginLoad(const std::shared_ptr<Session>& session);
    void Loaded(const std::shared_ptr<Session>& session, std::optional<Json> info);
    void Close(const std::shared_ptr<Session>& session);
    void Closed(const std::shared_ptr<Session>& session);
    void ArmDisconnect(const std::shared_ptr<Session>& session);
    void Dispatch(const std::shared_ptr<Session>& session, RenderRequest request);
    void Rendered(const std::shared_ptr<Session>& session, const RenderRequest& request,
                  std::shared_ptr<Frame> frame, const std::weak_ptr<WsConnection>& receiver);
    Json Status(const Session& session) const;
    void ServeStatic(const std::shared_ptr<HttpConnection>& connection, const Target& target,
                     const http::request<http::string_body>& request);

    asio::io_context& io_;
    asio::thread_pool& worker_;
    Config config_;
    std::unique_ptr<DicomCatalog> catalog_;
    std::vector<SeriesInfo> series_;
    Tcp::acceptor acceptor_;
    asio::signal_set signals_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::random_device randomId_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::shared_ptr<Session> active_;
    std::set<std::shared_ptr<HttpConnection>> connections_;
    bool gpuReady_ = false;
    bool stopping_ = false;
};

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    HttpConnection(Tcp::socket socket, std::shared_ptr<Server> server)
        : stream_(std::move(socket)), server_(std::move(server)) {}

    void Start() { Read(); }
    void Stop() {
        if (stopped_) return;
        stopped_ = true;
        ErrorCode ignored;
        stream_.socket().cancel(ignored);
        stream_.socket().shutdown(Tcp::socket::shutdown_both, ignored);
        stream_.socket().close(ignored);
        server_->Remove(shared_from_this());
    }

    beast::tcp_stream Release() {
        stopped_ = true;
        server_->Remove(shared_from_this());
        return std::move(stream_);
    }

    template<class Body>
    void Write(http::response<Body> response, std::shared_ptr<Frame> frame = {}) {
        if (stopped_) return;
        stream_.expires_after(std::chrono::seconds(30));
        const bool keepAlive = response.keep_alive();
        auto message = std::make_shared<http::response<Body>>(std::move(response));
        http::async_write(stream_, *message,
            [self = shared_from_this(), message, frame = std::move(frame), keepAlive]
            (ErrorCode error, std::size_t) {
                if (error || !keepAlive || self->server_->Stopping()) {
                    self->Stop();
                } else {
                    self->Read();
                }
            });
    }

    void JsonReply(http::status status, Json body,
                   const http::request<http::string_body>& request) {
        http::response<http::string_body> response{status, request.version()};
        Headers(response, request);
        response.set(http::field::content_type, "application/json; charset=utf-8");
        response.body() = body.dump(-1, ' ', false, Json::error_handler_t::replace);
        response.prepare_payload();
        Write(std::move(response));
    }

    void Error(http::status status, const char* code, const char* message,
               const http::request<http::string_body>& request) {
        JsonReply(status, ErrorJson(code, message), request);
    }

    template<class Body>
    void Headers(http::response<Body>& response,
                 const http::request<http::string_body>& request) const {
        response.keep_alive(request.keep_alive());
        response.set(http::field::server, "MedGL/1");
        response.set(http::field::cache_control, "no-store");
        response.set("X-Content-Type-Options", "nosniff");
        const auto origin = server_->AllowedOrigin(request);
        if (!origin.empty()) {
            response.set(http::field::access_control_allow_origin, origin);
            response.set(http::field::vary, "Origin");
        }
    }

private:
    void Read() {
        if (stopped_) return;
        parser_.emplace();
        parser_->body_limit(JsonLimit);
        parser_->header_limit(16 * 1024);
        stream_.expires_after(std::chrono::seconds(30));
        http::async_read(stream_, buffer_, *parser_,
            [self = shared_from_this()](ErrorCode error, std::size_t) {
                if (self->stopped_) return;
                if (error == http::error::body_limit || error == http::error::header_limit) {
                    http::request<http::string_body> request{http::verb::get, "/", 11};
                    request.keep_alive(false);
                    self->Error(http::status::payload_too_large, "request_too_large",
                                "Request exceeds the allowed size.", request);
                } else if (error) {
                    self->Stop();
                } else {
                    self->server_->Route(self, self->parser_->release());
                }
            });
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_{JsonLimit + 16 * 1024};
    std::optional<http::request_parser<http::string_body>> parser_;
    std::shared_ptr<Server> server_;
    bool stopped_ = false;
};

struct Outgoing {
    bool binary = false;
    std::shared_ptr<Frame> frame;
    std::string text;
};

class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    WsConnection(beast::tcp_stream stream, std::shared_ptr<Server> server,
                 std::shared_ptr<Session> session)
        : socket_(std::move(stream)), server_(std::move(server)), session_(std::move(session)) {}

    void Accept(http::request<http::string_body> request) {
        socket_.next_layer().expires_never();
        auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::server);
        timeout.idle_timeout = std::chrono::seconds(60);
        timeout.keep_alive_pings = true;
        socket_.set_option(timeout);
        socket_.read_message_max(JsonLimit);
        socket_.auto_fragment(true);
        socket_.write_buffer_bytes(16 * 1024);
        socket_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& response) {
                response.set(http::field::server, "MedGL/1");
            }));
        auto handshake = std::make_shared<http::request<http::string_body>>(std::move(request));
        socket_.async_accept(*handshake,
            [self = shared_from_this(), handshake](ErrorCode error) {
                if (error || self->session_->closing.load()) {
                    self->Stop();
                } else {
                    self->Read();
                }
            });
    }

    void Stop() {
        if (stopped_) return;
        stopped_ = true;
        pending_.reset();
        ErrorCode ignored;
        socket_.next_layer().socket().cancel(ignored);
        socket_.next_layer().socket().shutdown(Tcp::socket::shutdown_both, ignored);
        socket_.next_layer().socket().close(ignored);
        server_->Disconnected(session_, this);
    }

    void SendFrame(std::shared_ptr<Frame> frame) {
        auto message = std::make_shared<Outgoing>();
        message->binary = true;
        message->frame = std::move(frame);
        Queue(std::move(message));
    }

    void SendError(const char* code, const char* message) {
        auto outgoing = std::make_shared<Outgoing>();
        outgoing->text = ErrorJson(code, message).dump();
        Queue(std::move(outgoing));
    }

    void DiscardPendingFrame() {
        if (pending_ && pending_->binary) pending_.reset();
    }

private:
    void Queue(std::shared_ptr<Outgoing> message) {
        if (stopped_) return;
        if (writing_) {
            pending_ = std::move(message);
        } else {
            writing_ = std::move(message);
            Write();
        }
    }

    void Write() {
        if (writing_->binary &&
            (writing_->frame->seq != session_->latestSeq ||
             writing_->frame->revision != session_->latestRevision)) {
            writing_.reset();
            return;
        }
        socket_.binary(writing_->binary);
        const auto buffer = writing_->binary ? asio::buffer(writing_->frame->wire) :
                                               asio::buffer(writing_->text);
        socket_.async_write(buffer,
            [self = shared_from_this()](ErrorCode error, std::size_t) {
                self->writing_.reset();
                if (error || self->stopped_) {
                    self->Stop();
                } else if (self->pending_) {
                    self->writing_ = std::move(self->pending_);
                    self->Write();
                }
            });
    }

    void Read() {
        socket_.async_read(buffer_,
            [self = shared_from_this()](ErrorCode error, std::size_t) {
                if (error || self->stopped_) {
                    self->Stop();
                    return;
                }
                try {
                    if (!self->socket_.got_text()) {
                        throw InputError("Render requests must be JSON text messages.");
                    }
                    const auto text = beast::buffers_to_string(self->buffer_.data());
                    const auto message = ParseJson(text);
                    auto request = ParseRender(message);
                    if (self->session_->state != Lifecycle::Ready || self->session_->closing.load()) {
                        self->SendError("session_not_ready", "The session is not ready to render.");
                    } else {
                        self->server_->Submit(self->session_, std::move(request));
                    }
                } catch (const InputError& errorInput) {
                    self->SendError("invalid_render", errorInput.what());
                } catch (const Json::exception&) {
                    self->SendError("invalid_render", "Malformed JSON render request.");
                } catch (...) {
                    self->SendError("internal_error", "The request could not be processed.");
                }
                self->buffer_.consume(self->buffer_.size());
                if (!self->stopped_) self->Read();
            });
    }

    websocket::stream<beast::tcp_stream> socket_;
    beast::flat_buffer buffer_{JsonLimit};
    std::shared_ptr<Server> server_;
    std::shared_ptr<Session> session_;
    std::shared_ptr<Outgoing> writing_;
    std::shared_ptr<Outgoing> pending_;
    bool stopped_ = false;
};

void Server::Start() {
    const Tcp::endpoint endpoint{asio::ip::make_address_v4("127.0.0.1"), config_.port};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(asio::socket_base::max_listen_connections);
    signals_.async_wait([self = shared_from_this()](ErrorCode error, int) {
        if (!error) self->Shutdown();
    });
    Accept();
    std::cout << "MedGL service listening at http://127.0.0.1:" << config_.port << '\n';
}

void Server::Accept() {
    acceptor_.async_accept([self = shared_from_this()](ErrorCode error, Tcp::socket socket) {
        if (self->stopping_) return;
        if (!error) {
            if (self->connections_.size() >= ConnectionLimit) {
                ErrorCode ignored;
                socket.close(ignored);
            } else {
                auto connection = std::make_shared<HttpConnection>(std::move(socket), self);
                self->connections_.insert(connection);
                connection->Start();
            }
        } else if (error != asio::error::operation_aborted) {
            std::cerr << "Network accept failed; stopping service.\n";
            self->Shutdown();
            return;
        }
        if (!self->stopping_) self->Accept();
    });
}

void Server::Remove(const std::shared_ptr<HttpConnection>& connection) {
    connections_.erase(connection);
}

std::string Server::AllowedOrigin(const http::request<http::string_body>& request) const {
    const auto origin = request[http::field::origin];
    const auto port = std::to_string(config_.port);
    for (const auto& allowed : {"http://localhost:" + port, "http://127.0.0.1:" + port,
                               std::string("http://localhost:5173"),
                               std::string("http://127.0.0.1:5173")}) {
        if (origin == allowed) return allowed;
    }
    return {};
}

bool Server::Trusted(const http::request<http::string_body>& request) const {
    const auto host = request[http::field::host];
    const auto port = std::to_string(config_.port);
    if (!beast::iequals(host, "localhost:" + port) &&
        !beast::iequals(host, "127.0.0.1:" + port)) {
        return false;
    }
    if (request.count(http::field::host) != 1 || request.count(http::field::origin) > 1) {
        return false;
    }
    if (request.count(http::field::origin) != 0) return !AllowedOrigin(request).empty();
    return request["Sec-Fetch-Site"] != "cross-site";
}

Json Server::Status(const Session& session) const {
    const auto base = "/api/v1/sessions/" + session.id;
    Json result{{"id", session.id}, {"seriesId", session.seriesId},
                {"sessionId", session.id},
                {"state", StateName(session.state)}, {"protocolVersion", 1},
                {"statusUrl", base}, {"wsUrl", base + "/stream"}, {"frameUrl", base + "/frame"}};
    if (session.state == Lifecycle::Ready) result.update(session.volumeInfo);
    if (session.state == Lifecycle::Ready) {
        result["sceneRevision"] = std::max<std::uint64_t>(1, session.latestRevision);
    }
    if (session.state == Lifecycle::Failed) {
        result["error"] = {{"code", "load_failed"}, {"message", "Volume could not be loaded."}};
    }
    return result;
}

void Server::Route(const std::shared_ptr<HttpConnection>& connection,
                   http::request<http::string_body> request) {
    try {
        if (!Trusted(request)) {
            connection->Error(http::status::forbidden, "untrusted_request",
                              "Host or Origin is not allowed.", request);
            return;
        }
        if (stopping_) {
            connection->Error(http::status::service_unavailable, "service_stopping",
                              "The service is stopping.", request);
            return;
        }
        const auto target = ParseTarget(request.target());
        const auto parts = PathParts(target.path);
        const bool api = parts.size() >= 2 && parts[0] == "api" && parts[1] == "v1";
        if (request.method() == http::verb::options && api) {
            const auto method = request[http::field::access_control_request_method];
            const auto headers = request[http::field::access_control_request_headers];
            if (AllowedOrigin(request).empty() ||
                (method != "GET" && method != "POST" && method != "DELETE") ||
                (!headers.empty() && !beast::iequals(headers, "content-type"))) {
                connection->Error(http::status::forbidden, "untrusted_request",
                                  "Preflight request is not allowed.", request);
                return;
            }
            http::response<http::empty_body> response{http::status::no_content, request.version()};
            connection->Headers(response, request);
            response.set(http::field::access_control_allow_methods, "GET, POST, DELETE");
            response.set(http::field::access_control_allow_headers, "Content-Type");
            response.prepare_payload();
            connection->Write(std::move(response));
            return;
        }
        if (target.path == "/api/v1/health" && request.method() == http::verb::get) {
            connection->JsonReply(http::status::ok,
                {{"state", "serviceReady"}, {"serviceReady", true}, {"ready", gpuReady_},
                 {"gpuReady", gpuReady_}, {"gpuName", nullptr}, {"protocolVersion", 1},
                 {"pngCompression", config_.pngCompression == PngCompression::Fast ? "fast" : "default"}}, request);
            return;
        }
        if (target.path == "/api/v1/presets" && request.method() == http::verb::get) {
            connection->JsonReply(http::status::ok, PresetCatalogJson(), request);
            return;
        }
        if (target.path == "/api/v1/series" && request.method() == http::verb::get) {
            Json items = Json::array({{{"id", "phantom"}, {"description", "Synthetic phantom"},
                                      {"fileCount", 0}, {"supported", true}, {"reason", ""}}});
            for (const auto& series : series_) {
                if (series.id == "phantom") continue;
                items.push_back({{"id", series.id}, {"description", series.description},
                                 {"fileCount", series.fileCount}, {"supported", series.supported},
                                 {"reason", series.reason}, {"warning", series.warning}});
            }
            connection->JsonReply(http::status::ok, {{"series", std::move(items)}}, request);
            return;
        }
        if (target.path == "/api/v1/sessions" && request.method() == http::verb::post) {
            const auto contentType = request[http::field::content_type];
            const auto separator = contentType.find(';');
            if (!beast::iequals(contentType.substr(0, separator), "application/json")) {
                connection->Error(http::status::unsupported_media_type, "json_required",
                                  "Content-Type must be application/json.", request);
                return;
            }
            const auto body = ParseJson(request.body());
            const auto seriesId = Text(body, "seriesId");
            if (seriesId != "phantom") {
                const auto found = std::find_if(series_.begin(), series_.end(),
                    [&seriesId](const SeriesInfo& series) { return series.id == seriesId; });
                if (found == series_.end()) {
                    connection->Error(http::status::not_found, "series_not_found",
                                      "The requested series does not exist.", request);
                    return;
                }
                if (!found->supported) {
                    connection->Error(http::status::unprocessable_entity, "series_unsupported",
                                      "The requested series is not supported.", request);
                    return;
                }
            }
            if (active_ || sessions_.size() >= SessionLimit) {
                connection->Error(http::status::conflict, "session_conflict",
                                  "The session slot is occupied or retention capacity is full.", request);
                return;
            }
            std::string id;
            id.reserve(32);
            constexpr char digits[] = "0123456789abcdef";
            for (unsigned int index = 0; index < 4; ++index) {
                const auto word = static_cast<std::uint32_t>(randomId_());
                for (unsigned int shift = 0; shift < 32; shift += 4) {
                    id.push_back(digits[(word >> shift) & 15U]);
                }
            }
            auto session = std::make_shared<Session>(io_, id, seriesId);
            if (!sessions_.emplace(id, session).second) {
                throw std::runtime_error("Session identifier collision.");
            }
            active_ = session;
            BeginLoad(session);
            connection->JsonReply(http::status::accepted, Status(*session), request);
            return;
        }
        if (api && parts.size() >= 4 && parts[2] == "sessions") {
            const auto found = sessions_.find(parts[3]);
            if (found == sessions_.end()) {
                connection->Error(http::status::not_found, "session_not_found",
                                  "The requested session does not exist.", request);
                return;
            }
            const auto session = found->second;
            if (parts.size() == 4 && request.method() == http::verb::get) {
                connection->JsonReply(http::status::ok, Status(*session), request);
                return;
            }
            if (parts.size() == 4 && request.method() == http::verb::delete_) {
                Close(session);
                connection->JsonReply(http::status::accepted, Status(*session), request);
                return;
            }
            if (parts.size() == 5 && (parts[4] == "stream" || parts[4] == "ws") &&
                request.method() == http::verb::get && websocket::is_upgrade(request)) {
                if (session->closing.load()) {
                    connection->Error(http::status::gone, "session_closed",
                                      "The session is closing or closed.", request);
                } else if (session->state == Lifecycle::Failed) {
                    connection->Error(http::status::conflict, "session_failed",
                                      "The session failed to load.", request);
                } else if (session->socketReserved) {
                    connection->Error(http::status::conflict, "socket_conflict",
                                      "The session already has a WebSocket connection.", request);
                } else {
                    session->socketReserved = true;
                    session->timer.cancel();
                    auto socket = std::make_shared<WsConnection>(connection->Release(),
                                                                shared_from_this(), session);
                    session->socket = socket;
                    socket->Accept(std::move(request));
                }
                return;
            }
            if (parts.size() == 5 && parts[4] == "frame" && request.method() == http::verb::get) {
                if (target.query.size() != 2 || target.query.count("seq") != 1 ||
                    target.query.count("revision") != 1) {
                    throw InputError("Frame requests require seq and revision parameters.");
                }
                const auto seq = Decimal(target.query.at("seq"), SafeIntegerLimit);
                const auto revision = Decimal(target.query.at("revision"), SafeIntegerLimit);
                if (session->closing.load() ||
                    (session->hasSequence &&
                     (seq < session->latestSeq || revision < session->latestRevision))) {
                    connection->Error(http::status::gone, "frame_expired",
                                      "The requested frame has expired.", request);
                } else if (!session->finalFrame || session->finalFrame->seq != seq ||
                           session->finalFrame->revision != revision) {
                    connection->Error(http::status::conflict, "frame_pending",
                                      "The requested final frame is not available.", request);
                } else {
                    const auto frame = session->finalFrame;
                    http::response<http::buffer_body> response{http::status::ok, request.version()};
                    connection->Headers(response, request);
                    response.set(http::field::content_type, "image/png");
                    response.set("X-MedGL-Seq", std::to_string(seq));
                    response.set("X-MedGL-Revision", std::to_string(revision));
                    response.body().data = frame->wire.data() + frame->payloadOffset;
                    response.body().size = frame->wire.size() - frame->payloadOffset;
                    response.body().more = false;
                    response.content_length(response.body().size);
                    connection->Write(std::move(response), frame);
                }
                return;
            }
        }
        if (api || websocket::is_upgrade(request)) {
            connection->Error(http::status::not_found, "route_not_found",
                              "The requested API route does not exist.", request);
        } else if (request.method() != http::verb::get && request.method() != http::verb::head) {
            connection->Error(http::status::method_not_allowed, "method_not_allowed",
                              "This resource only accepts GET or HEAD.", request);
        } else {
            ServeStatic(connection, target, request);
        }
    } catch (const InputError& error) {
        connection->Error(http::status::bad_request, "invalid_request", error.what(), request);
    } catch (const Json::exception&) {
        connection->Error(http::status::bad_request, "invalid_json",
                          "Malformed JSON request.", request);
    } catch (...) {
        connection->Error(http::status::internal_server_error, "internal_error",
                          "The request could not be processed.", request);
    }
}

void Server::ServeStatic(const std::shared_ptr<HttpConnection>& connection, const Target& target,
                         const http::request<http::string_body>& request) {
    std::error_code error;
    const auto root = fs::canonical(config_.webRoot, error);
    if (error) {
        connection->Error(http::status::not_found, "static_not_found",
                          "The requested static file does not exist.", request);
        return;
    }
    auto relative = fs::u8path(target.path == "/" ? "index.html" : target.path.substr(1));
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        throw InputError("Invalid static-file path.");
    }
    const auto resolved = fs::canonical(root / relative, error);
    if (error || !WithinRoot(root, resolved) || !fs::is_regular_file(resolved, error) || error) {
        connection->Error(http::status::not_found, "static_not_found",
                          "The requested static file does not exist.", request);
        return;
    }
    const auto size = fs::file_size(resolved, error);
    if (error || size > StaticFileLimit) {
        connection->Error(http::status::not_found, "static_not_found",
                          "The requested static file cannot be served.", request);
        return;
    }
    if (request.method() == http::verb::head) {
        http::response<http::empty_body> response{http::status::ok, request.version()};
        connection->Headers(response, request);
        response.set(http::field::content_type, MimeType(resolved));
        response.content_length(size);
        connection->Write(std::move(response));
        return;
    }
    http::response<http::file_body> response{http::status::ok, request.version()};
    ErrorCode openError;
    response.body().open(resolved.u8string().c_str(), beast::file_mode::scan, openError);
    if (openError || response.body().size() > StaticFileLimit) {
        connection->Error(http::status::not_found, "static_not_found",
                          "The requested static file cannot be served.", request);
        return;
    }
    connection->Headers(response, request);
    response.set(http::field::content_type, MimeType(resolved));
    response.content_length(response.body().size());
    connection->Write(std::move(response));
}

void Server::BeginLoad(const std::shared_ptr<Session>& session) {
    session->state = Lifecycle::Loading;
    const auto acceptedAt = Clock::now();
    asio::post(worker_, [self = shared_from_this(), session, acceptedAt] {
        std::optional<Json> info;
        const auto workerStart = Clock::now();
        const double queueMs = ElapsedMs(acceptedAt);
        try {
            if (!session->closing.load()) {
                auto volume = session->seriesId == "phantom" ? MakePhantom() :
                                                              self->catalog_->Load(session->seriesId);
                if (!session->closing.load()) {
                    if (volume.voxels.size() > 256ULL * 1024 * 1024) {
                        throw InputError("Volume exceeds the configured memory limit.");
                    }
                    const auto histogram = VolumeHistogramJson(volume);
                    const double dataLoadMs = ElapsedMs(workerStart);
                    auto initial = FitView(volume);
                    ApplyPreset(initial, "ct-bone");
                    session->renderer = std::make_unique<VolumeRenderer>();
                    if (!session->closing.load()) {
                        session->renderer->Load(volume);
                        if (!session->closing.load()) {
                            const auto upload = session->renderer->LastLoadTimings();
                            info = {{"shape", {volume.shape.width, volume.shape.height, volume.shape.depth}},
                                    {"spacingMm", volume.spacing}, {"originMm", volume.origin},
                                    {"direction", volume.direction}, {"initialCamera", CameraJson(initial)},
                                    {"initialStyle", StyleJson(initial)},
                                    {"volume", {{"dimensions", {volume.shape.width, volume.shape.height,
                                                                  volume.shape.depth}},
                                                {"spacing", volume.spacing}}},
                                    {"standardViews", StandardViewsJson()},
                                    {"cropBoxSupported", true},
                                    {"clipPlaneSupported", true},
                                    {"transferFunctionSupported", true}, {"histogram", histogram},
                                    {"loadTimings", {{"queueMs", queueMs}, {"dataLoadMs", dataLoadMs},
                                        {"validationMs", upload.validationMs}, {"uploadMs", upload.uploadMs},
                                        {"workerMs", ElapsedMs(workerStart)}}},
                                    {"initial", {{"camera", CameraJson(initial)},
                                                 {"style", StyleJson(initial)}, {"cropBox", CropBoxJson(initial)},
                                                 {"clipPlane", ClipPlaneJson(initial)}}}};
                        }
                    }
                }
            }
        } catch (...) {
            info.reset();
            if (session->renderer) {
                try { session->renderer->Unload(); } catch (...) {}
                session->renderer.reset();
            }
        }
        asio::post(self->io_, [self, session, info = std::move(info)]() mutable {
            self->Loaded(session, std::move(info));
        });
    });
}

void Server::Loaded(const std::shared_ptr<Session>& session, std::optional<Json> info) {
    if (session->closing.load()) return;
    if (info) {
        session->volumeInfo = std::move(*info);
        session->state = Lifecycle::Ready;
        gpuReady_ = true;
    } else {
        session->state = Lifecycle::Failed;
        if (auto socket = session->socket.lock()) {
            socket->SendError("load_failed", "Volume could not be loaded.");
        }
    }
    if (!session->socketReserved) ArmDisconnect(session);
}

void Server::ArmDisconnect(const std::shared_ptr<Session>& session) {
    if (session->closing.load() || stopping_) return;
    session->timer.expires_after(std::chrono::seconds(30));
    session->timer.async_wait([self = shared_from_this(), session](ErrorCode error) {
        if (!error && !session->socketReserved) self->Close(session);
    });
}

void Server::Disconnected(const std::shared_ptr<Session>& session, const WsConnection* socket) {
    const auto current = session->socket.lock();
    if (current && current.get() != socket) return;
    session->socket.reset();
    session->socketReserved = false;
    session->pending.reset();
    ArmDisconnect(session);
}

void Server::Submit(const std::shared_ptr<Session>& session, RenderRequest request) {
    if (request.revision != 1 || (session->hasSequence && request.seq <= session->latestSeq)) {
        throw InputError("seq must increase and sceneRevision must match the current scene.");
    }
    session->hasSequence = true;
    request.receivedAt = Clock::now();
    session->latestSeq = request.seq;
    session->latestRevision = request.revision;
    session->finalFrame.reset();
    if (auto socket = session->socket.lock()) socket->DiscardPendingFrame();
    if (session->renderBusy) {
        session->pending = std::move(request);
    } else {
        Dispatch(session, std::move(request));
    }
}

std::shared_ptr<Frame> EncodeFrame(const std::string& id, const RenderRequest& request,
                                 const RgbImage& image, RenderTimings timings = {}, double queueMs = 0,
                                 PngCompression compression = PngCompression::Default) {
    image.Validate();
    if (image.width != request.settings.width || image.height != request.settings.height ||
        image.pixels.size() > ImageLimit) {
        throw InputError("Renderer returned an invalid image.");
    }
    const auto encodeStart = Clock::now();
    auto payload = request.finalQuality ? EncodePng(image, compression) : EncodeJpeg(image, 85);
    const double encodeMs = ElapsedMs(encodeStart);
    if (payload.empty() || payload.size() > ImageLimit) {
        throw InputError("Encoded image exceeds its allowed size.");
    }
    const Json metadata{{"protocolVersion", 1}, {"sessionId", id}, {"seq", request.seq},
                        {"sceneRevision", request.revision}, {"revision", request.revision},
                        {"width", image.width}, {"height", image.height},
                        {"encoding", request.finalQuality ? "png" : "jpeg"},
                        {"pngCompression", request.finalQuality
                            ? Json(compression == PngCompression::Fast ? "fast" : "default") : Json(nullptr)},
                        {"payloadBytes", payload.size()},
                        {"camera", CameraJson(request.settings)},
                        {"cropBox", CropBoxJson(request.settings)},
                        {"clipPlane", ClipPlaneJson(request.settings)},
                        {"transferFunction", TransferJson(request.settings.transfer)},
                        {"timings", {{"prepareMs", timings.prepareMs}, {"kernelMs", timings.kernelMs},
                            {"kernelWallMs", timings.kernelWallMs}, {"readbackMs", timings.readbackMs},
                            {"totalMs", timings.totalMs}, {"encodeMs", encodeMs}, {"queueMs", queueMs}}},
                        {"quality", request.finalQuality ? "final" : "preview"}};
    const auto header = metadata.dump();
    if (header.size() > JsonLimit) throw InputError("Frame metadata exceeds its allowed size.");
    auto frame = std::make_shared<Frame>();
    frame->seq = request.seq;
    frame->revision = request.revision;
    frame->payloadOffset = 4 + header.size();
    frame->wire.reserve(frame->payloadOffset + payload.size());
    const auto length = static_cast<std::uint32_t>(header.size());
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        frame->wire.push_back(static_cast<std::uint8_t>((length >> shift) & 0xffU));
    }
    frame->wire.insert(frame->wire.end(), header.begin(), header.end());
    frame->wire.insert(frame->wire.end(), payload.begin(), payload.end());
    return frame;
}

void Server::Dispatch(const std::shared_ptr<Session>& session, RenderRequest request) {
    session->renderBusy = true;
    const auto receiver = session->socket;
    asio::post(worker_, [self = shared_from_this(), session, request = std::move(request), receiver] {
        std::shared_ptr<Frame> frame;
        try {
            if (!session->closing.load() && session->renderer) {
                const double queueMs = ElapsedMs(request.receivedAt);
                const auto image = session->renderer->Render(request.settings);
                if (!session->closing.load()) frame = EncodeFrame(session->id, request, image,
                    session->renderer->LastRenderTimings(), queueMs, self->config_.pngCompression);
            }
        } catch (...) {}
        asio::post(self->io_, [self, session, request, frame = std::move(frame), receiver]() mutable {
            self->Rendered(session, request, std::move(frame), receiver);
        });
    });
}

void Server::Rendered(const std::shared_ptr<Session>& session, const RenderRequest& request,
                      std::shared_ptr<Frame> frame, const std::weak_ptr<WsConnection>& receiver) {
    session->renderBusy = false;
    if (session->closing.load()) return;
    if (request.seq == session->latestSeq && request.revision == session->latestRevision) {
        if (frame && request.finalQuality) session->finalFrame = frame;
        const auto socket = receiver.lock();
        if (socket && socket == session->socket.lock()) {
            if (frame) socket->SendFrame(std::move(frame));
            else socket->SendError("render_failed", "The requested frame could not be rendered.");
        }
    }
    if (session->pending) {
        auto next = std::move(*session->pending);
        session->pending.reset();
        Dispatch(session, std::move(next));
    }
}

void Server::Close(const std::shared_ptr<Session>& session) {
    if (session->closing.exchange(true)) return;
    session->state = Lifecycle::Closing;
    session->pending.reset();
    session->finalFrame.reset();
    session->timer.cancel();
    if (auto socket = session->socket.lock()) socket->Stop();
    asio::post(worker_, [self = shared_from_this(), session] {
        if (session->renderer) {
            try { session->renderer->Unload(); } catch (...) {}
            session->renderer.reset();
        }
        asio::post(self->io_, [self, session] { self->Closed(session); });
    });
}

void Server::Closed(const std::shared_ptr<Session>& session) {
    session->state = Lifecycle::Closed;
    session->volumeInfo = Json::object();
    if (active_ == session) active_.reset();
    if (stopping_) return;
    session->timer.expires_after(std::chrono::seconds(30));
    session->timer.async_wait([self = shared_from_this(), session](ErrorCode error) {
        if (!error) self->sessions_.erase(session->id);
    });
}

void Server::Shutdown() {
    if (stopping_) return;
    stopping_ = true;
    ErrorCode ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    signals_.cancel(ignored);
    while (!connections_.empty()) (*connections_.begin())->Stop();
    for (const auto& entry : sessions_) {
        entry.second->timer.cancel();
        Close(entry.second);
    }
    asio::post(worker_, [self = shared_from_this()] {
        asio::post(self->io_, [self] {
            self->guard_.reset();
            self->io_.stop();
        });
    });
}

}

int main(int argumentCount, char** arguments) {
    try {
        auto config = medgl::server::ParseConfig(argumentCount, arguments);
        if (config.fixtureRoot) medgl::WriteSyntheticCt(*config.fixtureRoot);
        auto catalog = std::make_unique<medgl::DicomCatalog>(
            config.fixtureRoot ? *config.fixtureRoot : config.dataRoot,
            medgl::DicomReadOptions{config.allowInstanceUidMismatch});
        if (config.allowInstanceUidMismatch)
            std::cout << "WARNING: instance UID compatibility is enabled; original DICOM files remain unchanged.\n";
        auto series = catalog->Scan();
        boost::asio::io_context io(1);
        boost::asio::thread_pool worker(1);
        auto server = std::make_shared<medgl::server::Server>(
            io, worker, std::move(config), std::move(catalog), std::move(series));
        server->Start();
        int exitCode = EXIT_SUCCESS;
        for (;;) {
            try {
                io.run();
                break;
            } catch (...) {
                std::cerr << "Request processing failed; draining service shutdown.\n";
                exitCode = EXIT_FAILURE;
                server->Shutdown();
                io.restart();
            }
        }
        worker.join();
        std::cout << "MedGL service stopped.\n";
        return exitCode;
    } catch (const medgl::server::InputError& error) {
        std::cerr << error.what() << '\n';
    } catch (...) {
        std::cerr << "MedGL startup failed. Check configuration and native dependencies.\n";
    }
    return EXIT_FAILURE;
}