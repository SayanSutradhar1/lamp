
#include "streaming/chunked_http_codec.hpp"
#include "streaming/stream_channel.hpp"
#include "streaming/stream_hub.hpp"
#include "streaming/streaming_http_bridge.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

namespace bridge = lamp::bridge;
namespace http = lamp::http;
namespace streaming = lamp::streaming;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestStreamChannelBoundedBackpressure() {
    streaming::StreamConfig cfg;
    cfg.max_buffered_chunks = 2;
    cfg.max_buffered_bytes = 10;
    cfg.max_chunk_bytes = 8;

    streaming::StreamChannel ch(cfg);

    // Oversized chunk rejected
    Require(
        ch.TryPush(streaming::StreamChunk::FromString("123456789")) ==
            streaming::StreamPushStatus::Oversized,
        "Oversized chunk rejected"
    );

    // Push two 5-byte chunks (total 10 bytes == max_buffered_bytes)
    Require(
        ch.TryPush(streaming::StreamChunk::FromString("aaaaa")) ==
            streaming::StreamPushStatus::Accepted,
        "Chunk 1 accepted"
    );
    Require(
        ch.TryPush(streaming::StreamChunk::FromString("bbbbb")) ==
            streaming::StreamPushStatus::Accepted,
        "Chunk 2 accepted"
    );

    // 3rd hits capacity -> Full
    Require(
        ch.TryPush(streaming::StreamChunk::FromString("c")) ==
            streaming::StreamPushStatus::Full,
        "3rd chunk rejected with Full backpressure"
    );

    auto c1 = ch.TryPop();
    Require(c1.has_value() && c1->sequence == 1, "Popped chunk 1");

    // 6 bytes wouldn't fit with 5 bytes remaining (11 > 10) -> Full
    Require(
        ch.TryPush(streaming::StreamChunk::FromString("cccccc")) ==
            streaming::StreamPushStatus::Full,
        "Byte limit enforced"
    );

    // 5 bytes fits (5 + 5 <= 10)
    Require(
        ch.TryPush(streaming::StreamChunk::FromString("ccccc")) ==
            streaming::StreamPushStatus::Accepted,
        "Fits within remaining byte budget"
    );

    const auto stats = ch.Stats();
    Require(stats.backpressure_events == 2, "Backpressure counted");
}

void TestStreamChannelBlockingPushAndSlowConsumer() {
    streaming::StreamConfig cfg;
    cfg.max_buffered_chunks = 2;
    cfg.max_buffered_bytes = 1024;
    streaming::StreamChannel ch(cfg);

    constexpr int kTotalChunks = 20;
    std::atomic<int> pushedCount{0};

    std::thread producer([&]() {
        for (int i = 0; i < kTotalChunks; ++i) {
            const auto status = ch.Push(
                streaming::StreamChunk::FromString(
                    "msg_" + std::to_string(i)
                ),
                2000ms
            );
            if (status != streaming::StreamPushStatus::Accepted) {
                return;
            }
            ++pushedCount;
        }
        ch.Complete();
    });

    std::vector<std::string> received;
    while (!ch.IsDrainedAndClosed()) {
        if (auto item = ch.Pop(500ms)) {
            received.emplace_back(
                item->data.begin(),
                item->data.end()
            );
            std::this_thread::sleep_for(2ms);
        }
    }

    producer.join();

    Require(pushedCount.load() == kTotalChunks, "All 20 chunks pushed");
    Require(received.size() == kTotalChunks, "All 20 chunks received");
    for (int i = 0; i < kTotalChunks; ++i) {
        Require(
            received[static_cast<std::size_t>(i)] ==
                "msg_" + std::to_string(i),
            "Chunk order preserved"
        );
    }
    Require(ch.Stats().backpressure_events > 0, "Producer experienced backpressure");
}

void TestStreamChannelCancellationAndError() {
    // 1. External cancellation wakes blocked producer
    bridge::RequestCancellationSource cancelSrc;
    streaming::StreamConfig cfg;
    cfg.max_buffered_chunks = 1;
    streaming::StreamChannel ch(cfg, cancelSrc.Token());

    Require(
        ch.TryPush(streaming::StreamChunk::FromString("fill")) ==
            streaming::StreamPushStatus::Accepted,
        "Fill 1-slot channel"
    );

    std::promise<streaming::StreamPushStatus> blockedRes;
    auto blockedFuture = blockedRes.get_future();
    std::thread producer([&]() {
        blockedRes.set_value(
            ch.Push(streaming::StreamChunk::FromString("blocked"), 3000ms)
        );
    });

    std::this_thread::sleep_for(15ms);
    ch.Cancel();

    Require(
        blockedFuture.get() == streaming::StreamPushStatus::Cancelled,
        "Blocked producer woke up with Cancelled"
    );
    Require(ch.IsCancelled(), "Channel reports cancelled");
    producer.join();

    // 2. External RequestCancellationToken propagates
    streaming::StreamChannel extCh(cfg, cancelSrc.Token());
    cancelSrc.Cancel();
    Require(
        extCh.TryPush(streaming::StreamChunk::FromString("x")) ==
            streaming::StreamPushStatus::Cancelled,
        "External cancellation token stops push"
    );

    // 3. Error termination preserves already-buffered chunks and error
    streaming::StreamConfig errCfg;
    errCfg.max_buffered_chunks = 4;
    streaming::StreamChannel errCh(errCfg);
    Require(
        errCh.TryPush(streaming::StreamChunk::FromString("partial")) ==
            streaming::StreamPushStatus::Accepted,
        "Push before fail"
    );
    errCh.Fail({
        bridge::OperationErrorCode::Internal,
        "upstream failure",
        "STREAM_ERR",
        false
    });
    Require(errCh.State() == streaming::StreamState::Failed, "Failed state");
    auto partial = errCh.TryPop();
    Require(partial.has_value(), "Buffered chunk still readable before terminal error");
    Require(errCh.IsDrainedAndClosed(), "Drained after pop");
    Require(
        errCh.Error().has_value() &&
            errCh.Error()->identifier == "STREAM_ERR",
        "Error preserved"
    );
}

void TestChunkedHttpCodecRoundtripAndIncrementalSplits() {
    http::HttpHeaders headers;
    headers.set("Content-Type", "application/octet-stream");
    headers.set("Content-Length", "999"); // must be stripped in chunked mode

    const std::string headerWire = streaming::ChunkedHttpEncoder::EncodeHeader(
        http::HttpStatus::Ok,
        headers
    );
    Require(
        headerWire.find("Transfer-Encoding: chunked") != std::string::npos,
        "Transfer-Encoding: chunked set"
    );
    Require(
        headerWire.find("Content-Length") == std::string::npos,
        "Conflicting Content-Length stripped"
    );

    const std::vector<std::uint8_t> binChunk{0x00, 0xFF, 'A', 'B', 0x00};
    const std::string wireBody =
        streaming::ChunkedHttpEncoder::EncodeChunk("hello ") +
        streaming::ChunkedHttpEncoder::EncodeChunk(binChunk) +
        "0;ext=1\r\nX-Trailer: ok\r\n\r\n";

    // Feed 1 byte at a time
    streaming::ChunkedHttpDecoder decoder;
    std::vector<std::uint8_t> reassembled;
    for (std::size_t i = 0; i < wireBody.size(); ++i) {
        const auto res = decoder.Feed(
            std::string_view(&wireBody[i], 1),
            [&](std::span<const std::uint8_t> data) {
                reassembled.insert(
                    reassembled.end(),
                    data.begin(),
                    data.end()
                );
            }
        );
        Require(!res.failed, "1-byte feed must not fail");
    }

    Require(decoder.Completed(), "Chunked stream completed");

    std::vector<std::uint8_t> expected{'h', 'e', 'l', 'l', 'o', ' '};
    expected.insert(expected.end(), binChunk.begin(), binChunk.end());
    Require(reassembled == expected, "Binary-safe chunked roundtrip matched");

    // Malformed chunk tests
    streaming::ChunkedHttpDecoder badHex;
    Require(
        badHex.Feed("ZZZ\r\n", {}).failed,
        "Invalid chunk hex rejected"
    );

    streaming::ChunkedHttpDecoder badDelim;
    Require(
        badDelim.Feed("3\r\nabcXX", {}).failed,
        "Missing CRLF after chunk rejected"
    );
}

void TestSseCodecEncoding() {
    streaming::SseEvent ev;
    ev.id = "42\nignored";
    ev.event = "token\r\n";
    ev.retry_ms = 1500;
    ev.data = "line1\nline2";

    const std::string encoded = streaming::SseCodec::Encode(ev);
    Require(
        encoded ==
            "id: 42ignored\n"
            "event: token\n"
            "retry: 1500\n"
            "data: line1\n"
            "data: line2\n\n",
        "SSE multi-line & sanitized encoding"
    );
}

void TestStreamHubFanOutSlowConsumerAndPruning() {
    streaming::StreamHub hub(16);

    streaming::StreamConfig fastCfg;
    fastCfg.max_buffered_chunks = 8;
    streaming::StreamConfig slowCfg;
    slowCfg.max_buffered_chunks = 1;

    auto fastSub = hub.Subscribe("prices", fastCfg);
    auto slowSub = hub.Subscribe("prices", slowCfg);
    auto ephemeralSub = hub.Subscribe("prices", fastCfg);
    ephemeralSub->Cancel();

    Require(hub.SubscriberCount("prices") == 2, "Cancelled subscriber pruned");

    const auto r1 = hub.Publish(
        "prices",
        streaming::StreamChunk::FromString("tick1")
    );
    Require(r1.delivered == 2 && r1.backpressured == 0, "Both got tick1");

    const auto r2 = hub.Publish(
        "prices",
        streaming::StreamChunk::FromString("tick2")
    );
    Require(
        r2.delivered == 1 && r2.backpressured == 1,
        "Slow subscriber backpressured on tick2 without blocking fast subscriber"
    );

    hub.CloseTopic("prices");
    Require(hub.SubscriberCount("prices") == 0, "Topic closed");
    Require(fastSub->State() == streaming::StreamState::Completed, "Fast sub completed");
}

void TestHttpChunkedStreamPumpAndBackpressure() {
    streaming::StreamConfig cfg;
    cfg.max_buffered_chunks = 8;
    auto ch = std::make_shared<streaming::StreamChannel>(cfg);

    // Small pending buffer (64 bytes) to verify backpressure on PollChunks
    streaming::HttpChunkedStreamPump pump(
        ch,
        http::HttpStatus::Ok,
        {},
        http::HttpVersion{},
        64
    );

    Require(
        ch->TryPush(streaming::StreamChunk::FromString(std::string(40, 'A'))) ==
            streaming::StreamPushStatus::Accepted,
        "Push A"
    );
    Require(
        ch->TryPush(streaming::StreamChunk::FromString(std::string(40, 'B'))) ==
            streaming::StreamPushStatus::Accepted,
        "Push B"
    );
    ch->Complete();

    // Header alone is ~47 bytes, so pulling 1st chunk pushes PendingOutput >= 64
    // and leaves 2nd chunk inside StreamChannel!
    const std::size_t pulled1 = pump.PollChunks();
    Require(pulled1 == 1, "Write limit stopped pulling 2nd chunk");
    Require(!pump.IsFinished(), "Not finished until 2nd chunk drained");

    std::string fullWire;
    while (!pump.ShouldClose()) {
        (void)pump.PollChunks();
        const auto view = pump.PendingOutput();
        if (!view.empty()) {
            const std::size_t take = std::min<std::size_t>(view.size(), 16);
            fullWire.append(view.data(), take);
            pump.ConsumeOutput(take);
        }
    }

    const auto hdrEnd = fullWire.find("\r\n\r\n");
    Require(hdrEnd != std::string::npos, "Headers present");
    streaming::ChunkedHttpDecoder decoder;
    std::string decoded;
    const auto decRes = decoder.Feed(
        std::string_view(fullWire).substr(hdrEnd + 4),
        [&](std::span<const std::uint8_t> s) {
            decoded.append(reinterpret_cast<const char*>(s.data()), s.size());
        }
    );
    Require(!decRes.failed && decoder.Completed(), "Decoded streamed response");
    Require(
        decoded == std::string(40, 'A') + std::string(40, 'B'),
        "Chunked stream payload intact"
    );
}

void TestLiveTcpChunkedStreamOverLoopback() {
#ifdef _WIN32
    WSADATA wsaData{};
    Require(::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0, "WSAStartup");
    struct WsaGuard final {
        ~WsaGuard() { ::WSACleanup(); }
    } wsaGuard;
    using SockType = SOCKET;
    constexpr SockType kInvalid = INVALID_SOCKET;
    auto closeSock = [](SockType s) {
        if (s != INVALID_SOCKET) { ::closesocket(s); }
    };
#else
    using SockType = int;
    constexpr SockType kInvalid = -1;
    auto closeSock = [](SockType s) {
        if (s != -1) { ::close(s); }
    };
#endif

    SockType listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(listener != kInvalid, "listener socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    Require(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1, "inet_pton");
    Require(
        ::bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 &&
        ::listen(listener, 1) == 0,
        "bind/listen"
    );

    sockaddr_in bound{};
#ifdef _WIN32
    int boundLen = sizeof(bound);
#else
    socklen_t boundLen = sizeof(bound);
#endif
    Require(
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0,
        "getsockname"
    );
    const std::uint16_t port = ntohs(bound.sin_port);

    streaming::StreamConfig scfg;
    scfg.max_buffered_chunks = 4;
    auto channel = std::make_shared<streaming::StreamChannel>(scfg);

    std::thread serverThread([&]() {
        SockType peer = ::accept(listener, nullptr, nullptr);
        if (peer == kInvalid) {
            return;
        }
        streaming::HttpChunkedStreamPump pump(
            channel,
            http::HttpStatus::Ok,
            {},
            http::HttpVersion{},
            128
        );

        std::thread producer([&]() {
            for (int i = 0; i < 10; ++i) {
                (void)channel->Push(
                    streaming::StreamChunk::FromString("item" + std::to_string(i) + ";"),
                    2000ms
                );
            }
            channel->Complete();
        });

        while (!pump.ShouldClose()) {
            (void)pump.PollChunks();
            const auto out = pump.PendingOutput();
            if (!out.empty()) {
                const int sent = ::send(
                    peer,
                    out.data(),
                    static_cast<int>(out.size()),
                    0
                );
                if (sent <= 0) {
                    pump.Cancel();
                    break;
                }
                pump.ConsumeOutput(static_cast<std::size_t>(sent));
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }

        producer.join();
        closeSock(peer);
    });

    SockType client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(client != kInvalid, "client socket");
    sockaddr_in connectAddr{};
    connectAddr.sin_family = AF_INET;
    connectAddr.sin_port = htons(port);
    Require(::inet_pton(AF_INET, "127.0.0.1", &connectAddr.sin_addr) == 1, "inet_pton");
    Require(
        ::connect(
            client,
            reinterpret_cast<const sockaddr*>(&connectAddr),
            sizeof(connectAddr)
        ) == 0,
        "connect"
    );

    std::string raw;
    char buf[1024];
    while (true) {
        const int n = ::recv(client, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buf, static_cast<std::size_t>(n));
    }

    closeSock(client);
    serverThread.join();
    closeSock(listener);

    const auto hdrEnd = raw.find("\r\n\r\n");
    Require(hdrEnd != std::string::npos, "HTTP header received");
    Require(
        raw.substr(0, hdrEnd).find("Transfer-Encoding: chunked") != std::string::npos,
        "Chunked transfer header received over TCP"
    );

    streaming::ChunkedHttpDecoder decoder;
    std::string body;
    const auto dec = decoder.Feed(
        std::string_view(raw).substr(hdrEnd + 4),
        [&](std::span<const std::uint8_t> s) {
            body.append(reinterpret_cast<const char*>(s.data()), s.size());
        }
    );
    Require(!dec.failed && decoder.Completed(), "TCP chunked stream decoded");

    std::string expected;
    for (int i = 0; i < 10; ++i) {
        expected += "item" + std::to_string(i) + ";";
    }
    Require(body == expected, "Live TCP chunked body matches");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        std::function<void()> run;
    };

    const std::vector<TestCase> tests = {
        {"StreamChannelBoundedBackpressure", TestStreamChannelBoundedBackpressure},
        {"StreamChannelBlockingPushAndSlowConsumer",
         TestStreamChannelBlockingPushAndSlowConsumer},
        {"StreamChannelCancellationAndError",
         TestStreamChannelCancellationAndError},
        {"ChunkedHttpCodecRoundtripAndIncrementalSplits",
         TestChunkedHttpCodecRoundtripAndIncrementalSplits},
        {"SseCodecEncoding", TestSseCodecEncoding},
        {"StreamHubFanOutSlowConsumerAndPruning",
         TestStreamHubFanOutSlowConsumerAndPruning},
        {"HttpChunkedStreamPumpAndBackpressure",
         TestHttpChunkedStreamPumpAndBackpressure},
        {"LiveTcpChunkedStreamOverLoopback", TestLiveTcpChunkedStreamOverLoopback}
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            return 1;
        }
    }

    std::cout
        << "Lamp streaming: "
        << passed << "/" << tests.size()
        << " tests passed\n";
    return 0;
}
