#include "host/lamp_app_server.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace lamp::host;
using namespace lamp::transport;

int g_failed = 0;

void Expect(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
        ++g_failed;
    }
}

#define EXPECT(expr) Expect(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(NativeSocket s) noexcept {
    if (s != kInvalidSocket) ::closesocket(s);
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void CloseSocket(NativeSocket s) noexcept {
    if (s != kInvalidSocket) ::close(s);
}
#endif

NativeSocket ConnectTcp(const std::string& host, std::uint16_t port) {
    NativeSocket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) throw std::runtime_error("socket failed");

    int nodelay = 1;
    (void)::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        CloseSocket(sock);
        throw std::runtime_error("inet_pton failed");
    }
    if (::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSocket(sock);
        throw std::runtime_error("connect failed");
    }
    return sock;
}

void SendAll(NativeSocket sock, const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int n = ::send(sock, reinterpret_cast<const char*>(data + sent),
                             static_cast<int>(size - sent), 0);
        if (n <= 0) throw std::runtime_error("send failed");
        sent += static_cast<std::size_t>(n);
    }
}

std::string SendHttpRawAndClose(const std::string& host, std::uint16_t port, std::string_view req) {
    NativeSocket sock = ConnectTcp(host, port);
    SendAll(sock, reinterpret_cast<const std::uint8_t*>(req.data()), req.size());
    std::string resp;
    char buf[4096];
    while (true) {
        const int n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    CloseSocket(sock);
    return resp;
}

std::pair<LmpFrame, DecodedRpcResult> CallBinaryRpc(
    NativeSocket sock,
    std::uint64_t request_id,
    std::string_view op_id,
    const lamp::serialization::Value& input,
    const std::vector<LampFilePayload>& files = {}
) {
    const auto payload = LcbCodec::EncodeRpcRequest(op_id, input, files);
    const auto frame = EncodeLmpFrame(
        FrameType::Request,
        request_id,
        payload,
        static_cast<std::uint8_t>(files.empty() ? FrameFlags::None : FrameFlags::HasFiles)
    );
    SendAll(sock, frame.data(), frame.size());

    LmpFrameDecoder dec;
    LmpFrame resp;
    std::uint8_t buf[8192];
    while (true) {
        const auto st = dec.TryNextFrame(resp);
        if (st == LmpFrameDecoder::FeedStatus::FrameReady) break;
        if (st == LmpFrameDecoder::FeedStatus::ProtocolError) {
            throw std::runtime_error("Protocol error decoding response");
        }
        const int n = ::recv(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) throw std::runtime_error("Unexpected EOF from binary server");
        dec.Append(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
    }

    DecodedRpcResult rpc_res;
    if (!LcbCodec::DecodeRpcResponse(resp.payload, rpc_res)) {
        throw std::runtime_error("Malformed LCB1 response payload");
    }
    return {std::move(resp), std::move(rpc_res)};
}

} // namespace

int main() {
    LampAppServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_pool.min_threads = 2;
    config.worker_pool.max_threads = 6;
    config.wire_cache.max_ttl_limit = std::chrono::milliseconds(5000);
    config.entry_name = "binary-e2e-app.js";
    config.entry_source = R"JS(
const app = createLampApp({
  cache: { enabled: true, defaultTtl: 10 },
});

const users = new Map();
let hitsCounter = 0;

app.register("users.create", {
  input: s.object({ name: s.string({ minLength: 2 }) }),
  output: s.object({ id: s.string(), name: s.string() }),
  handler: async ({ input }) => {
    const id = "u_" + (users.size + 1);
    const user = { id, name: input.name };
    users.set(id, user);
    return user;
  },
});

app.register("users.getCached", {
  input: s.object({ id: s.string() }),
  output: s.object({ id: s.string(), name: s.string(), evals: s.number() }),
  cache: { ttl: 600 }, // exceeds 5000ms limit -> clamped server-side
  handler: ({ input }) => {
    hitsCounter += 1;
    const found = users.get(input.id);
    if (!found) {
      throw new LampError("User not found", { code: "NOT_FOUND", identifier: "USER_MISSING" });
    }
    return { id: found.id, name: found.name, evals: hitsCounter };
  },
});

app.register("files.inspect", {
  input: s.object({
    label: s.string(),
    document: s.file({ maxSizeBytes: 1024 * 1024 }),
  }),
  output: s.object({
    label: s.string(),
    fileName: s.string(),
    mimeType: s.string(),
    byteLength: s.number(),
    firstByte: s.number(),
  }),
  handler: ({ input }) => ({
    label: input.label,
    fileName: input.document.name,
    mimeType: input.document.mimeType,
    byteLength: input.document.data.byteLength,
    firstByte: input.document.data[0] ?? -1,
  }),
});

app.register("compute.parallelSquares", {
  input: s.object({ items: s.array(s.number()) }),
  output: s.object({ squares: s.array(s.number()) }),
  handler: async ({ input, ctx }) => {
    const squares = await ctx.parallel(input.items, (n) => n * n + 1);
    return { squares };
  },
});

app.start();
)JS";

    LampAppServer server(std::move(config));
    server.Start();
    EXPECT(server.IsRunning());
    EXPECT(server.Port() > 0);

    // 1. Verify plain HTTP REST requests are rejected
    {
        const std::string rest_resp = SendHttpRawAndClose(
            "127.0.0.1",
            server.Port(),
            "GET /users HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
        );
        EXPECT(rest_resp.find("404 Not Found") != std::string::npos);
    }

    // 2. Binary LMP1 persistent connection checks
    {
        NativeSocket sock = ConnectTcp("127.0.0.1", server.Port());

        // Manifest
        {
            const auto [frame, res] = CallBinaryRpc(
                sock, 1, "$lamp.manifest", lamp::serialization::Value::MakeNull());
            EXPECT(res.ok);
            EXPECT(res.output.Find("restPattern") && !res.output.Find("restPattern")->AsBool().value_or(true));
        }

        // Create user
        std::string created_id;
        {
            lamp::serialization::Value::Object req_obj;
            req_obj.emplace_back("name", lamp::serialization::Value("Ada Lovelace"));
            const auto [frame, res] = CallBinaryRpc(
                sock, 2, "users.create", lamp::serialization::Value::MakeObject(std::move(req_obj)));
            EXPECT(res.ok);
            EXPECT(res.output.Find("id") && res.output.Find("id")->IsString());
            created_id = *res.output.Find("id")->AsString();
            EXPECT(created_id == "u_1");
        }

        // Full-cycle wire cache: 1st call executes JS (miss), 2nd call hits cache BEFORE V8
        {
            lamp::serialization::Value::Object req_obj;
            req_obj.emplace_back("id", lamp::serialization::Value("u_1"));
            const auto input_val = lamp::serialization::Value::MakeObject(std::move(req_obj));

            const auto [f1, r1] = CallBinaryRpc(sock, 3, "users.getCached", input_val);
            EXPECT(r1.ok);
            EXPECT((f1.header.flags & static_cast<std::uint8_t>(FrameFlags::CacheHit)) == 0);
            EXPECT(r1.output.Find("evals")->AsInt64().value_or(0) == 1);

            const auto [f2, r2] = CallBinaryRpc(sock, 4, "users.getCached", input_val);
            EXPECT(r2.ok);
            EXPECT((f2.header.flags & static_cast<std::uint8_t>(FrameFlags::CacheHit)) != 0);
            EXPECT(r2.output.Find("evals")->AsInt64().value_or(0) == 1);
            EXPECT(server.WireCache()->Hits() >= 1);
        }

        // Native binary file transfer (s.file() + LCB1 files array, zero base64)
        {
            LampFilePayload file;
            file.name = "avatar.bin";
            file.mime_type = "application/octet-stream";
            file.data = {0x7F, 0x10, 0x20, 0x30};

            lamp::serialization::Value::Object ref_obj;
            ref_obj.emplace_back("__lampFileRef", lamp::serialization::Value(0));

            lamp::serialization::Value::Object req_obj;
            req_obj.emplace_back("label", lamp::serialization::Value("profile"));
            req_obj.emplace_back("document", lamp::serialization::Value::MakeObject(std::move(ref_obj)));

            const auto [frame, res] = CallBinaryRpc(
                sock, 5, "files.inspect",
                lamp::serialization::Value::MakeObject(std::move(req_obj)),
                {file}
            );
            EXPECT(res.ok);
            EXPECT(res.output.Find("fileName")->AsString().value_or("") == "avatar.bin");
            EXPECT(res.output.Find("byteLength")->AsInt64().value_or(0) == 4);
            EXPECT(res.output.Find("firstByte")->AsInt64().value_or(0) == 0x7F);
        }

        // Parallel subtask splitting (ctx.parallel -> DynamicWorkerPool)
        {
            lamp::serialization::Value::Array items;
            for (int i = 1; i <= 8; ++i) {
                items.emplace_back(i);
            }
            lamp::serialization::Value::Object req_obj;
            req_obj.emplace_back("items", lamp::serialization::Value::MakeArray(std::move(items)));

            const auto [frame, res] = CallBinaryRpc(
                sock, 6, "compute.parallelSquares",
                lamp::serialization::Value::MakeObject(std::move(req_obj))
            );
            EXPECT(res.ok);
            const auto* sq = res.output.Find("squares") ? res.output.Find("squares")->AsArray() : nullptr;
            EXPECT(sq != nullptr && sq->size() == 8);
            if (sq && sq->size() == 8) {
                EXPECT((*sq)[0].AsInt64().value_or(0) == 2);
                EXPECT((*sq)[7].AsInt64().value_or(0) == 65);
            }
        }

        // Validation error propagation
        {
            lamp::serialization::Value::Object req_obj;
            req_obj.emplace_back("name", lamp::serialization::Value("A"));
            const auto [frame, res] = CallBinaryRpc(
                sock, 7, "users.create", lamp::serialization::Value::MakeObject(std::move(req_obj))
            );
            EXPECT(!res.ok);
            EXPECT(res.error_code == "INVALID_ARGUMENT");
            EXPECT(res.error_identifier == "INPUT_VALIDATION_FAILED");
        }

        CloseSocket(sock);
    }

    server.Shutdown();
    EXPECT(!server.IsRunning());

    if (g_failed == 0) {
        std::cout << "Lamp binary host app server test passed\n";
    } else {
        std::cerr << g_failed << " check(s) failed\n";
    }
    return g_failed == 0 ? 0 : 1;
}
