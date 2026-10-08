#ifndef BINARY_FILE_SERVER_PROTOCOL_HPP
#define BINARY_FILE_SERVER_PROTOCOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bfp {

constexpr std::uint16_t kMagic = 0x4254;
constexpr std::uint8_t kVersion = 0x01;
constexpr std::size_t kFrameHeaderSize = 12;
constexpr std::size_t kMaxRequestPayloadSize = 1U + 2U + 0xffffU;

enum class FrameType : std::uint8_t {
    Request = 0x01,
    Response = 0x02,
};

struct FrameHeader {
    std::uint16_t magic = kMagic;
    std::uint8_t version = kVersion;
    std::uint8_t type = 0;
    std::uint32_t payload_length = 0;
    std::uint32_t request_id = 0;
};

std::array<std::uint8_t, kFrameHeaderSize> serialize_header(const FrameHeader& header);
FrameHeader deserialize_header(const std::array<std::uint8_t, kFrameHeaderSize>& bytes);
bool valid_header(const FrameHeader& header);

struct Frame {
    FrameHeader header;
    std::vector<std::uint8_t> payload;
};

enum class IoStatus {
    Complete,
    EndOfStream,
    Truncated,
    Error,
};

enum class FrameReadStatus {
    Frame,
    OversizedRequest,
    EndOfStream,
    Truncated,
    InvalidHeader,
    Error,
};

IoStatus read_exact(int socket_fd, std::uint8_t* data, std::size_t length, std::string& error);
bool write_all(int socket_fd, const std::uint8_t* data, std::size_t length, std::string& error);
FrameReadStatus read_frame(
    int socket_fd,
    Frame& frame,
    std::string& error,
    const std::function<void(const std::vector<std::uint8_t>&)>& skipped_frame_observer = {});
std::vector<std::uint8_t> make_frame(
    std::uint8_t frame_type,
    std::uint32_t request_id,
    const std::vector<std::uint8_t>& payload);

struct Request {
    std::string path;
};

bool encode_request_payload(const Request& request, std::vector<std::uint8_t>& payload, std::string& error);
bool decode_request_payload(const std::vector<std::uint8_t>& payload, Request& request, std::string& error);

struct Response {
    std::uint16_t status = 0;
    std::string content_type;
    std::vector<std::uint8_t> body;
};

bool encode_response_payload(const Response& response, std::vector<std::uint8_t>& payload, std::string& error);
bool decode_response_payload(const std::vector<std::uint8_t>& payload, Response& response, std::string& error);

std::string hexdump(const std::vector<std::uint8_t>& frame_bytes);

} // namespace bfp

#endif
