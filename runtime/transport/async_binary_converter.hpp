#pragma once

// ============================================================
// Lamp Framework
// Native Binary Transport Layer - Async Streaming Converter
//
// File: runtime/transport/async_binary_converter.hpp
//
// Purpose:
//   Converts JSON inputs and binary file attachments into
//   LCB1 compact binary streams asynchronously on the dynamic
//   C++ worker pool without blocking callers or event loops.
// ============================================================

#include "core/dynamic_worker_pool.hpp"
#include "serialization/json_codec.hpp"
#include "streaming/stream_channel.hpp"
#include "transport/lamp_binary_protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace lamp::transport {

struct AsyncBinaryConversionRequest final {
    std::string operation_id;
    std::string json_input;
    std::vector<LampFilePayload> files;
    std::size_t chunk_bytes = 16384;
};

struct AsyncBinaryConversionStats final {
    std::size_t json_input_bytes = 0;
    std::size_t file_input_bytes = 0;
    std::size_t binary_output_bytes = 0;
    std::size_t chunks_emitted = 0;
    bool ok = false;
    std::string error;
};

class AsyncBinaryStreamConverter final {
public:
    explicit AsyncBinaryStreamConverter(
        std::shared_ptr<core::DynamicWorkerPool> pool
    )
        : pool_(std::move(pool)) {}

    // Begins non-blocking async conversion, returning immediately with both:
    //   1) A bounded StreamChannel receiving LCB1 binary chunks as produced
    //   2) A future yielding final AsyncBinaryConversionStats on completion
    [[nodiscard]]
    std::pair<
        std::shared_ptr<streaming::StreamChannel>,
        std::future<AsyncBinaryConversionStats>
    >
    ConvertToBinaryStreamAsync(AsyncBinaryConversionRequest req) {
        const std::size_t chunk_sz =
            (std::max)(std::size_t{256}, req.chunk_bytes);

        streaming::StreamConfig sc;
        sc.max_buffered_chunks = 64;
        sc.max_buffered_bytes = chunk_sz * 64U;
        sc.max_chunk_bytes = chunk_sz;
        auto channel = std::make_shared<streaming::StreamChannel>(sc);

        auto fut = pool_->Submit([channel, req = std::move(req), chunk_sz]() mutable {
            AsyncBinaryConversionStats stats;
            stats.json_input_bytes = req.json_input.size();
            for (const auto& f : req.files) {
                stats.file_input_bytes += f.data.size();
            }

            serialization::Value input_val = serialization::Value::MakeNull();
            if (!req.json_input.empty()) {
                auto decoded = serialization::JsonCodec::DecodeString(req.json_input);
                if (!decoded.Ok() || !decoded.value) {
                    stats.error = decoded.error.message.empty()
                        ? "Invalid JSON input"
                        : decoded.error.message;
                    channel->Fail(bridge::OperationError{
                        bridge::OperationErrorCode::InvalidArgument,
                        stats.error,
                        "JSON_DECODE_FAILED",
                        false
                    });
                    return stats;
                }
                input_val = std::move(*decoded.value);
            }

            auto binary = LcbCodec::EncodeRpcRequest(
                req.operation_id,
                input_val,
                req.files
            );
            stats.binary_output_bytes = binary.size();

            std::size_t offset = 0;
            while (offset < binary.size()) {
                const std::size_t len = (std::min)(chunk_sz, binary.size() - offset);
                streaming::StreamChunk chunk;
                chunk.data.assign(
                    binary.begin() + static_cast<std::ptrdiff_t>(offset),
                    binary.begin() + static_cast<std::ptrdiff_t>(offset + len)
                );
                if (channel->Push(std::move(chunk)) !=
                    streaming::StreamPushStatus::Accepted) {
                    stats.error = "Binary stream consumer cancelled or closed";
                    return stats;
                }
                ++stats.chunks_emitted;
                offset += len;
            }

            channel->Complete();
            stats.ok = true;
            return stats;
        });

        return {channel, std::move(fut)};
    }

private:
    std::shared_ptr<core::DynamicWorkerPool> pool_;
};

} // namespace lamp::transport
