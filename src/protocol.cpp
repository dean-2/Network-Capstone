#include "protocol.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <limits>
#include <new>
#include <stdexcept>
#include <sys/socket.h>

namespace bfp {
namespace {

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[1] = static_cast<std::uint8_t>(value & 0xffU);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(in[0]) << 8) |
        static_cast<std::uint16_t>(in[1]));
}

std::uint32_t get_u32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

bool valid_utf8(const std::string& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char first = bytes[i];
        if (first <= 0x7f) {
            ++i;
            continue;
        }

        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (i + count > value.size()) {
            return false;
        }
        for (std::size_t j = 1; j < count; ++j) {
            const unsigned char next = bytes[i + j];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3fU);
        }
        if ((count == 3 && codepoint < 0x800) ||
            (count == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) {
            return false;
        }
        i += count;
    }
    return true;
}

bool parse_decimal_size(const std::string& value, std::size_t& result) {
    if (value.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    result = static_cast<std::size_t>(parsed);
    return true;
}

void append_header(
    std::vector<std::uint8_t>& payload,
    std::uint8_t header_id,
    const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("response header value exceeds 16-bit length");
    }
    payload.push_back(header_id);
    append_u16(payload, static_cast<std::uint16_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
}

} // namespace

std::array<std::uint8_t, kFrameHeaderSize> serialize_header(const FrameHeader& header) {
    std::array<std::uint8_t, kFrameHeaderSize> bytes{};
    put_u16(bytes.data(), header.magic);
    bytes[2] = header.version;
    bytes[3] = header.type;
    put_u32(bytes.data() + 4, header.payload_length);
    put_u32(bytes.data() + 8, header.request_id);
    return bytes;
}

FrameHeader deserialize_header(const std::array<std::uint8_t, kFrameHeaderSize>& bytes) {
    FrameHeader header;
    header.magic = get_u16(bytes.data());
    header.version = bytes[2];
    header.type = bytes[3];
    header.payload_length = get_u32(bytes.data() + 4);
    header.request_id = get_u32(bytes.data() + 8);
    return header;
}

bool valid_header(const FrameHeader& header) {
    return header.magic == kMagic && header.version == kVersion;
}

IoStatus read_exact(int socket_fd, std::uint8_t* data, std::size_t length, std::string& error) {
    std::size_t received = 0;
    while (received < length) {
        const ssize_t amount = ::recv(socket_fd, data + received, length - received, 0);
        if (amount == 0) {
            return received == 0 ? IoStatus::EndOfStream : IoStatus::Truncated;
        }
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "recv failed";
            return IoStatus::Error;
        }
        received += static_cast<std::size_t>(amount);
    }
    return IoStatus::Complete;
}

bool write_all(int socket_fd, const std::uint8_t* data, std::size_t length, std::string& error) {
    std::size_t sent = 0;
    while (sent < length) {
        const ssize_t amount = ::send(socket_fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "send failed";
            return false;
        }
        if (amount == 0) {
            error = "send returned zero bytes";
            return false;
        }
        sent += static_cast<std::size_t>(amount);
    }
    return true;
}

FrameReadStatus read_frame(
    int socket_fd,
    Frame& frame,
    std::string& error,
    const std::function<void(const std::vector<std::uint8_t>&)>& skipped_frame_observer) {
    for (;;) {
        std::array<std::uint8_t, kFrameHeaderSize> header_bytes{};
        const IoStatus header_status = read_exact(socket_fd, header_bytes.data(), header_bytes.size(), error);
        if (header_status == IoStatus::EndOfStream) {
            return FrameReadStatus::EndOfStream;
        }
        if (header_status == IoStatus::Truncated) {
            error = "truncated frame header";
            return FrameReadStatus::Truncated;
        }
        if (header_status == IoStatus::Error) {
            return FrameReadStatus::Error;
        }

        const FrameHeader header = deserialize_header(header_bytes);
        if (!valid_header(header)) {
            error = "invalid frame magic or version";
            return FrameReadStatus::InvalidHeader;
        }

        const bool recognized = header.type == static_cast<std::uint8_t>(FrameType::Request) ||
                                 header.type == static_cast<std::uint8_t>(FrameType::Response);
        if (!recognized && !skipped_frame_observer) {
            std::array<std::uint8_t, 4096> discard{};
            std::uint32_t remaining = header.payload_length;
            while (remaining > 0) {
                const std::size_t amount = std::min<std::size_t>(remaining, discard.size());
                const IoStatus payload_status = read_exact(socket_fd, discard.data(), amount, error);
                if (payload_status != IoStatus::Complete) {
                    error = payload_status == IoStatus::Error ? error : "truncated unknown frame payload";
                    return payload_status == IoStatus::Error ? FrameReadStatus::Error : FrameReadStatus::Truncated;
                }
                remaining -= static_cast<std::uint32_t>(amount);
            }
            continue;
        }

        if (header.type == static_cast<std::uint8_t>(FrameType::Request) &&
            header.payload_length > kMaxRequestPayloadSize) {
            std::array<std::uint8_t, 4096> discard{};
            std::uint32_t remaining = header.payload_length;
            while (remaining > 0) {
                const std::size_t amount = std::min<std::size_t>(remaining, discard.size());
                const IoStatus payload_status = read_exact(socket_fd, discard.data(), amount, error);
                if (payload_status != IoStatus::Complete) {
                    error = payload_status == IoStatus::Error ? error : "truncated oversized request payload";
                    return payload_status == IoStatus::Error ? FrameReadStatus::Error : FrameReadStatus::Truncated;
                }
                remaining -= static_cast<std::uint32_t>(amount);
            }

            frame.header = header;
            frame.payload.clear();
            error = "request payload exceeds the format maximum";
            return FrameReadStatus::OversizedRequest;
        }

        std::vector<std::uint8_t> payload;
        try {
            payload.resize(header.payload_length);
        } catch (const std::bad_alloc&) {
            error = "unable to allocate frame payload";
            return FrameReadStatus::Error;
        } catch (const std::length_error&) {
            error = "frame payload is too large";
            return FrameReadStatus::Error;
        }
        if (!payload.empty()) {
            const IoStatus payload_status = read_exact(socket_fd, payload.data(), payload.size(), error);
            if (payload_status != IoStatus::Complete) {
                error = payload_status == IoStatus::Error ? error : "truncated frame payload";
                return payload_status == IoStatus::Error ? FrameReadStatus::Error : FrameReadStatus::Truncated;
            }
        }

        if (!recognized) {
            if (skipped_frame_observer) {
                std::vector<std::uint8_t> complete_frame;
                const auto serialized_header = serialize_header(header);
                complete_frame.insert(complete_frame.end(), serialized_header.begin(), serialized_header.end());
                complete_frame.insert(complete_frame.end(), payload.begin(), payload.end());
                skipped_frame_observer(complete_frame);
            }
            continue;
        }

        frame.header = header;
        frame.payload = std::move(payload);
        return FrameReadStatus::Frame;
    }
}

std::vector<std::uint8_t> make_frame(
    std::uint8_t frame_type,
    std::uint32_t request_id,
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("frame payload exceeds 32-bit length");
    }
    const FrameHeader header{
        kMagic,
        kVersion,
        frame_type,
        static_cast<std::uint32_t>(payload.size()),
        request_id,
    };
    const auto header_bytes = serialize_header(header);
    std::vector<std::uint8_t> frame_bytes(header_bytes.begin(), header_bytes.end());
    frame_bytes.insert(frame_bytes.end(), payload.begin(), payload.end());
    return frame_bytes;
}

bool encode_request_payload(const Request& request, std::vector<std::uint8_t>& payload, std::string& error) {
    if (request.path.empty() || request.path.front() != '/') {
        error = "request path must begin with '/'";
        return false;
    }
    if (request.path.size() > std::numeric_limits<std::uint16_t>::max()) {
        error = "request path exceeds 16-bit length";
        return false;
    }
    if (!valid_utf8(request.path)) {
        error = "request path is not valid UTF-8";
        return false;
    }
    payload.clear();
    payload.push_back(0x01);
    append_u16(payload, static_cast<std::uint16_t>(request.path.size()));
    payload.insert(payload.end(), request.path.begin(), request.path.end());
    return true;
}

bool decode_request_payload(const std::vector<std::uint8_t>& payload, Request& request, std::string& error) {
    if (payload.size() < 3) {
        error = "request payload is too short";
        return false;
    }
    if (payload[0] != 0x01) {
        error = "unsupported request method";
        return false;
    }
    const std::size_t path_length = get_u16(payload, 1);
    if (path_length == 0 || payload.size() != 3 + path_length) {
        error = "request path length does not match payload";
        return false;
    }
    request.path.assign(payload.begin() + 3, payload.end());
    if (request.path.front() != '/') {
        error = "request path must begin with '/'";
        return false;
    }
    if (!valid_utf8(request.path)) {
        error = "request path is not valid UTF-8";
        return false;
    }
    return true;
}

bool encode_response_payload(const Response& response, std::vector<std::uint8_t>& payload, std::string& error) {
    if (response.status != 200 && response.status != 400 && response.status != 404) {
        error = "response status is not defined by the protocol";
        return false;
    }
    if (response.status == 200 && response.content_type.empty()) {
        error = "successful response requires Content-Type";
        return false;
    }
    if (response.content_type.size() > std::numeric_limits<std::uint16_t>::max()) {
        error = "Content-Type exceeds 16-bit header length";
        return false;
    }

    payload.clear();
    append_u16(payload, response.status);
    payload.push_back(response.status == 200 ? 2 : 1);
    append_header(payload, 0x01, std::to_string(response.body.size()));
    if (response.status == 200) {
        append_header(payload, 0x02, response.content_type);
    }
    payload.insert(payload.end(), response.body.begin(), response.body.end());
    return true;
}

bool decode_response_payload(const std::vector<std::uint8_t>& payload, Response& response, std::string& error) {
    if (payload.size() < 3) {
        error = "response payload is too short";
        return false;
    }
    const std::uint16_t status = get_u16(payload, 0);
    const std::size_t header_count = payload[2];
    std::size_t offset = 3;
    bool have_content_length = false;
    bool have_content_type = false;
    std::size_t content_length = 0;
    std::string content_type;

    for (std::size_t i = 0; i < header_count; ++i) {
        if (offset + 3 > payload.size()) {
            error = "response header is truncated";
            return false;
        }
        const std::uint8_t id = payload[offset];
        const std::size_t value_length = get_u16(payload, offset + 1);
        offset += 3;
        if (value_length > payload.size() - offset) {
            error = "response header value is truncated";
            return false;
        }
        const std::string value(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                payload.begin() + static_cast<std::ptrdiff_t>(offset + value_length));
        offset += value_length;

        if (id == 0x01) {
            if (have_content_length || !parse_decimal_size(value, content_length)) {
                error = "invalid or duplicate Content-Length header";
                return false;
            }
            have_content_length = true;
        } else if (id == 0x02) {
            if (have_content_type) {
                error = "duplicate Content-Type header";
                return false;
            }
            content_type = value;
            have_content_type = true;
        } else {
            error = "unknown response header ID";
            return false;
        }
    }

    if (!have_content_length) {
        error = "response is missing Content-Length";
        return false;
    }
    if (status == 200 && !have_content_type) {
        error = "successful response is missing Content-Type";
        return false;
    }
    if (content_length != payload.size() - offset) {
        error = "Content-Length does not match response body";
        return false;
    }

    response.status = status;
    response.content_type = std::move(content_type);
    response.body.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end());
    return true;
}

std::string hexdump(const std::vector<std::uint8_t>& frame_bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (std::size_t i = 0; i < frame_bytes.size(); ++i) {
        if (i != 0 && i % 12 == 0) {
            result.push_back('\n');
        } else if (i != 0) {
            result.push_back(' ');
        }
        const std::uint8_t value = frame_bytes[i];
        result.push_back(digits[(value >> 4) & 0x0fU]);
        result.push_back(digits[value & 0x0fU]);
    }
    result.push_back('\n');
    return result;
}

} // namespace bfp
