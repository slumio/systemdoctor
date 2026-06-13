#include "profiler.h"
#include "utils.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace profiler {

static std::vector<std::string> read_thread_stack(pid_t pid, pid_t tid) {
    std::vector<std::string> frames;
    std::string stack_path = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stack";
    std::ifstream file(stack_path);
    if (!file.is_open()) {
        // Try fallback to proc/pid/stack directly if tid == pid
        if (pid == tid) {
            file.open("/proc/" + std::to_string(pid) + "/stack");
        }
    }
    
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            // Parse line like: "[<0>] pipe_read+0x310/0x440"
            // or "[<ffffffff810c9be0>] pipe_read+0x310/0x440"
            size_t bracket_close = line.find(']');
            if (bracket_close != std::string::npos) {
                std::string frame = utils::trim(line.substr(bracket_close + 1));
                if (!frame.empty() && frame != "0xffffffffffffffff") {
                    frames.push_back(frame);
                }
            } else {
                frames.push_back(utils::trim(line));
            }
        }
    }
    return frames;
}

ProfileReport profile_process(pid_t pid, bool run_perf) {
    ProfileReport report;
    std::string pid_str = std::to_string(pid);
    
    if (!utils::file_exists("/proc/" + pid_str)) {
        return report;
    }
    
    // 1. Gather all thread stack traces
    std::string task_dir = "/proc/" + pid_str + "/task";
    try {
        if (utils::file_exists(task_dir)) {
            for (const auto& entry : fs::directory_iterator(task_dir)) {
                if (entry.is_directory()) {
                    std::string tid_str = entry.path().filename().string();
                    bool is_all_digits = true;
                    for (char c : tid_str) {
                        if (!std::isdigit(c)) {
                            is_all_digits = false;
                            break;
                        }
                    }
                    if (is_all_digits) {
                        pid_t tid = std::stoi(tid_str);
                        std::vector<std::string> frames = read_thread_stack(pid, tid);
                        if (!frames.empty()) {
                            StackTrace st;
                            st.tid = tid;
                            st.frames = frames;
                            report.active_stacks.push_back(st);
                        }
                    }
                }
            }
        } else {
            // Fallback for single thread
            std::vector<std::string> frames = read_thread_stack(pid, pid);
            if (!frames.empty()) {
                StackTrace st;
                st.tid = pid;
                st.frames = frames;
                report.active_stacks.push_back(st);
            }
        }
    } catch (...) {}
    
    // 2. Perform perf profiling if requested
    if (run_perf) {
        int exit_code = 0;
        utils::run_command_output("which perf", &exit_code);
        if (exit_code == 0) {
            report.perf_available = true;
            
            // Run perf record for 1.5 seconds
            std::string perf_record_cmd = "perf record -F 99 -g -p " + pid_str + " -- sleep 1.5";
            utils::run_command_output(perf_record_cmd);
            
            // Generate report
            std::string perf_report_cmd = "perf report --stdio --no-children --max-stack 12";
            std::string perf_report = utils::run_command_output(perf_report_cmd);
            
            // Clean up perf.data
            utils::delete_file("perf.data");
            
            if (!perf_report.empty()) {
                // Parse report lines to extract top symbols
                std::istringstream stream(perf_report);
                std::string line;
                int line_count = 0;
                while (std::getline(stream, line)) {
                    if (line_count < 80) {
                        report.call_graph_summary += line + "\n";
                        line_count++;
                    }
                    
                    // Simple parsing for hot symbols: e.g. "     8.50%  my_app  [.] my_function"
                    std::string trimmed = utils::trim(line);
                    if (trimmed.empty() || trimmed[0] == '#') continue;
                    
                    // Check if it starts with a percentage
                    size_t pct_pos = trimmed.find('%');
                    if (pct_pos != std::string::npos && pct_pos < 10) {
                        try {
                            double percentage = std::stod(trimmed.substr(0, pct_pos));
                            
                            // Find the symbol marker "[.]" or "[k]"
                            size_t symbol_pos = trimmed.find("[.]");
                            if (symbol_pos == std::string::npos) {
                                symbol_pos = trimmed.find("[k]");
                            }
                            
                            if (symbol_pos != std::string::npos) {
                                std::string symbol = utils::trim(trimmed.substr(symbol_pos + 3));
                                if (!symbol.empty()) {
                                    report.top_symbols.push_back({symbol, percentage});
                                }
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }
    
    return report;
}

std::string serialize_profile_to_json_string(const ProfileReport& report) {
    json j;
    j["perf_available"] = report.perf_available;
    
    json jsyms = json::array();
    for (const auto& sym : report.top_symbols) {
        json jsym;
        jsym["symbol"] = sym.first;
        jsym["overhead_percent"] = sym.second;
        jsyms.push_back(jsym);
    }
    j["top_symbols"] = jsyms;
    
    j["call_graph_summary"] = report.call_graph_summary;
    
    json jstacks = json::array();
    for (const auto& stack : report.active_stacks) {
        json jstack;
        jstack["tid"] = stack.tid;
        json jframes = json::array();
        for (const auto& frame : stack.frames) {
            jframes.push_back(frame);
        }
        jstack["frames"] = jframes;
        jstacks.push_back(jstack);
    }
    j["active_stacks"] = jstacks;
    
    return j.dump(4);
}

} // namespace profiler
