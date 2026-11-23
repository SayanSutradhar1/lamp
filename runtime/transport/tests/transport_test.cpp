#include "http/http_server.hpp"
#include "serialization/json_codec.hpp"
#include "transport/async_binary_converter.hpp"
#include "transport/lamp_binary_protocol.hpp"
#include "transport/lamp_binary_server.hpp"
#include "transport/wire_cache.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace lamp;
using namespace lamp::transport;
using Clock = std::chrono::steady_clock;

int g_failed = 0;

void Expect(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
        ++g_failed;
    }
}

#define EXPECT(expr) Expect(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
void CloseSock(Sock s) noexcept {
    if (s != kBadSock) ::closesocket(s);
}
#else
using Sock = int;
constexpr Sock kBadSock = -1;
void CloseSock(Sock s) noexcept {
    if (s != kBadSock) ::close(s);
}
#endif

Sock ConnectLoopback(std::uint16_t port) {
    Sock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSock) throw std::runtime_error("socket failed");
    int nodelay = 1;
#ifdef _WIN32
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#else
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSock(s);
        throw std::runtime_error("connect failed");
    }
    return s;
}

void SendAll(Sock s, std::span<const std::uint8_t> data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const int n = ::send(
            s,
            reinterpret_cast<const char*>(data.data() + off),
            static_cast<int>(data.size() - off),
            0
        );
        if (n <= 0) throw std::runtime_error("send failed");
        off += static_cast<std::size_t>(n);
    }
}

LmpFrame RecvNextLmpFrame(Sock s, LmpFrameDecoder& dec) {
    LmpFrame out;
    while (true) {
        const auto st = dec.TryNextFrame(out);
        if (st == LmpFrameDecoder::FeedStatus::FrameReady) {
            return out;
        }
        if (st == LmpFrameDecoder::FeedStatus::ProtocolError) {
            throw std::runtime_error("ProtocolError decoding LMP frame");
        }
        std::uint8_t buf[8192];
        const int n = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) throw std::runtime_error("recv closed unexpectedly");
        dec.Append(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
    }
}

bool TestCompactBinaryAndBandwidthSavings() {
    // Compare REST HTTP/1.1 + JSON + base64 file vs LMP1 + LCB1 binary frame
    serialization::Value::Array rows;
    for (int i = 0; i < 4; ++i) {
        auto obj = serialization::Value::MakeObject();
        obj.Set("id", "user-" + std::to_string(i));
        obj.Set("name", "Alice Cooper");
        obj.Set("email", "alice" + std::to_string(i) + "@example.com");
        obj.Set("role", "member");
        obj.Set("active", true);
        rows.push_back(std::move(obj));
    }
    serialization::Value input_val(rows);

    std::vector<std::uint8_t> file_bytes(512, std::uint8_t{0xAB});
    LampFilePayload file{"avatar.png", "image/png", file_bytes};

    const auto lcb_req = LcbCodec::EncodeRpcRequest("users.bulkImport", input_val, {file});
    const auto lmp_frame = EncodeLmpFrame(FrameType::Request, 1, lcb_req);

    DecodedRpcInvocation decoded;
    EXPECT(LcbCodec::DecodeRpcRequest(lcb_req, decoded));
    EXPECT(decoded.operation_id == "users.bulkImport");
    EXPECT(decoded.files.size() == 1);
    EXPECT(decoded.files[0].name == "avatar.png");
    EXPECT(decoded.files[0].data == file_bytes);

    // Construct equivalent HTTP/1.1 REST JSON + base64 request
    const auto json_input = *serialization::JsonCodec::EncodeString(input_val).value;
    const auto b64_file = serialization::Base64::Encode(file_bytes);
    const std::string rest_body =
        "{\"operation\":\"users.bulkImport\",\"rows\":" + json_input +
        ",\"file\":{\"name\":\"avatar.png\",\"mimeType\":\"image/png\",\"data\":\"" + b64_file + "\"}}";
    const std::string rest_http =
        "POST /api/v1/users/bulkImport HTTP/1.1\r\n"
        "Host: localhost:3000\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Content-Length: " + std::to_string(rest_body.size()) + "\r\n\r\n" +
        rest_body;

    // Repeated field keys + zero HTTP header overhead + raw binary file -> compact LCB1 wins easily.
    // Also check metadata-free typical RPC object packet (< 50% of REST HTTP packet):
    auto single = serialization::Value::MakeObject();
    single.Set("id", "1");
    single.Set("name", "Alice");
    single.Set("email", "alice@example.com");
    single.Set("role", "admin");
    const auto small_lmp = EncodeLmpFrame(
        FrameType::Request,
        2,
        LcbCodec::EncodeRpcRequest("users.get", single)
    );
    const std::string small_rest_body = R"({"id":"1","name":"Alice","email":"alice@example.com","role":"admin"})";
    const std::string small_rest_http =
        "POST /api/v1/users/get HTTP/1.1\r\nHost: 127.0.0.1:3000\r\nContent-Type: application/json\r\nAccept: application/json\r\nContent-Length: " +
        std::to_string(small_rest_body.size()) + "\r\n\r\n" + small_rest_body;

    EXPECT(lmp_frame.size() < rest_http.size());
    EXPECT(small_lmp.size() * 2U <= small_rest_http.size());
    std::cout << "  [wire] small_rest=" << small_rest_http.size()
              << "B vs small_lmp=" << small_lmp.size() << "B ("
              << (static_cast<double>(small_rest_http.size()) / static_cast<double>(small_lmp.size()))
              << "x smaller)\n";
    return true;
}

bool TestAsyncBinaryStreamingConversion() {
    auto pool = std::make_shared<core::DynamicWorkerPool>();
    AsyncBinaryStreamConverter converter(pool);

    std::vector<std::uint8_t> attachment(20000, std::uint8_t{0x5C});
    AsyncBinaryConversionRequest req;
    req.operation_id = "files.upload";
    req.json_input = R"({"folder":"avatars","public":true})";
    req.files = {LampFilePayload{"photo.bin", "application/octet-stream", attachment}};
    req.chunk_bytes = 4096;

    auto [channel, fut] = converter.ConvertToBinaryStreamAsync(std::move(req));
    std::vector<std::uint8_t> reassembled;
    while (auto chunk = channel->Pop(std::chrono::milliseconds{2000})) {
        reassembled.insert(reassembled.end(), chunk->data.begin(), chunk->data.end());
    }

    const auto stats = fut.get();
    EXPECT(stats.ok);
    EXPECT(stats.chunks_emitted >= 4);
    EXPECT(reassembled.size() == stats.binary_output_bytes);

    DecodedRpcInvocation decoded;
    EXPECT(LcbCodec::DecodeRpcRequest(reassembled, decoded));
    EXPECT(decoded.operation_id == "files.upload");
    EXPECT(decoded.files.size() == 1);
    EXPECT(decoded.files[0].data == attachment);
    return true;
}

bool TestFullCycleWireCacheAndTtlUpperLimit() {
    auto now = std::make_shared<Clock::time_point>(Clock::time_point(std::chrono::seconds(100)));
    FullCycleWireCacheConfig cfg;
    cfg.max_bytes = 4096;
    cfg.max_entries = 16;
    cfg.shard_count = 1;
    cfg.max_ttl_limit = std::chrono::milliseconds{5000}; // hard 5s upper limit

    FullCycleWireCache cache(cfg, [now] { return *now; });
    const std::vector<std::uint8_t> req_bytes = {'r', 'e', 'q'};
    const std::vector<std::uint8_t> resp_bytes = {'r', 'e', 's', 'p'};

    // Caller asks for 60s TTL -> must be clamped down to hard upper limit 5s
    const auto effective = cache.Store(
        "users.get",
        req_bytes,
        resp_bytes,
        std::chrono::milliseconds{60000}
    );
    EXPECT(effective == std::chrono::milliseconds{5000});

    EXPECT(cache.Lookup("users.get", req_bytes).has_value());
    *now += std::chrono::milliseconds{4999};
    EXPECT(cache.Lookup("users.get", req_bytes).has_value());
    *now += std::chrono::milliseconds{1};
    EXPECT(!cache.Lookup("users.get", req_bytes).has_value());
    EXPECT(cache.Hits() == 2);
    EXPECT(cache.Misses() == 1);
    return true;
}

bool TestBinaryServerSpeedAndBandwidthVsRest() {
    std::atomic<std::uint64_t> handler_executions{0};

    LampBinaryServerConfig bin_cfg;
    bin_cfg.port = 0;
    bin_cfg.wire_cache.max_ttl_limit = std::chrono::milliseconds{30'000};

    LampBinaryServer bin_server(
        [&](const DecodedRpcInvocation& inv, std::uint64_t) -> BinaryRpcDispatchResult {
            handler_executions.fetch_add(1, std::memory_order_relaxed);
            auto out = serialization::Value::MakeObject();
            out.Set("id", "u-1");
            out.Set("name", "Alice");
            out.Set("op", inv.operation_id);

            BinaryRpcDispatchResult res;
            res.ok = true;
            res.encoded_lcb_response = LcbCodec::EncodeRpcResponse(true, out);
            res.cache_ttl = std::chrono::milliseconds{10'000};
            return res;
        },
        bin_cfg
    );
    bin_server.Start();

    // Verify full-cycle cache hit + WebSocket accept helper + HTTP REST rejection
    {
        Sock cli = ConnectLoopback(bin_server.Port());
        auto in_obj = serialization::Value::MakeObject();
        in_obj.Set("id", "u-1");
        const auto lcb = LcbCodec::EncodeRpcRequest("users.get", in_obj);
        const auto frame = EncodeLmpFrame(FrameType::Request, 1, lcb);

        SendAll(cli, frame);
        LmpFrameDecoder dec;
        const auto f1 = RecvNextLmpFrame(cli, dec);
        EXPECT(f1.header.type == FrameType::Response);
        EXPECT((f1.header.flags & static_cast<std::uint8_t>(FrameFlags::CacheHit)) == 0);

        // 2nd identical frame served directly from full-cycle WireCache (CacheHit flag set, handler count stays 1)
        SendAll(cli, frame);
        const auto f2 = RecvNextLmpFrame(cli, dec);
        EXPECT(f2.header.type == FrameType::Response);
        EXPECT((f2.header.flags & static_cast<std::uint8_t>(FrameFlags::CacheHit)) != 0);
        EXPECT(handler_executions.load() == 1);
        CloseSock(cli);

        // Plain REST request is rejected (HTTP 426)
        Sock rest_cli = ConnectLoopback(bin_server.Port());
        const std::string bad_rest = "POST /users HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        SendAll(rest_cli, std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bad_rest.data()), bad_rest.size()));
        char tmp[256]{};
        const int rlen = ::recv(rest_cli, tmp, sizeof(tmp), 0);
        EXPECT(rlen > 0 && std::string_view(tmp, static_cast<std::size_t>(rlen)).find("426") != std::string_view::npos);
        CloseSock(rest_cli);

        // RFC 6455 WebSocket handshake verification
        EXPECT(
            ComputeWebSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
        );
    }

    // Comparative loopback benchmark:
    // Baseline HTTP/1.1 REST (new connection per REST request, typical short-lived REST pattern)
    // vs Lamp persistent multiplexed binary LMP1 + LCB1 over TCP (with distinct request payloads).
    http::HttpServerConfig http_cfg;
    http_cfg.port = 0;
    http::HttpServer rest_server(http_cfg);
    rest_server.route(
        http::HttpMethod::Post,
        "/api/v1/users/get",
        [](const http::HttpRequest&, const http::HttpRouteContext&) {
            http::HttpResponse resp(http::HttpStatus::Ok);
            resp.setHeader("Content-Type", "application/json");
            resp.setBody(R"({"id":"u-1","name":"Alice","op":"users.get"})");
            return resp;
        }
    );
    rest_server.start();
    const std::uint16_t rest_port = rest_server.port();

    constexpr int kIters = 120;
    std::size_t total_rest_bytes = 0;
    std::size_t total_lamp_bytes = 0;

    const auto rest_t0 = Clock::now();
    for (int i = 0; i < kIters; ++i) {
        Sock rs = ConnectLoopback(rest_port);
        const std::string body = "{\"id\":\"u-" + std::to_string(i) + "\"}";
        const std::string req =
            "POST /api/v1/users/get HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        SendAll(rs, std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(req.data()), req.size()));
        total_rest_bytes += req.size();
        char rbuf[2048];
        while (true) {
            const int n = ::recv(rs, rbuf, sizeof(rbuf), 0);
            if (n <= 0) break;
            total_rest_bytes += static_cast<std::size_t>(n);
        }
        CloseSock(rs);
    }
    const auto rest_ms = std::chrono::duration<double, std::milli>(Clock::now() - rest_t0).count();

    Sock ls = ConnectLoopback(bin_server.Port());
    LmpFrameDecoder ldec;
    const auto lamp_t0 = Clock::now();
    for (int i = 0; i < kIters; ++i) {
        auto in_obj = serialization::Value::MakeObject();
        in_obj.Set("id", "u-" + std::to_string(i));
        const auto lcb = LcbCodec::EncodeRpcRequest("users.get", in_obj);
        const auto frame = EncodeLmpFrame(FrameType::Request, static_cast<std::uint64_t>(i + 1), lcb);
        SendAll(ls, frame);
        total_lamp_bytes += frame.size();
        const auto resp = RecvNextLmpFrame(ls, ldec);
        total_lamp_bytes += kLmpHeaderBytes + resp.payload.size();
    }
    const auto lamp_ms = std::chrono::duration<double, std::milli>(Clock::now() - lamp_t0).count();
    CloseSock(ls);

    rest_server.shutdown();
    bin_server.Shutdown();

    const double speedup = rest_ms / (lamp_ms > 0.001 ? lamp_ms : 0.001);
    const double bw_ratio = static_cast<double>(total_rest_bytes) /
                            static_cast<double>(total_lamp_bytes);
    std::cout << "  [bench " << kIters << " iters] REST_ms=" << rest_ms
              << " vs LAMP_ms=" << lamp_ms << " (" << speedup << "x faster)"
              << " | REST_bytes=" << total_rest_bytes
              << " vs LAMP_bytes=" << total_lamp_bytes << " (" << bw_ratio << "x less bandwidth)\n";

    EXPECT(speedup >= 4.0);
    EXPECT(bw_ratio >= 2.0);
    return true;
}

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };
    const Case cases[] = {
        {"CompactBinaryAndBandwidthSavings", TestCompactBinaryAndBandwidthSavings},
        {"AsyncBinaryStreamingConversion", TestAsyncBinaryStreamingConversion},
        {"FullCycleWireCacheAndTtlUpperLimit", TestFullCycleWireCacheAndTtlUpperLimit},
        {"BinaryServerSpeedAndBandwidthVsRest", TestBinaryServerSpeedAndBandwidthVsRest},
    };

    int passed = 0;
    const int total = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    for (const auto& c : cases) {
        const int before = g_failed;
        if (c.fn() && g_failed == before) {
            std::cout << "[PASS] " << c.name << "\n";
            ++passed;
        } else {
            std::cout << "[FAIL] " << c.name << "\n";
        }
    }
    std::cout << "Lamp transport: " << passed << "/" << total << " tests passed\n";
    return (passed == total && g_failed == 0) ? 0 : 1;
}
