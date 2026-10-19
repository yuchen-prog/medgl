#ifdef MEDGL_SERVER_SELF_TEST

#define main MedglServerEntrypoint
#include "main.cpp"
#undef main

#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

std::thread::id IoThread;
std::thread::id WorkerThread;
std::atomic<unsigned int> Violations{0};
std::atomic<unsigned int> Constructions{0};
std::atomic<unsigned int> Loads{0};
std::atomic<unsigned int> Renders{0};
std::atomic<unsigned int> Destructions{0};
std::mutex GateMutex;
std::condition_variable GateCondition;
bool DecodeStarted = false;
bool ReleaseDecode = false;
bool RenderStarted = false;
bool ReleaseRender = false;

void WorkerCall() {
    const auto thread = std::this_thread::get_id();
    if (thread == IoThread || (WorkerThread != std::thread::id{} && thread != WorkerThread)) {
        ++Violations;
    }
    WorkerThread = thread;
}

void Check(bool condition) {
    if (!condition) throw std::runtime_error("Server self-test assertion failed.");
}

struct ReleaseGates {
    ~ReleaseGates() {
        {
            std::lock_guard<std::mutex> lock(GateMutex);
            ReleaseDecode = true;
            ReleaseRender = true;
        }
        GateCondition.notify_all();
    }
};

template<class Function>
void Rejected(Function function) {
    bool rejected = false;
    try { function(); } catch (const medgl::server::InputError&) { rejected = true; }
    Check(rejected);
}

medgl::VolumeData SmallVolume() {
    medgl::VolumeData volume;
    volume.shape = {2, 2, 2};
    volume.voxels.assign(8, 1);
    return volume;
}

medgl::server::Json Request(std::uint64_t seq, const char* quality = "final") {
    return {{"seq", seq}, {"sceneRevision", 1}, {"quality", quality},
            {"width", 16}, {"height", 16},
            {"camera", {{"targetMm", {0, 0, 0}}, {"rotationXyzw", {0, 0, 0, 1}},
                        {"orthoHeightMm", 10}}},
            {"style", {{"presetId", "ct-bone"}, {"windowWidth", 1000},
                       {"windowLevel", 400}, {"opacityScale", 1}, {"backgroundRgb", {0, 0, 0}}}}};
}

}

namespace medgl {

struct VolumeRenderer::Impl {};
VolumeRenderer::VolumeRenderer() : m_impl(std::make_unique<Impl>()) {
    WorkerCall();
    ++Constructions;
}
VolumeRenderer::~VolumeRenderer() { WorkerCall(); ++Destructions; }
void VolumeRenderer::Load(const VolumeData&) { WorkerCall(); ++Loads; }
void VolumeRenderer::Unload() { WorkerCall(); }
RenderTimings VolumeRenderer::LastRenderTimings() const { WorkerCall(); return {1, 2, 3, 4, 8}; }
LoadTimings VolumeRenderer::LastLoadTimings() const { WorkerCall(); return {1, 2, 3}; }
RgbImage VolumeRenderer::Render(const RenderSettings& settings) {
    WorkerCall();
    const auto count = ++Renders;
    if (count == 1) {
        std::unique_lock<std::mutex> lock(GateMutex);
        RenderStarted = true;
        GateCondition.notify_all();
        GateCondition.wait(lock, [] { return ReleaseRender; });
    }
    return {settings.width, settings.height,
            std::vector<std::uint8_t>(static_cast<std::size_t>(settings.width) * settings.height * 3, 42)};
}
VolumeData MakePhantom() { WorkerCall(); return SmallVolume(); }
RenderSettings FitView(const VolumeData&) { WorkerCall(); return {}; }

struct DicomCatalog::Impl {};
DicomCatalog::DicomCatalog(std::filesystem::path, DicomReadOptions) : m_impl(std::make_unique<Impl>()) {}
DicomCatalog::~DicomCatalog() = default;
std::vector<SeriesInfo> DicomCatalog::Scan() { return {{"slow", "fixture", 1, true, ""}}; }
VolumeData DicomCatalog::Load(const std::string&) const {
    WorkerCall();
    std::unique_lock<std::mutex> lock(GateMutex);
    DecodeStarted = true;
    GateCondition.notify_all();
    GateCondition.wait(lock, [] { return ReleaseDecode; });
    return SmallVolume();
}
void WriteSyntheticCt(const std::filesystem::path&) {}
std::vector<std::uint8_t> EncodePng(const RgbImage&) { WorkerCall(); return {137, 80, 78, 71}; }
std::vector<std::uint8_t> EncodePng(const RgbImage&, PngCompression) { WorkerCall(); return {137, 80, 78, 71}; }
std::vector<std::uint8_t> EncodeJpeg(const RgbImage&, int) { WorkerCall(); return {255, 216, 255, 217}; }

}

namespace {

using namespace medgl::server;

void ProtocolChecks() {
    char program[] = "medgl_server";
    char compatibilityOption[] = "--allow-instance-uid-mismatch";
    char* defaultArguments[] = {program};
    char* compatibilityArguments[] = {program, compatibilityOption};
    Check(!ParseConfig(1, defaultArguments).allowInstanceUidMismatch);
    Check(ParseConfig(2, compatibilityArguments).allowInstanceUidMismatch);
    Check(ParseConfig(1, defaultArguments).pngCompression == medgl::PngCompression::Default);
    char pngOption[] = "--png-compression";
    char fastValue[] = "fast";
    char defaultValue[] = "default";
    char invalidValue[] = "lossy";
    char* fastArguments[] = {program, pngOption, fastValue};
    char* pngDefaultArguments[] = {program, pngOption, defaultValue};
    char* invalidArguments[] = {program, pngOption, invalidValue};
    Check(ParseConfig(3, fastArguments).pngCompression == medgl::PngCompression::Fast);
    Check(ParseConfig(3, pngDefaultArguments).pngCompression == medgl::PngCompression::Default);
    Rejected([&] { ParseConfig(3, invalidArguments); });
    Rejected([&] { ParseConfig(2, fastArguments); });
    const auto parsed = ParseRender(Request(1));
    Check(parsed.seq == 1 && parsed.revision == 1 && parsed.finalQuality);
    Check(!parsed.settings.cropBox.enabled && parsed.settings.cropBox.maximum[2] == 1);
    Check(!parsed.settings.clipPlane.enabled && parsed.settings.clipPlane.normal[2] == 1);
    auto planeRequest = Request(2);
    const Json validPlane{{"enabled", true}, {"normal", {0.6, 0.8, 0}}, {"offsetMm", 12.5}, {"keepPositive", false}};
    planeRequest["clipPlane"] = validPlane;
    Check(ClipPlaneJson(ParseRender(planeRequest).settings).at("offsetMm") == 12.5);
    Check(!ParseRender(planeRequest).settings.clipPlane.keepPositive);
    for (const auto& invalidPlane : std::vector<Json>{nullptr, Json::object(),
        {{"enabled", 1}, {"normal", {0, 0, 1}}, {"offsetMm", 0}, {"keepPositive", true}},
        {{"enabled", false}, {"normal", {0, 0, 0}}, {"offsetMm", 0}, {"keepPositive", true}},
        {{"enabled", false}, {"normal", {0, 0}}, {"offsetMm", 0}, {"keepPositive", true}},
        {{"enabled", true}, {"normal", {1, 1, 1}}, {"offsetMm", 0}, {"keepPositive", true}},
        {{"enabled", true}, {"normal", {0, 0, 1}}, {"offsetMm", 1000001}, {"keepPositive", true}},
        {{"enabled", true}, {"normal", {0, 0, 1}}, {"offsetMm", 0}, {"keepPositive", "yes"}}}) {
        planeRequest["clipPlane"] = invalidPlane;
        Rejected([&] { ParseRender(planeRequest); });
    }
    auto cropRequest = Request(2);
    cropRequest["cropBox"] = {{"enabled", true}, {"minimum", {0.1, 0.2, 0.3}}, {"maximum", {0.8, 0.9, 1.0}}};
    Check(ParseRender(cropRequest).settings.cropBox.enabled);
    Check(std::abs(ParseRender(cropRequest).settings.cropBox.minimum[1] - 0.2f) < 1e-6f);
    for (const auto& invalidCrop : std::vector<Json>{nullptr, Json::object(),
        {{"enabled", 1}, {"minimum", {0, 0, 0}}, {"maximum", {1, 1, 1}}},
        {{"enabled", false}, {"minimum", {0, 0}}, {"maximum", {1, 1, 1}}},
        {{"enabled", false}, {"minimum", {-0.1, 0, 0}}, {"maximum", {1, 1, 1}}},
        {{"enabled", true}, {"minimum", {0, 0, 0}}, {"maximum", {1, 1, 1.1}}},
        {{"enabled", false}, {"minimum", {0, 0.5, 0}}, {"maximum", {1, 0.5, 1}}},
        {{"enabled", true}, {"minimum", {0, 0.9, 0}}, {"maximum", {1, 0.5, 1}}}}) {
        cropRequest["cropBox"] = invalidCrop;
        Rejected([&] { ParseRender(cropRequest); });
    }
    Check(Preset("ct-soft-tissue").size() >= 2 && Preset("ct-lung").size() >= 2);
    Check(AppearancePresets().size() == 8);
    std::set<std::string> presetIds;
    for (const auto& preset : AppearancePresets()) {
        Check(presetIds.insert(preset.id).second);
        Check(preset.windowWidth > 0 && std::isfinite(preset.windowWidth) && std::isfinite(preset.windowLevel));
        Check(preset.opacityScale > 0 && preset.opacityScale <= 3);
        Check(preset.transfer.size() >= 2 && preset.transfer.front().position == 0 && preset.transfer.back().position == 1);
        Check(preset.transfer.front().rgba[3] == 0);
        float previous = -1;
        for (const auto& point : preset.transfer) {
            Check(point.position > previous && point.position <= 1);
            previous = point.position;
            for (float component : point.rgba) Check(std::isfinite(component) && component >= 0 && component <= 1);
        }
        auto request = Request(1);
        request["style"]["presetId"] = preset.id;
        Check(ParseRender(request).settings.transfer.size() == preset.transfer.size());
        medgl::RenderSettings applied;
        applied.opacityScale = 3;
        applied.lighting = false;
        ApplyPreset(applied, preset.id);
        Check(applied.windowWidth == preset.windowWidth && applied.windowLevel == preset.windowLevel
            && applied.opacityScale == preset.opacityScale && applied.lighting == preset.lighting);
    }
    Check(Preset("ct-soft").size() == Preset("ct-soft-tissue").size());
    Check(Preset("ct-lung").back().rgba[3] == 0);
    Check(PresetCatalogJson().at("presets").size() == AppearancePresets().size());
    Rejected([] { FindPreset("nonexistent"); });
    const auto initialStyle = StyleJson(parsed.settings);
    Check(!initialStyle.contains("transferFunction"));
    Check(StandardViewsJson().size() == 6);
    Check(StandardViewsJson()[0].at("id") == "anterior");
    auto message = Request(2, "preview");
    Check(!ParseRender(message).finalQuality);
    message["width"] = ViewportLimit + 1;
    Rejected([&] { ParseRender(message); });
    message = Request(2);
    message["seq"] = SafeIntegerLimit + 1;
    Rejected([&] { ParseRender(message); });
    message["seq"] = -1;
    Rejected([&] { ParseRender(message); });
    message = Request(2);
    message["camera"]["rotationXyzw"] = {0, 0, 0, 0};
    Rejected([&] { ParseRender(message); });
    message = Request(2);
    message["style"]["opacityScale"] = std::numeric_limits<double>::infinity();
    Rejected([&] { ParseRender(message); });
    message = Request(2);
    message["style"]["transferFunction"] = Json::array({
        {{"position", 0.5}, {"rgba", {0, 0, 0, 0}}},
        {{"position", 0.5}, {"rgba", {1, 1, 1, 1}}}});
    Rejected([&] { ParseRender(message); });
    Rejected([] { ParseJson(std::string(JsonLimit + 1, ' ')); });
    Rejected([] { ParseJson(std::string(32, '[') + "0" + std::string(32, ']')); });
    Rejected([] { ParseJson("{"); });
    const auto target = ParseTarget("/api/v1/sessions/abc/ws?label=a%20b");
    Check(target.path == "/api/v1/sessions/abc/ws" && target.query.at("label") == "a b");
    Rejected([] { ParseTarget("/bad%0"); });
    Rejected([] { ParseTarget("/bad%00"); });
    Rejected([] { ParseTarget("/bad%5cpath"); });
    Rejected([] { ParseTarget("/ok?seq=1&seq=2"); });
    Rejected([] { PathParts(ParseTarget("/%2e%2e/secrets").path); });
    Rejected([] { PathParts(ParseTarget("/..%20/secrets").path); });
    Rejected([] { PathParts(ParseTarget("/C%3a/secrets").path); });
    Check(WithinRoot(fs::path("C:/web"), fs::path("C:/web/assets/app.js")));
    Check(!WithinRoot(fs::path("C:/web"), fs::path("C:/web-other/app.js")));
}

Json Invoke(const std::shared_ptr<Server>& server, asio::io_context& io,
            http::verb method, const std::string& path, const Json& body = Json::object(),
            const std::string& origin = {}, const std::string& host = "127.0.0.1:8080",
            http::status expected = http::status::ok) {
    asio::io_context clientIo;
    Tcp::acceptor listener(clientIo, {asio::ip::address_v4::loopback(), 0});
    Tcp::socket client(clientIo);
    client.connect(listener.local_endpoint());
    Tcp::socket peer(io);
    listener.accept(peer);
    auto connection = std::make_shared<HttpConnection>(std::move(peer), server);
    http::request<http::string_body> request{method, path, 11};
    request.set(http::field::host, host);
    if (!origin.empty()) request.set(http::field::origin, origin);
    request.set(http::field::content_type, "application/json");
    request.body() = body.dump();
    request.keep_alive(false);
    server->Route(connection, std::move(request));
    io.poll();
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(client, buffer, response);
    Check(response.result() == expected);
    io.poll();
    if (response[http::field::content_type] == "image/png") return Json{{"bytes", response.body().size()}};
    return ParseJson(response.body());
}

template<class Predicate>
void Pump(asio::io_context& io, Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        Check(Clock::now() < deadline);
        io.poll();
        std::this_thread::yield();
    }
}

void LifecycleChecks() {
    asio::io_context io(1);
    asio::thread_pool worker(1);
    ReleaseGates releaseGates;
    auto catalog = std::make_unique<medgl::DicomCatalog>("data");
    auto series = catalog->Scan();
    auto server = std::make_shared<Server>(io, worker, Config{}, std::move(catalog), std::move(series));
    const auto health = Invoke(server, io, http::verb::get, "/api/v1/health");
    Check(health.at("serviceReady") == true && health.at("gpuReady") == false);
    Check(health.at("pngCompression") == "default");
    const auto presetCatalog = Invoke(server, io, http::verb::get, "/api/v1/presets");
    Check(presetCatalog == PresetCatalogJson());
    Invoke(server, io, http::verb::get, "/api/v1/health", {}, "http://evil.test", "127.0.0.1:8080",
           http::status::forbidden);
    Invoke(server, io, http::verb::get, "/api/v1/health", {}, {}, "evil.test",
           http::status::forbidden);
    Invoke(server, io, http::verb::get, "/api/v1/health", {}, "http://localhost:5173");
    const auto items = Invoke(server, io, http::verb::get, "/api/v1/series");
    Check(items.at("series").size() == 2);
    const auto created = Invoke(server, io, http::verb::post, "/api/v1/sessions",
                                {{"seriesId", "slow"}}, {}, "127.0.0.1:8080", http::status::accepted);
    const auto id = created.at("id").get<std::string>();
    const auto path = "/api/v1/sessions/" + id;
    Check(created.at("state") == "loading" && created.at("sessionId") == id);
    {
        std::unique_lock<std::mutex> lock(GateMutex);
        Check(GateCondition.wait_for(lock, std::chrono::seconds(5), [] { return DecodeStarted; }));
    }
    Invoke(server, io, http::verb::post, "/api/v1/sessions", {{"seriesId", "phantom"}}, {},
           "127.0.0.1:8080", http::status::conflict);
    const auto closing = Invoke(server, io, http::verb::delete_, path, {}, {},
                                "127.0.0.1:8080", http::status::accepted);
    Check(closing.at("state") == "closing");
    {
        std::lock_guard<std::mutex> lock(GateMutex);
        ReleaseDecode = true;
    }
    GateCondition.notify_all();
    Pump(io, [&] { return Invoke(server, io, http::verb::get, path).at("state") == "closed"; });
    Check(Constructions.load() == 0 && Loads.load() == 0);
    Invoke(server, io, http::verb::get, path + "/frame?seq=1&revision=1", {}, {},
           "127.0.0.1:8080", http::status::gone);

    auto session = std::make_shared<Session>(io, "test-session", "phantom");
    session->state = Lifecycle::Ready;
    asio::post(worker, [session] { session->renderer = std::make_unique<medgl::VolumeRenderer>(); });
    server->Submit(session, ParseRender(Request(1)));
    {
        std::unique_lock<std::mutex> lock(GateMutex);
        Check(GateCondition.wait_for(lock, std::chrono::seconds(5), [] { return RenderStarted; }));
    }
    for (std::uint64_t seq = 2; seq <= 100; ++seq) server->Submit(session, ParseRender(Request(seq)));
    Check(session->renderBusy && session->pending && session->pending->seq == 100);
    Rejected([&] { server->Submit(session, ParseRender(Request(100))); });
    auto staleRevision = Request(101);
    staleRevision["sceneRevision"] = 100;
    Rejected([&] { server->Submit(session, ParseRender(staleRevision)); });
    {
        std::lock_guard<std::mutex> lock(GateMutex);
        ReleaseRender = true;
    }
    GateCondition.notify_all();
    Pump(io, [&] { return !session->renderBusy; });
    Check(Renders.load() == 2 && session->finalFrame && session->finalFrame->seq == 100);
    const auto& frame = *session->finalFrame;
    std::uint32_t length = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        length |= static_cast<std::uint32_t>(frame.wire[index]) << (index * 8);
    }
    Check(length + 4 == frame.payloadOffset);
    const auto metadata = ParseJson(std::string(frame.wire.begin() + 4,
                                              frame.wire.begin() + frame.payloadOffset));
    Check(metadata.at("seq") == 100 && metadata.at("encoding") == "png" &&
          metadata.at("payloadBytes") == frame.wire.size() - frame.payloadOffset);
    Check(metadata.at("camera") == CameraJson(ParseRender(Request(100)).settings));
    Check(metadata.at("cropBox") == CropBoxJson(ParseRender(Request(100)).settings));
    Check(metadata.at("clipPlane") == ClipPlaneJson(ParseRender(Request(100)).settings));
    Check(metadata.at("pngCompression") == "default");
    Check(metadata.at("timings").at("kernelMs") == 2 && metadata.at("timings").at("readbackMs") == 4);
    Check(metadata.at("timings").at("encodeMs").get<double>() >= 0 && metadata.at("timings").at("queueMs").get<double>() >= 0);

    asio::io_context clientIo;
    Tcp::acceptor listener(clientIo, {asio::ip::address_v4::loopback(), 0});
    websocket::stream<Tcp::socket> client(clientIo);
    client.next_layer().connect(listener.local_endpoint());
    Tcp::socket peer(io);
    listener.accept(peer);
    bool handshakeRead = false;
    bool handshakeDone = false;
    ErrorCode handshakeError;
    client.async_handshake("127.0.0.1:8080", "/api/v1/sessions/test-session/stream?token=ignored",
        [&](ErrorCode error) { handshakeError = error; handshakeDone = true; });
    beast::flat_buffer handshakeBuffer;
    http::request<http::string_body> handshake;
    http::async_read(peer, handshakeBuffer, handshake,
        [&](ErrorCode error, std::size_t) { Check(!error); handshakeRead = true; });
    Pump(io, [&] { clientIo.poll(); return handshakeRead; });
    auto socket = std::make_shared<WsConnection>(beast::tcp_stream(std::move(peer)), server, session);
    session->socket = socket;
    session->socketReserved = true;
    socket->Accept(std::move(handshake));
    Pump(io, [&] { clientIo.poll(); return handshakeDone; });
    Check(!handshakeError);
    const auto requestText = Request(101, "preview").dump();
    client.text(true);
    client.write(asio::buffer(requestText));
    bool received = false;
    ErrorCode receiveError;
    beast::flat_buffer incoming;
    clientIo.restart();
    client.async_read(incoming, [&](ErrorCode error, std::size_t) {
        receiveError = error;
        received = true;
    });
    Pump(io, [&] { clientIo.poll(); return received; });
    Check(!receiveError && client.got_binary());
    const auto packet = beast::buffers_to_string(incoming.data());
    std::uint32_t packetLength = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        packetLength |= static_cast<std::uint32_t>(static_cast<unsigned char>(packet[index])) << (index * 8);
    }
    const auto packetMeta = ParseJson(packet.substr(4, packetLength));
    Check(packetMeta.at("seq") == 101 && packetMeta.at("encoding") == "jpeg" &&
          packetMeta.at("quality") == "preview");
    incoming.consume(incoming.size());
    received = false;
    client.write(asio::buffer(requestText));
    clientIo.restart();
    client.async_read(incoming, [&](ErrorCode error, std::size_t) {
        receiveError = error;
        received = true;
    });
    Pump(io, [&] { clientIo.poll(); return received; });
    Check(!receiveError && client.got_text());
    Check(ParseJson(beast::buffers_to_string(incoming.data())).at("error").at("code") == "invalid_render");
    socket->Stop();
    Check(!session->socketReserved && session->socket.expired());
    session->timer.cancel();
    asio::post(worker, [session] { session->renderer->Unload(); session->renderer.reset(); });
    server->Shutdown();
    io.run();
    worker.join();
    Check(Violations.load() == 0 && Constructions.load() == Destructions.load());
}

}

int main() {
    IoThread = std::this_thread::get_id();
    try {
        ProtocolChecks();
        LifecycleChecks();
        std::cout << "Server self-test passed: protocol, HTTP, WebSocket, cancellation, latest-wins, worker ownership.\n";
        return EXIT_SUCCESS;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(GateMutex);
            ReleaseDecode = true;
            ReleaseRender = true;
        }
        GateCondition.notify_all();
        std::cerr << "Server self-test failed.\n";
        return EXIT_FAILURE;
    }
}

#endif