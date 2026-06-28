#ifndef UTILS_H
#define UTILS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <string_view>

namespace utils {

// String manipulation
std::string trim(const std::string &str);
std::vector<std::string> split(const std::string &str, char delimiter);
std::vector<std::string> split(const std::string &str,
                               const std::string &delimiter);
std::string replace(std::string str, const std::string &from,
                    const std::string &to);
bool starts_with(std::string_view str, std::string_view prefix);
bool ends_with(std::string_view str, std::string_view suffix);
std::string to_lower(std::string str);

// Filesystem/Paths
std::string get_home_directory();
std::string get_syspilot_directory();
bool create_directory_recursive(const std::string &path);
bool file_exists(const std::string &path);
bool is_directory(const std::string &path);
uint64_t get_file_size(const std::string &path);
uint64_t get_last_modified_time(const std::string &path);
std::vector<std::string> list_directory(const std::string &path,
                                        bool recursive = true);

// Subprocess helpers
std::string run_command_output(const std::string &cmd,
                               int *exit_code = nullptr);
bool write_file_content(const std::string &path, const std::string &content);
std::string read_file_content(const std::string &path, bool *success = nullptr);
bool delete_file(const std::string &path);

std::string run_command_secure(const std::vector<std::string> &args,
                               const std::string &input_data = "",
                               int *exit_code = nullptr);

bool run_command_secure_stream(
    const std::vector<std::string> &args, const std::string &input_data,
    std::function<void(const std::string &)> callback,
    int *exit_code = nullptr);

} // namespace utils

#endif // UTILS_H
