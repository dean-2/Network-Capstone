#include "protocol.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

struct Target {
    std::string host;
    std::string port;
    std::string path;
};

bool parse_target(const std::string& text, Target& target) {
    const std::size_t path_start = text.find('/');
    if (path_start == std::string::npos || path_start == 0 || path_start + 1 > text.size()) {
        return false;
    }

    const std::string host_and_port = text.substr(0, path_start);
    target.path = text.substr(path_start);
    std::string port_text;
    if (!host_and_port.empty() && host_and_port.front() == '[') {
        const std::size_t close = host_and_port.find(']');
        if (close == std::string::npos || close + 1 >= host_and_port.size() || host_and_port[close + 1] != ':') {
            return false;
        }
        target.host = host_and_port.substr(1, close - 1);
        port_text = host_and_port.substr(close + 2);
    } else {
        const std::size_t colon = host_and_port.rfind(':');
        if (colon == std::string::npos || colon == 0) {
            return false;
        }
        target.host = host_and_port.substr(0, colon);
        port_text = host_and_port.substr(colon + 1);
    }

    unsigned port_number = 0;
    const auto conversion = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port_number);
    if (port_text.empty() || conversion.ec != std::errc{} || conversion.ptr != port_text.data() + port_text.size() ||
        port_number == 0 || port_number > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    target.port = port_text;
    return !target.host.empty();
}

int connect_to(const Target& target, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo(target.host.c_str(), target.port.c_str(), &hints, &addresses);
    if (lookup != 0) {
        error = std::string("host lookup failed: ") + ::gai_strerror(lookup);
        return -1;
    }

    int socket_fd = -1;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        socket_fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (::connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        ::close(socket_fd);
        socket_fd = -1;
    }
    ::freeaddrinfo(addresses);
    if (socket_fd < 0) {
        error = "unable to connect to server";
    }
    return socket_fd;
}

void print_frame(const char* direction, const std::vector<std::uint8_t>& bytes) {
    std::cerr << direction << " frame (" << bytes.size() << " bytes):\n" << bfp::hexdump(bytes);
}

std::vector<std::uint8_t> full_frame(const bfp::Frame& frame) {
    const auto header = bfp::serialize_header(frame.header);
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    int target_arg = 1;
    if (argc > 1 && std::string(argv[1]) == "-v") {
        verbose = true;
        target_arg = 2;
    }
    if (argc != target_arg + 1) {
        std::cerr << "usage: bcurL [-v] <host>:<port>/<path>\n";
        return 2;
    }

    Target target;
    if (!parse_target(argv[target_arg], target)) {
        std::cerr << "invalid target; expected <host>:<port>/<path>\n";
        return 2;
    }

    std::string error;
    const int socket_fd = connect_to(target, error);
    if (socket_fd < 0) {
        std::cerr << error << '\n';
        return 1;
    }

    bfp::Request request{target.path};
    std::vector<std::uint8_t> request_payload;
    if (!bfp::encode_request_payload(request, request_payload, error)) {
        std::cerr << error << '\n';
        ::close(socket_fd);
        return 2;
    }

    std::vector<std::uint8_t> request_frame;
    try {
        request_frame = bfp::make_frame(
            static_cast<std::uint8_t>(bfp::FrameType::Request), 1, request_payload);
    } catch (const std::exception& exception) {
        std::cerr << "unable to build request frame: " << exception.what() << '\n';
        ::close(socket_fd);
        return 1;
    }

    if (verbose) {
        print_frame("Sent", request_frame);
    }
    if (!bfp::write_all(socket_fd, request_frame.data(), request_frame.size(), error)) {
        std::cerr << "unable to send request: " << error << '\n';
        ::close(socket_fd);
        return 1;
    }

    bfp::Frame response_frame;
    const auto observer = verbose
        ? std::function<void(const std::vector<std::uint8_t>&)>(
              [](const auto& bytes) { print_frame("Received (skipped)", bytes); })
        : std::function<void(const std::vector<std::uint8_t>&)>{};
    const bfp::FrameReadStatus read_status = bfp::read_frame(socket_fd, response_frame, error, observer);
    if (read_status != bfp::FrameReadStatus::Frame) {
        std::cerr << "unable to read response: " << (error.empty() ? "connection closed" : error) << '\n';
        ::close(socket_fd);
        return 1;
    }
    if (verbose) {
        print_frame("Received", full_frame(response_frame));
    }
    if (response_frame.header.type != static_cast<std::uint8_t>(bfp::FrameType::Response)) {
        std::cerr << "server sent a non-response frame\n";
        ::close(socket_fd);
        return 1;
    }
    if (response_frame.header.request_id != 1) {
        std::cerr << "response request ID does not match request\n";
        ::close(socket_fd);
        return 1;
    }

    bfp::Response response;
    if (!bfp::decode_response_payload(response_frame.payload, response, error)) {
        std::cerr << "malformed response: " << error << '\n';
        ::close(socket_fd);
        return 1;
    }

    if (response.status != 200 && response.status != 400 && response.status != 404) {
        std::cerr << "unsupported response status: " << response.status << '\n';
        ::close(socket_fd);
        return 1;
    }

    if (!response.body.empty()) {
        std::cout.write(reinterpret_cast<const char*>(response.body.data()),
                        static_cast<std::streamsize>(response.body.size()));
    }
    std::cout.flush();
    const bool output_ok = static_cast<bool>(std::cout);
    ::close(socket_fd);
    if (!output_ok) {
        std::cerr << "unable to write response body to stdout\n";
        return 1;
    }
    return response.status == 200 ? 0 : 1;
}
