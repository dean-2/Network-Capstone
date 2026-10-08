#include "file_server.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace bfp {
namespace {

FileResult bad_request() {
    FileResult result;
    result.status = 400;
    const std::string message = "Bad Request\n";
    result.body.assign(message.begin(), message.end());
    return result;
}

FileResult not_found() {
    FileResult result;
    result.status = 404;
    const std::string message = "Not Found\n";
    result.body.assign(message.begin(), message.end());
    return result;
}

std::string content_type_for(const std::string& filename) {
    std::string extension = std::filesystem::path(filename).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });

    if (extension == ".html" || extension == ".htm") return "text/html";
    if (extension == ".txt") return "text/plain";
    if (extension == ".css") return "text/css";
    if (extension == ".js") return "application/javascript";
    if (extension == ".json") return "application/json";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".pdf") return "application/pdf";
    return "application/octet-stream";
}

bool build_components(const std::string& request_path, std::vector<std::string>& components) {
    if (request_path.empty() || request_path.front() != '/' ||
        request_path.find('\0') != std::string::npos) {
        return false;
    }

    std::size_t start = 1;
    while (start <= request_path.size()) {
        const std::size_t slash = request_path.find('/', start);
        const std::size_t end = slash == std::string::npos ? request_path.size() : slash;
        const std::string_view part(request_path.data() + start, end - start);
        if (part == "..") {
            if (components.empty()) {
                return false;
            }
            components.pop_back();
        } else if (!part.empty() && part != ".") {
            components.emplace_back(part);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

} // namespace

int open_web_root(const std::filesystem::path& root, std::string& error) {
    std::error_code filesystem_error;
    const std::filesystem::path canonical_root = std::filesystem::canonical(root, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(canonical_root, filesystem_error) || filesystem_error) {
        error = "web root must be an existing directory";
        return -1;
    }

    const int fd = ::open(canonical_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        error = "unable to open web root";
        return -1;
    }
    return fd;
}

FileResult serve_file(int root_fd, const std::string& request_path) {
    std::vector<std::string> components;
    if (!build_components(request_path, components)) {
        return bad_request();
    }
    if (components.empty()) {
        return not_found();
    }

    int current_fd = ::dup(root_fd);
    if (current_fd < 0) {
        FileResult result;
        result.fatal_error = true;
        return result;
    }

    int file_fd = -1;
    for (std::size_t i = 0; i < components.size(); ++i) {
        struct stat component_info {};
        if (::fstatat(current_fd, components[i].c_str(), &component_info, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(component_info.st_mode)) {
            ::close(current_fd);
            return bad_request();
        }

        const bool is_last = i + 1 == components.size();
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (!is_last) {
            flags |= O_DIRECTORY;
        }
        file_fd = ::openat(current_fd, components[i].c_str(), flags);
        const int open_error = errno;
        ::close(current_fd);
        if (file_fd < 0) {
            if (open_error == ELOOP) {
                return bad_request();
            }
            return not_found();
        }
        if (!is_last) {
            current_fd = file_fd;
            file_fd = -1;
        }
    }

    struct stat info {};
    if (::fstat(file_fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(file_fd);
        return not_found();
    }

    FileResult result;
    result.status = 200;
    result.content_type = content_type_for(components.back());
    std::uint8_t buffer[16384];
    for (;;) {
        const ssize_t amount = ::read(file_fd, buffer, sizeof(buffer));
        if (amount == 0) {
            break;
        }
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.body.clear();
            result.fatal_error = true;
            break;
        }
        result.body.insert(result.body.end(), buffer, buffer + amount);
    }
    ::close(file_fd);
    return result;
}

} // namespace bfp
