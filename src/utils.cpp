#include "utils.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace utils {

std::string trim(const std::string &str) {
  auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char ch) {
    return std::isspace(ch);
  });
  auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) {
               return std::isspace(ch);
             }).base();
  return (start < end) ? std::string(start, end) : "";
}

std::vector<std::string> split(const std::string &str, char delimiter) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(str);
  while (std::getline(tokenStream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

std::vector<std::string> split(const std::string &str,
                               const std::string &delimiter) {
  std::vector<std::string> tokens;
  size_t prev = 0, pos = 0;
  do {
    pos = str.find(delimiter, prev);
    if (pos == std::string::npos)
      pos = str.length();
    std::string token = str.substr(prev, pos - prev);
    tokens.push_back(token);
    prev = pos + delimiter.length();
  } while (pos < str.length() && prev < str.length());
  return tokens;
}

std::string replace(std::string str, const std::string &from,
                    const std::string &to) {
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
  return str;
}

bool starts_with(std::string_view str, std::string_view prefix) {
  return str.size() >= prefix.size() &&
         str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view str, std::string_view suffix) {
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string to_lower(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return str;
}

std::string get_home_directory() {
  const char *home = std::getenv("HOME");
  if (home) {
    return std::string(home);
  }
  return "/";
}

std::string get_syspilot_directory() {
  return get_home_directory() + "/.syspilot";
}

bool create_directory_recursive(const std::string &path) {
  try {
    return fs::create_directories(path);
  } catch (...) {
    return false;
  }
}

bool file_exists(const std::string &path) { return fs::exists(path); }

bool is_directory(const std::string &path) { return fs::is_directory(path); }

uint64_t get_file_size(const std::string &path) {
  try {
    if (fs::exists(path) && fs::is_regular_file(path)) {
      return fs::file_size(path);
    }
  } catch (...) {
  }
  return 0;
}

uint64_t get_last_modified_time(const std::string &path) {
  try {
    if (fs::exists(path)) {
      auto ftime = fs::last_write_time(path);
      auto sct = std::chrono::time_point_cast<std::chrono::seconds>(
          ftime - fs::file_time_type::clock::now() +
          std::chrono::system_clock::now());
      return sct.time_since_epoch().count();
    }
  } catch (...) {
  }
  return 0;
}

std::vector<std::string> list_directory(const std::string &path,
                                        bool recursive) {
  std::vector<std::string> files;
  if (!fs::exists(path) || !fs::is_directory(path)) {
    return files;
  }
  const std::vector<std::string> valid_exts = {
      "rs", "py",  "js",   "ts",  "c",    "cpp",  "h",   "hpp", "java", "go",
      "md", "txt", "html", "css", "json", "yaml", "yml", "sh",  "toml"};
  try {
    if (recursive) {
      // Attempt to use 'rg' (ripgrep) for high-performance directory listing
      int exit_code = -1;
      std::string escaped_path = path;
      size_t pos = 0;
      while ((pos = escaped_path.find('\'', pos)) != std::string::npos) {
        escaped_path.replace(pos, 1, "'\\''");
        pos += 4;
      }
      std::string cmd = "rg --files '" + escaped_path + "'";
      std::string output = run_command_output(cmd, &exit_code);
      if (exit_code == 0 && !output.empty()) {
        std::vector<std::string> lines = split(output, '\n');
        for (auto &line : lines) {
          line = trim(line);
          if (line.empty())
            continue;
          size_t dot_pos = line.find_last_of('.');
          if (dot_pos != std::string::npos &&
              dot_pos > line.find_last_of('/')) {
            std::string ext = line.substr(dot_pos + 1);
            if (std::find(valid_exts.begin(), valid_exts.end(), ext) !=
                valid_exts.end()) {
              files.push_back(line);
            }
          }
        }
        return files;
      }

      // Fallback to std::filesystem::recursive_directory_iterator
      for (const auto &entry : fs::recursive_directory_iterator(path)) {
        std::string name = entry.path().filename().string();
        if (name == ".git" || name == "target" || name == "node_modules" ||
            name == "dist" || name == "build" || name == ".syspilot") {
          continue;
        }
        if (entry.is_regular_file()) {
          std::string ext = entry.path().extension().string();
          if (!ext.empty() && ext[0] == '.') {
            ext = ext.substr(1);
          }
          if (std::find(valid_exts.begin(), valid_exts.end(), ext) !=
              valid_exts.end()) {
            files.push_back(entry.path().string());
          }
        }
      }
    } else {
      for (const auto &entry : fs::directory_iterator(path)) {
        if (entry.is_regular_file()) {
          files.push_back(entry.path().string());
        }
      }
    }
  } catch (...) {
  }
  return files;
}

std::string run_command_output(const std::string &cmd, int *exit_code) {
  std::array<char, 512> buffer;
  std::string result;
  // Redirect stderr to stdout so we capture it as well
  std::string cmd_with_stderr = cmd + " 2>&1";
  std::unique_ptr<FILE, int (*)(FILE *)> pipe(
      popen(cmd_with_stderr.c_str(), "r"), pclose);
  if (!pipe) {
    if (exit_code)
      *exit_code = -1;
    return "Failed to start process";
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  int status = pclose(pipe.release());
  if (exit_code) {
    if (WIFEXITED(status)) {
      *exit_code = WEXITSTATUS(status);
    } else {
      *exit_code = status;
    }
  }
  return result;
}

bool write_file_content(const std::string &path, const std::string &content) {
  std::ofstream file(path);
  if (!file.is_open())
    return false;
  file << content;
  return true;
}

std::string read_file_content(const std::string &path, bool *success) {
  std::ifstream file(path);
  if (!file.is_open()) {
    if (success)
      *success = false;
    return "";
  }
  if (success)
    *success = true;
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool delete_file(const std::string &path) {
  try {
    return fs::remove(path);
  } catch (...) {
    return false;
  }
}

std::string run_command_secure(const std::vector<std::string> &args,
                               const std::string &input_data, int *exit_code) {
  if (args.empty()) {
    if (exit_code)
      *exit_code = -1;
    return "";
  }

  int stdin_pipe[2];
  int stdout_pipe[2];

  if (pipe(stdin_pipe) < 0) {
    if (exit_code)
      *exit_code = -1;
    return "Failed to create stdin pipe";
  }
  if (pipe(stdout_pipe) < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    if (exit_code)
      *exit_code = -1;
    return "Failed to create stdout pipe";
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    if (exit_code)
      *exit_code = -1;
    return "Failed to fork process";
  }

  if (pid == 0) { // Child
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    // Redirect stderr to stdout so we capture it
    dup2(stdout_pipe[1], STDERR_FILENO);

    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    // Prepare argv
    std::vector<char *> argv;
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    std::exit(127);
  }

  // Parent
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);

  // Write input data to child's stdin
  if (!input_data.empty()) {
    size_t written = 0;
    while (written < input_data.size()) {
      ssize_t res = write(stdin_pipe[1], input_data.data() + written,
                          input_data.size() - written);
      if (res < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      written += res;
    }
  }
  close(stdin_pipe[1]); // Send EOF to child stdin

  // Read stdout from child
  std::string result;
  char buffer[4096];
  while (true) {
    ssize_t res = read(stdout_pipe[0], buffer, sizeof(buffer));
    if (res < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (res == 0)
      break; // EOF
    result.append(buffer, res);
  }
  close(stdout_pipe[0]);

  int status;
  waitpid(pid, &status, 0);
  if (exit_code) {
    if (WIFEXITED(status)) {
      *exit_code = WEXITSTATUS(status);
    } else {
      *exit_code = status;
    }
  }

  return result;
}

bool run_command_secure_stream(
    const std::vector<std::string> &args, const std::string &input_data,
    std::function<void(const std::string &)> callback, int *exit_code) {
  if (args.empty()) {
    if (exit_code)
      *exit_code = -1;
    return false;
  }

  int stdin_pipe[2];
  int stdout_pipe[2];

  if (pipe(stdin_pipe) < 0) {
    if (exit_code)
      *exit_code = -1;
    return false;
  }
  if (pipe(stdout_pipe) < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    if (exit_code)
      *exit_code = -1;
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    if (exit_code)
      *exit_code = -1;
    return false;
  }

  if (pid == 0) { // Child
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    // Redirect stderr to stdout
    dup2(stdout_pipe[1], STDERR_FILENO);

    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    std::vector<char *> argv;
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    std::exit(127);
  }

  // Parent
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);

  // Write input data to child's stdin
  if (!input_data.empty()) {
    size_t written = 0;
    while (written < input_data.size()) {
      ssize_t res = write(stdin_pipe[1], input_data.data() + written,
                          input_data.size() - written);
      if (res < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      written += res;
    }
  }
  close(stdin_pipe[1]);

  // Read stdout from child and stream it
  char buffer[4096];
  while (true) {
    ssize_t res = read(stdout_pipe[0], buffer, sizeof(buffer));
    if (res < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (res == 0)
      break; // EOF
    callback(std::string(buffer, res));
  }
  close(stdout_pipe[0]);

  int status;
  waitpid(pid, &status, 0);
  if (exit_code) {
    if (WIFEXITED(status)) {
      *exit_code = WEXITSTATUS(status);
    } else {
      *exit_code = status;
    }
  }

  return (exit_code ? (*exit_code == 0) : true);
}

} // namespace utils
