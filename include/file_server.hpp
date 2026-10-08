#ifndef BINARY_FILE_SERVER_FILE_SERVER_HPP
#define BINARY_FILE_SERVER_FILE_SERVER_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bfp {

struct FileResult {
    std::uint16_t status = 404;
    std::string content_type;
    std::vector<std::uint8_t> body;
    bool fatal_error = false;
};

int open_web_root(const std::filesystem::path& root, std::string& error);
FileResult serve_file(int root_fd, const std::string& request_path);

} // namespace bfp

#endif
