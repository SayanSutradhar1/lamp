
#include "serialization/codec_negotiator.hpp"
#include "serialization/json_codec.hpp"
#include "serialization/operation_codec_adapter.hpp"
#include "serialization/protobuf_codec.hpp"
#include "serialization/protobuf_wire.hpp"
#include "serialization/schema_registry.hpp"
#include "serialization/serialization_value.hpp"

#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace bridge = lamp::bridge;
namespace ser = lamp::serialization;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::shared_ptr<ser::SchemaRegistry> BuildTestSchemas() {
    auto registry = std::make_shared<ser::SchemaRegistry>();

    ser::MessageSchema addressSchema;
    addressSchema.ref = {"common.Address", 1};
    addressSchema.fields = {
        {1, "city", ser::FieldType::String, true},
        {2, "zip", ser::FieldType::UInt64, false}
    };
    Require(registry->Register(std::move(addressSchema)).Ok(), "Address schema");

    ser::MessageSchema userSchema;
    userSchema.ref = {"users.User", 1};
    userSchema.fields = {
        {1, "id", ser::FieldType::SInt64, true},
        {2, "name", ser::FieldType::String, true, false, true,
         std::nullopt, {}, 1, 64},
        {3, "active", ser::FieldType::Bool, false, false, true,
         ser::Value(true)},
        {4, "score", ser::FieldType::Double, false, false, true,
         std::nullopt, {}, std::nullopt, std::nullopt, 0.0, 100.0},
        {5, "avatar", ser::FieldType::Bytes, false},
        {6, "tags", ser::FieldType::String, false, true},
        {7, "scores", ser::FieldType::Int64, false, true, true},
        {8, "address", ser::FieldType::Message, false, false, true,
         std::nullopt, {"common.Address", 1}},
        {9, "uint_id", ser::FieldType::UInt64, false}
    };
    Require(registry->Register(std::move(userSchema)).Ok(), "User schema");

    return registry;
}

void TestProtobufWirePrimitivesAndZigZag() {
    ser::ProtobufWriter w;
    w.WriteTag(1, ser::WireType::Varint);
    w.WriteZigZagVarint(-123456789LL);
    w.WriteTag(2, ser::WireType::Fixed64);
    w.WriteDouble(3.14159265358979);
    w.WriteTag(3, ser::WireType::Fixed32);
    w.WriteFixed32(0x12345678U);
    w.WriteTag(4, ser::WireType::LengthDelimited);
    w.WriteString("hello");

    ser::ProtobufReader r(w.Data());
    std::uint32_t field = 0;
    ser::WireType wire = ser::WireType::Varint;

    Require(r.ReadTag(field, wire) && field == 1 && wire == ser::WireType::Varint, "Tag 1");
    std::uint64_t zz = 0;
    Require(r.ReadVarint(zz) && ser::ZigZagDecode64(zz) == -123456789LL, "ZigZag decode");

    Require(r.ReadTag(field, wire) && field == 2 && wire == ser::WireType::Fixed64, "Tag 2");
    double d = 0.0;
    Require(r.ReadDouble(d) && d == 3.14159265358979, "Double decode");

    Require(r.ReadTag(field, wire) && field == 3 && wire == ser::WireType::Fixed32, "Tag 3");
    std::uint32_t f32 = 0;
    Require(r.ReadFixed32(f32) && f32 == 0x12345678U, "Fixed32 decode");

    Require(r.ReadTag(field, wire) && field == 4 && wire == ser::WireType::LengthDelimited, "Tag 4");
    std::span<const std::uint8_t> strSlice;
    Require(r.ReadLengthDelimited(strSlice), "String slice");
    Require(
        std::string_view(
            reinterpret_cast<const char*>(strSlice.data()),
            strSlice.size()
        ) == "hello",
        "String value"
    );
    Require(r.AtEnd(), "Reader at end");

    // Reject invalid wire type (StartGroup = 3) and overlong varint
    const std::vector<std::uint8_t> badWire = { (1U << 3U) | 3U };
    ser::ProtobufReader badReader(badWire);
    Require(!badReader.ReadTag(field, wire), "Group wire type rejected");

    const std::vector<std::uint8_t> overlong(11, 0x80);
    ser::ProtobufReader overlongReader(overlong);
    Require(!overlongReader.ReadVarint(zz), "Overlong varint rejected");
}

void TestProtobufCodecNestedRepeatedPackedAndBinarySafe() {
    auto registry = BuildTestSchemas();
    auto userSchema = registry->Find("users.User", 1);
    Require(userSchema != nullptr, "Find user schema");

    ser::Value::Bytes binaryAvatar{0x00, 0xFF, 0x10, 0x00, 0xAB, 0xCD};

    ser::Value addr = ser::Value::MakeObject();
    addr.Set("city", "Zurich");
    addr.Set("zip", std::uint64_t{8001});

    ser::Value user = ser::Value::MakeObject();
    user.Set("id", std::int64_t{-42});
    user.Set("name", "Ada Lovelace");
    user.Set("active", true);
    user.Set("score", 98.5);
    user.Set("avatar", binaryAvatar);
    user.Set("tags", ser::Value::Array{ser::Value("eng"), ser::Value("math")});
    user.Set(
        "scores",
        ser::Value::Array{ser::Value(-10), ser::Value(0), ser::Value(99)}
    );
    user.Set("address", std::move(addr));
    user.Set("uint_id", std::uint64_t{18446744073709551615ULL});

    const auto encoded = ser::ProtobufCodec::Encode(
        *userSchema,
        user,
        registry.get()
    );
    Require(encoded.Ok(), "Protobuf encode succeeded");
    Require(!encoded.value->empty(), "Non-empty wire output");

    const auto decoded = ser::ProtobufCodec::Decode(
        *userSchema,
        *encoded.value,
        registry.get()
    );
    Require(decoded.Ok(), "Protobuf decode succeeded");

    const auto& out = *decoded.value;
    Require(out.Find("id")->AsInt64() == -42, "Negative sint64 preserved");
    Require(out.Find("name")->AsString() == "Ada Lovelace", "String preserved");
    Require(out.Find("active")->AsBool() == true, "Bool preserved");
    Require(out.Find("score")->AsDouble() == 98.5, "Double preserved");
    Require(*out.Find("avatar")->AsBytes() == binaryAvatar, "Binary bytes with nulls preserved");
    Require(out.Find("uint_id")->AsUInt64() == 18446744073709551615ULL, "Max uint64 preserved");

    const auto* tags = out.Find("tags")->AsArray();
    Require(tags && tags->size() == 2 && (*tags)[0].AsString() == "eng" &&
                (*tags)[1].AsString() == "math",
            "Repeated strings preserved");

    const auto* scores = out.Find("scores")->AsArray();
    Require(scores && scores->size() == 3 &&
                (*scores)[0].AsInt64() == -10 &&
                (*scores)[1].AsInt64() == 0 &&
                (*scores)[2].AsInt64() == 99,
            "Packed repeated int64 preserved");

    const auto* outAddr = out.Find("address");
    Require(outAddr && outAddr->Find("city")->AsString() == "Zurich" &&
                outAddr->Find("zip")->AsUInt64() == 8001,
            "Nested message preserved");
}

void TestProtobufUnknownFieldHandling() {
    ser::MessageSchema strictSchema;
    strictSchema.ref = {"test.Msg", 1};
    strictSchema.allow_unknown_fields = false;
    strictSchema.fields = {
        {1, "known", ser::FieldType::String, false}
    };

    ser::MessageSchema permissiveSchema = strictSchema;
    permissiveSchema.allow_unknown_fields = true;

    ser::ProtobufWriter w;
    w.WriteTag(1, ser::WireType::LengthDelimited);
    w.WriteString("ok");
    w.WriteTag(99, ser::WireType::Varint);
    w.WriteVarint(777);
    w.WriteTag(100, ser::WireType::Fixed32);
    w.WriteFixed32(42);
    w.WriteTag(101, ser::WireType::LengthDelimited);
    w.WriteString("skip_me");

    const auto strictRes =
        ser::ProtobufCodec::Decode(strictSchema, w.Data());
    Require(
        !strictRes.Ok() &&
            strictRes.error.code == ser::CodecErrorCode::SchemaViolation,
        "Strict schema rejects unknown field"
    );

    const auto permRes =
        ser::ProtobufCodec::Decode(permissiveSchema, w.Data());
    Require(permRes.Ok(), "Permissive schema skips unknown fields");
    Require(permRes.value->Find("known")->AsString() == "ok", "Known field decoded");
}

void TestProtobufMalformedAndMaxDepth() {
    auto registry = BuildTestSchemas();
    auto userSchema = registry->Find("users.User", 1);

    // Truncated length-delimited payload
    const std::vector<std::uint8_t> truncated = {
        static_cast<std::uint8_t>((2U << 3U) | 2U),
        10,
        'a', 'b'
    };
    const auto resTrunc = ser::ProtobufCodec::Decode(
        *userSchema,
        truncated,
        registry.get()
    );
    Require(
        !resTrunc.Ok() &&
            resTrunc.error.code == ser::CodecErrorCode::MalformedWireData,
        "Truncated payload rejected"
    );

    // Self-recursive schema depth limit check
    auto recReg = std::make_shared<ser::SchemaRegistry>();
    ser::MessageSchema nodeSchema;
    nodeSchema.ref = {"test.Node", 1};
    nodeSchema.fields = {
        {1, "next", ser::FieldType::Message, false, false, true,
         std::nullopt, {"test.Node", 1}}
    };
    Require(recReg->Register(nodeSchema).Ok(), "Recursive schema");

    ser::Value deep = ser::Value::MakeObject();
    for (int i = 0; i < 6; ++i) {
        ser::Value next = ser::Value::MakeObject();
        next.Set("next", std::move(deep));
        deep = std::move(next);
    }

    ser::ProtobufCodecOptions depthOpts;
    depthOpts.max_depth = 3;
    const auto depthRes = ser::ProtobufCodec::Encode(
        nodeSchema,
        deep,
        recReg.get(),
        depthOpts
    );
    Require(!depthRes.Ok(), "Max depth enforced");
}

void TestJsonCodecRoundtripAndEscaping() {
    auto registry = BuildTestSchemas();
    auto userSchema = registry->Find("users.User", 1);

    ser::Value::Bytes binaryData{0x00, 0x01, 0xFE, 0xFF};
    ser::Value addr = ser::Value::MakeObject();
    addr.Set("city", "Tokyo\n\t\"\\");

    ser::Value user = ser::Value::MakeObject();
    user.Set("id", std::int64_t{-7});
    user.Set("name", "Bob");
    user.Set("score", 75.25);
    user.Set("avatar", binaryData);
    user.Set("address", std::move(addr));

    const auto encoded = ser::JsonCodec::EncodeWithSchema(
        *userSchema,
        user,
        registry.get()
    );
    Require(encoded.Ok(), "JSON encode succeeded");

    const auto decoded = ser::JsonCodec::DecodeWithSchema(
        *userSchema,
        *encoded.value,
        registry.get()
    );
    Require(decoded.Ok(), "JSON schema decode succeeded");

    const auto& out = *decoded.value;
    Require(out.Find("id")->AsInt64() == -7, "JSON int64 preserved");
    Require(out.Find("active")->AsBool() == true, "JSON default applied");
    Require(out.Find("score")->AsDouble() == 75.25, "JSON double preserved");
    Require(*out.Find("avatar")->AsBytes() == binaryData, "JSON base64 bytes roundtrip");
    Require(
        out.Find("address")->Find("city")->AsString() == "Tokyo\n\t\"\\",
        "JSON escaped string roundtrip"
    );

    // Surrogate pair decoding (\uD83D\uDE00 -> UTF-8 F0 9F 98 80)
    const auto emojiRes = ser::JsonCodec::DecodeString(R"({"emoji":"\uD83D\uDE00"})");
    Require(emojiRes.Ok(), "Surrogate pair parsed");
    const auto emojiStr = emojiRes.value->Find("emoji")->AsString();
    Require(
        emojiStr &&
            emojiStr->size() == 4 &&
            static_cast<unsigned char>((*emojiStr)[0]) == 0xF0 &&
            static_cast<unsigned char>((*emojiStr)[1]) == 0x9F &&
            static_cast<unsigned char>((*emojiStr)[2]) == 0x98 &&
            static_cast<unsigned char>((*emojiStr)[3]) == 0x80,
        "Surrogate pair converted to UTF-8"
    );
}

void TestJsonCodecRejection() {
    Require(
        !ser::JsonCodec::DecodeString(R"({"a":1} trailing)").Ok(),
        "Trailing characters rejected"
    );
    Require(
        !ser::JsonCodec::DecodeString(R"({"a":01})").Ok(),
        "Leading zero rejected"
    );
    Require(
        !ser::JsonCodec::DecodeString("{\"a\":\"\x01\"}").Ok(),
        "Unescaped control char rejected"
    );
    Require(
        !ser::JsonCodec::DecodeString(R"({"a":"\uD800"})").Ok(),
        "Lone high surrogate rejected"
    );

    ser::Value badDouble(std::numeric_limits<double>::quiet_NaN());
    Require(!ser::JsonCodec::Encode(badDouble).Ok(), "NaN rejected");

    ser::JsonCodecOptions smallDepth;
    smallDepth.max_depth = 2;
    Require(
        !ser::JsonCodec::DecodeString("[[[1]]]", smallDepth).Ok(),
        "JSON max_depth enforced"
    );
}

void TestSchemaRegistryValidationAndConstraints() {
    ser::SchemaRegistry reg;

    // Reserved Protobuf field number rejected
    ser::MessageSchema badReserved;
    badReserved.ref = {"bad.Reserved", 1};
    badReserved.fields = {{19000, "x", ser::FieldType::Int64}};
    Require(
        reg.Register(badReserved).status == ser::SchemaStatus::InvalidSchema,
        "Reserved field number rejected"
    );

    // Duplicate field number rejected
    ser::MessageSchema badDup;
    badDup.ref = {"bad.Dup", 1};
    badDup.fields = {
        {1, "a", ser::FieldType::Int64},
        {1, "b", ser::FieldType::Int64}
    };
    Require(
        reg.Register(badDup).status == ser::SchemaStatus::InvalidSchema,
        "Duplicate field number rejected"
    );

    auto fullReg = BuildTestSchemas();
    ser::Value invalidUser = ser::Value::MakeObject();
    invalidUser.Set("id", std::int64_t{1});
    invalidUser.Set("name", ""); // violates min_length = 1
    auto errLen = fullReg->Validate({"users.User", 1}, invalidUser);
    Require(
        errLen && errLen->code == ser::SchemaValidationCode::LengthConstraint,
        "Min length enforced"
    );

    invalidUser.Set("name", "Valid");
    invalidUser.Set("score", 150.0); // violates max_value = 100.0
    auto errRange = fullReg->Validate({"users.User", 1}, invalidUser);
    Require(
        errRange && errRange->code == ser::SchemaValidationCode::RangeConstraint,
        "Max value enforced"
    );

    // Missing required field
    ser::Value missingReq = ser::Value::MakeObject();
    missingReq.Set("name", "NoId");
    auto errReq = fullReg->Validate({"users.User", 1}, missingReq);
    Require(
        errReq && errReq->code == ser::SchemaValidationCode::MissingRequiredField,
        "Required field enforced"
    );
}

void TestMediaTypeAndProtobufFirstNegotiation() {
    using Fmt = bridge::SerializationFormat;

    // Protobuf-first defaults
    const auto inDefault = ser::CodecNegotiator::NegotiateInput(
        std::nullopt,
        Fmt::Auto,
        true
    );
    Require(inDefault.Ok() && inDefault.format == Fmt::Protobuf,
            "Auto input defaults to Protobuf");

    const auto outDefault = ser::CodecNegotiator::NegotiateOutput(
        std::nullopt,
        Fmt::Auto
    );
    Require(outDefault.Ok() && outDefault.format == Fmt::Protobuf,
            "Auto output defaults to Protobuf");

    const auto outWildcard = ser::CodecNegotiator::NegotiateOutput(
        "*/*",
        Fmt::Auto
    );
    Require(outWildcard.Ok() && outWildcard.format == Fmt::Protobuf,
            "Wildcard Accept picks Protobuf first");

    // Quality weights prefer JSON when explicitly weighted higher
    const auto outJsonPreferred = ser::CodecNegotiator::NegotiateOutput(
        "application/x-protobuf;q=0.5, application/json;q=0.9",
        Fmt::Auto
    );
    Require(outJsonPreferred.Ok() && outJsonPreferred.format == Fmt::Json,
            "Quality factor selects JSON fallback");

    // Equal quality ties break in favor of Protobuf
    const auto outTie = ser::CodecNegotiator::NegotiateOutput(
        "application/json;q=0.8, application/x-protobuf;q=0.8",
        Fmt::Auto
    );
    Require(outTie.Ok() && outTie.format == Fmt::Protobuf,
            "Equal quality tie breaks to Protobuf");

    // Content-Type mismatch against strict descriptor format
    const auto inMismatch = ser::CodecNegotiator::NegotiateInput(
        "application/json; charset=utf-8",
        Fmt::Protobuf,
        true
    );
    Require(
        inMismatch.status == ser::NegotiationStatus::UnsupportedMediaType,
        "Content-Type mismatch returns UnsupportedMediaType"
    );

    // Accept exclusion returns NotAcceptable
    const auto outExcluded = ser::CodecNegotiator::NegotiateOutput(
        "application/json",
        Fmt::Protobuf
    );
    Require(
        outExcluded.status == ser::NegotiationStatus::NotAcceptable,
        "Excluded Accept format returns NotAcceptable"
    );
}

void TestPayloadCodecAndExecutionDispatcherIntegration() {
    auto schemas = BuildTestSchemas();

    bridge::OperationDescriptor desc;
    desc.id = "users.echo";
    desc.execution.mode = bridge::ExecutionMode::Native;
    desc.input_format = bridge::SerializationFormat::Auto;
    desc.output_format = bridge::SerializationFormat::Auto;
    desc.input_schema = {"users.User", 1};
    desc.output_schema = {"users.User", 1};

    auto handler = std::make_shared<ser::SchemaAwareNativeHandler>(
        schemas,
        desc,
        [](const bridge::RequestEnvelope&, const ser::Value& input) {
            ser::Value out = input;
            out.Set("name", std::string(input.Find("name")->AsString().value()) + "!");
            return ser::StructuredOperationOutcome::Success(std::move(out));
        }
    );

    auto opRegistry = std::make_shared<bridge::OperationRegistry>();
    bridge::OperationBackends backends;
    backends.native = handler;
    Require(opRegistry->Register(desc, std::move(backends)).Ok(), "Op registered");

    bridge::ExecutionDispatcher dispatcher(opRegistry);

    ser::Value user = ser::Value::MakeObject();
    user.Set("id", std::int64_t{101});
    user.Set("name", "Grace");
    user.Set("avatar", ser::Value::Bytes{0x00, 0x7F, 0xFF});

    const auto userSchema = schemas->Find("users.User", 1);
    const auto pbBytes = ser::ProtobufCodec::Encode(*userSchema, user, schemas.get());
    Require(pbBytes.Ok(), "Encode request protobuf");

    // 1. Protobuf in -> Protobuf out
    bridge::RequestEnvelope pbReq;
    pbReq.request_id = 1;
    pbReq.operation_id = "users.echo";
    pbReq.transport = bridge::TransportKind::Http;
    pbReq.format = bridge::SerializationFormat::Protobuf;
    pbReq.payload = *pbBytes.value;

    const auto pbRes = dispatcher.Dispatch(pbReq);
    Require(pbRes.IsSuccess(), "Protobuf dispatch succeeded");
    Require(pbRes.format == bridge::SerializationFormat::Protobuf, "Protobuf response format");
    const auto decodedPb = ser::ProtobufCodec::Decode(*userSchema, pbRes.payload, schemas.get());
    Require(decodedPb.Ok() && decodedPb.value->Find("name")->AsString() == "Grace!",
            "Protobuf roundtrip through ExecutionDispatcher");

    // 2. Protobuf in -> JSON out via Accept negotiation metadata
    pbReq.request_id = 2;
    pbReq.AddMetadata("lamp.accept", "application/json");
    const auto crossRes = dispatcher.Dispatch(pbReq);
    Require(crossRes.IsSuccess(), "Cross-codec dispatch succeeded");
    Require(crossRes.format == bridge::SerializationFormat::Json, "Negotiated JSON response");
    const auto decodedJson = ser::JsonCodec::DecodeWithSchema(
        *userSchema,
        crossRes.payload,
        schemas.get()
    );
    Require(decodedJson.Ok() &&
                decodedJson.value->Find("name")->AsString() == "Grace!" &&
                *decodedJson.value->Find("avatar")->AsBytes() ==
                    (ser::Value::Bytes{0x00, 0x7F, 0xFF}),
            "Cross-codec binary bytes & value preserved");

    // 3. Invalid schema input rejected cleanly
    bridge::RequestEnvelope badJsonReq;
    badJsonReq.request_id = 3;
    badJsonReq.operation_id = "users.echo";
    badJsonReq.transport = bridge::TransportKind::Http;
    badJsonReq.format = bridge::SerializationFormat::Json;
    const std::string badJson = R"({"id":1,"name":""})";
    badJsonReq.payload.assign(badJson.begin(), badJson.end());
    const auto badRes = dispatcher.Dispatch(badJsonReq);
    Require(
        badRes.IsFailure() &&
            badRes.error.code == bridge::OperationErrorCode::InvalidArgument,
        "Schema violation mapped to InvalidArgument"
    );

    // 4. Binary passthrough format preserves arbitrary bytes
    bridge::OperationDescriptor binDesc;
    binDesc.id = "raw.echo";
    binDesc.execution.mode = bridge::ExecutionMode::Native;
    binDesc.input_format = bridge::SerializationFormat::Binary;
    binDesc.output_format = bridge::SerializationFormat::Binary;
    const ser::Value::Bytes rawBytes{0x00, 0x01, 0x80, 0xFF};
    const auto binDecoded = ser::PayloadCodec::DecodeRequest(
        schemas.get(),
        binDesc,
        bridge::SerializationFormat::Binary,
        rawBytes
    );
    Require(binDecoded.Ok() && *binDecoded.value->AsBytes() == rawBytes,
            "Binary decode passthrough");
    const auto binEncoded = ser::PayloadCodec::EncodeResponse(
        schemas.get(),
        binDesc,
        bridge::SerializationFormat::Binary,
        *binDecoded.value
    );
    Require(binEncoded.Ok() && *binEncoded.value == rawBytes,
            "Binary encode passthrough");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        std::function<void()> run;
    };

    const std::vector<TestCase> tests = {
        {"ProtobufWirePrimitivesAndZigZag", TestProtobufWirePrimitivesAndZigZag},
        {"ProtobufCodecNestedRepeatedPackedAndBinarySafe",
         TestProtobufCodecNestedRepeatedPackedAndBinarySafe},
        {"ProtobufUnknownFieldHandling", TestProtobufUnknownFieldHandling},
        {"ProtobufMalformedAndMaxDepth", TestProtobufMalformedAndMaxDepth},
        {"JsonCodecRoundtripAndEscaping", TestJsonCodecRoundtripAndEscaping},
        {"JsonCodecRejection", TestJsonCodecRejection},
        {"SchemaRegistryValidationAndConstraints",
         TestSchemaRegistryValidationAndConstraints},
        {"MediaTypeAndProtobufFirstNegotiation",
         TestMediaTypeAndProtobufFirstNegotiation},
        {"PayloadCodecAndExecutionDispatcherIntegration",
         TestPayloadCodecAndExecutionDispatcherIntegration}
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
        << "Lamp serialization: "
        << passed << "/" << tests.size()
        << " tests passed\n";
    return 0;
}
