#include "file_server.hpp"
#include "protocol.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool parse_port(const char* text, std::uint16_t& port) {
    const std::string value(text);
    unsigned parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
        parsed == 0 || parsed > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

bool send_response(int client_fd, std::uint32_t request_id, const bfp::Response& response) {
    std::vector<std::uint8_t> payload;
    std::string error;
    if (!bfp::encode_response_payload(response, payload, error)) {
        std::cerr << "unable to encode response: " << error << '\n';
        return false;
    }

    try {
        const auto frame = bfp::make_frame(
            static_cast<std::uint8_t>(bfp::FrameType::Response), request_id, payload);
        if (!bfp::write_all(client_fd, frame.data(), frame.size(), error)) {
            std::cerr << "unable to send response: " << error << '\n';
            return false;
        }
    } catch (const std::exception& exception) {
        std::cerr << "unable to build response frame: " << exception.what() << '\n';
        return false;
    }
    return true;
}

void serve_connection(int client_fd, int root_fd) {
    for (;;) {
        bfp::Frame frame;
        std::string error;
        const bfp::FrameReadStatus read_status = bfp::read_frame(client_fd, frame, error);
        if (read_status == bfp::FrameReadStatus::EndOfStream) {
            return;
        }
        if (read_status != bfp::FrameReadStatus::Frame &&
            read_status != bfp::FrameReadStatus::OversizedRequest) {
            if (!error.empty()) {
                std::cerr << "closing connection: " << error << '\n';
            }
            return;
        }

        bfp::Response response;
        if (read_status == bfp::FrameReadStatus::OversizedRequest ||
            frame.header.type != static_cast<std::uint8_t>(bfp::FrameType::Request)) {
            response.status = 400;
            const std::string message = "Bad Request\n";
            response.body.assign(message.begin(), message.end());
        } else {
            bfp::Request request;
            if (!bfp::decode_request_payload(frame.payload, request, error)) {
                response.status = 400;
                const std::string message = "Bad Request\n";
                response.body.assign(message.begin(), message.end());
            } else {
                const bfp::FileResult file = bfp::serve_file(root_fd, request.path);
                if (file.fatal_error) {
                    std::cerr << "unable to read requested file\n";
                    return;
                }
                response.status = file.status;
                response.content_type = file.content_type;
                response.body = file.body;
            }
        }

        if (!send_response(client_fd, frame.header.request_id, response)) {
            return;
        }
    }
}

int make_listener(std::uint16_t port, std::string& error) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        error = "unable to create listening socket";
        return -1;
    }

    int reuse = 1;
    if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        error = "unable to set SO_REUSEADDR";
        ::close(listener);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::string("bind failed: ") + std::strerror(errno);
        ::close(listener);
        return -1;
    }
    if (::listen(listener, 16) != 0) {
        error = std::string("listen failed: ") + std::strerror(errno);
        ::close(listener);
        return -1;
    }
    return listener;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: bserve <web-root> <port>\n";
        return 2;
    }

    std::uint16_t port = 0;
    if (!parse_port(argv[2], port)) {
        std::cerr << "invalid port\n";
        return 2;
    }

    std::string error;
    const int root_fd = bfp::open_web_root(argv[1], error);
    if (root_fd < 0) {
        std::cerr << error << '\n';
        return 1;
    }

    const int listener = make_listener(port, error);
    if (listener < 0) {
        std::cerr << error << '\n';
        ::close(root_fd);
        return 1;
    }

    for (;;) {
        const int client_fd = ::accept(listener, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            break;
        }
        serve_connection(client_fd, root_fd);
        ::close(client_fd);
    }

    ::close(listener);
    ::close(root_fd);
    return 1;
}
