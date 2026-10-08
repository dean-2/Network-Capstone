#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_header_serialization() {
    const bfp::FrameHeader header{
        bfp::kMagic,
        bfp::kVersion,
        static_cast<std::uint8_t>(bfp::FrameType::Request),
        14,
        1,
    };
    const auto bytes = bfp::serialize_header(header);
    const std::array<std::uint8_t, bfp::kFrameHeaderSize> expected{
        0x42, 0x54, 0x01, 0x01, 0x00, 0x00,
        0x00, 0x0e, 0x00, 0x00, 0x00, 0x01,
    };

    check(bytes.size() == 12, "serialized header is exactly 12 bytes");
    check(bytes == expected, "header fields are serialized in big-endian order");

    const auto decoded = bfp::deserialize_header(bytes);
    check(decoded.magic == header.magic, "magic round-trips");
    check(decoded.version == header.version, "version round-trips");
    check(decoded.type == header.type, "frame type round-trips");
    check(decoded.payload_length == header.payload_length, "payload length round-trips");
    check(decoded.request_id == header.request_id, "request ID round-trips");
    check(bfp::valid_header(decoded), "valid magic and version are accepted");
}

void test_request_encoding_and_decoding() {
    const bfp::Request request{"/index.html"};
    std::vector<std::uint8_t> payload;
    std::string error;
    check(bfp::encode_request_payload(request, payload, error), "GET request encodes");

    const std::vector<std::uint8_t> expected{
        0x01, 0x00, 0x0b, 0x2f, 0x69, 0x6e, 0x64, 0x65,
        0x78, 0x2e, 0x68, 0x74, 0x6d, 0x6c,
    };
    check(payload == expected, "GET payload bytes match the corrected specification");

    bfp::Request decoded;
    check(bfp::decode_request_payload(payload, decoded, error), "valid request payload decodes");
    check(decoded.path == request.path, "request path round-trips");

    check(!bfp::decode_request_payload({}, decoded, error), "empty request payload is malformed");
    auto bad_method = payload;
    bad_method[0] = 0x02;
    check(!bfp::decode_request_payload(bad_method, decoded, error), "non-GET method is malformed");
    auto bad_length = payload;
    bad_length[2] = 0x0a;
    check(!bfp::decode_request_payload(bad_length, decoded, error), "wrong path length is malformed");
    auto bad_path = payload;
    bad_path[3] = 'x';
    check(!bfp::decode_request_payload(bad_path, decoded, error), "path without leading slash is malformed");
    auto bad_utf8 = payload;
    bad_utf8.back() = 0xff;
    check(!bfp::decode_request_payload(bad_utf8, decoded, error), "invalid UTF-8 path is malformed");
    check(!bfp::encode_request_payload({"relative"}, payload, error), "encoder rejects a path without slash");
}

void test_response_encoding_and_decoding() {
    bfp::Response response;
    response.status = 200;
    response.content_type = "text/html";
    response.body = {'H', 'e', 'l', 'l', 'o'};

    std::vector<std::uint8_t> payload;
    std::string error;
    check(bfp::encode_response_payload(response, payload, error), "200 response encodes");
    const std::vector<std::uint8_t> expected_payload{
        0x00, 0xc8, 0x02,
        0x01, 0x00, 0x01, 0x35,
        0x02, 0x00, 0x09, 0x74, 0x65, 0x78, 0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c,
        0x48, 0x65, 0x6c, 0x6c, 0x6f,
    };
    check(payload == expected_payload, "response payload matches the specification hexdump");

    const auto frame = bfp::make_frame(static_cast<std::uint8_t>(bfp::FrameType::Response), 1, payload);
    const std::vector<std::uint8_t> expected_frame{
        0x42, 0x54, 0x01, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x01,
        0x00, 0xc8, 0x02, 0x01, 0x00, 0x01, 0x35,
        0x02, 0x00, 0x09, 0x74, 0x65, 0x78, 0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c,
        0x48, 0x65, 0x6c, 0x6c, 0x6f,
    };
    check(frame == expected_frame, "complete response frame matches the specification hexdump");
    check(frame.size() == bfp::kFrameHeaderSize + 24, "response frame has a 24-byte payload");

    bfp::Response decoded;
    check(bfp::decode_response_payload(payload, decoded, error), "response payload decodes");
    check(decoded.status == 200, "response status round-trips");
    check(decoded.content_type == "text/html", "Content-Type round-trips");
    check(decoded.body == response.body, "response body round-trips");

    auto broken = payload;
    broken[6] = 0x36;
    check(!bfp::decode_response_payload(broken, decoded, error), "incorrect Content-Length is rejected");
    check(!bfp::encode_response_payload({200, "", {}}, payload, error), "200 requires Content-Type");
}

void test_partial_reads() {
    int sockets[2]{};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair for partial read test");

    const auto payload = std::vector<std::uint8_t>{0x01, 0x00, 0x0b, '/', 'i', 'n', 'd', 'e', 'x', '.', 'h', 't', 'm', 'l'};
    const auto bytes = bfp::make_frame(static_cast<std::uint8_t>(bfp::FrameType::Request), 7, payload);
    std::thread writer([&] {
        std::string error;
        std::size_t offset = 0;
        const std::array<std::size_t, 5> chunks{1, 2, 3, 4, 5};
        for (const std::size_t requested : chunks) {
            if (offset == bytes.size()) break;
            const std::size_t amount = std::min(requested, bytes.size() - offset);
            bfp::write_all(sockets[0], bytes.data() + offset, amount, error);
            offset += amount;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (offset < bytes.size()) {
            bfp::write_all(sockets[0], bytes.data() + offset, bytes.size() - offset, error);
        }
        ::shutdown(sockets[0], SHUT_WR);
    });

    bfp::Frame frame;
    std::string error;
    const auto result = bfp::read_frame(sockets[1], frame, error);
    check(result == bfp::FrameReadStatus::Frame, "frame is read across partial socket writes");
    check(frame.header.request_id == 7 && frame.payload == payload, "partial read restores complete frame");
    writer.join();
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_partial_writes() {
    int sockets[2]{};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair for partial write test");
    int send_buffer = 4096;
    check(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0,
          "small send buffer is configured");

    std::vector<std::uint8_t> bytes(8 * 1024 * 1024);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i & 0xffU);
    }
    std::vector<std::uint8_t> received(bytes.size());
    std::thread reader([&] {
        std::string error;
        const auto status = bfp::read_exact(sockets[1], received.data(), received.size(), error);
        if (status != bfp::IoStatus::Complete) {
            std::cerr << "FAIL: reader did not receive all bytes\n";
            std::exit(1);
        }
    });

    std::string error;
    check(bfp::write_all(sockets[0], bytes.data(), bytes.size(), error), "write_all handles a large partial write");
    reader.join();
    check(received == bytes, "partial writes preserve all bytes");
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_unknown_frame_is_skipped() {
    int sockets[2]{};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair for unknown frame test");
    const auto unknown = bfp::make_frame(0x99, 55, {0xaa, 0xbb, 0xcc});
    const auto request = bfp::make_frame(
        static_cast<std::uint8_t>(bfp::FrameType::Request), 9,
        {0x01, 0x00, 0x01, '/'});
    std::vector<std::uint8_t> combined = unknown;
    combined.insert(combined.end(), request.begin(), request.end());
    std::string write_error;
    check(bfp::write_all(sockets[0], combined.data(), combined.size(), write_error), "unknown and known frames are sent");

    std::vector<std::uint8_t> skipped;
    bfp::Frame frame;
    std::string error;
    const auto result = bfp::read_frame(sockets[1], frame, error,
                                        [&](const auto& bytes) { skipped = bytes; });
    check(result == bfp::FrameReadStatus::Frame, "reader continues after an unknown frame");
    check(skipped == unknown, "unknown frame is consumed completely and observed intact");
    check(frame.header.request_id == 9, "next known frame is returned");
    ::close(sockets[0]);
    ::close(sockets[1]);
}

} // namespace

int main() {
    test_header_serialization();
    test_request_encoding_and_decoding();
    test_response_encoding_and_decoding();
    test_partial_reads();
    test_partial_writes();
    test_unknown_frame_is_skipped();
    std::cout << "protocol tests passed\n";
    return 0;
}
