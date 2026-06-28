#include "tui.h"
#include "../causal_engine.h"
#include "../telemetry.h"
#include "../utils.h"
#include "../ai.h"
#include "../ui/streamer.h"
#include "../config.h"
#include "../nlohmann/json.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>
#include <algorithm>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <dirent.h>

using json = nlohmann::json;

namespace ui {

struct TuiProcess {
    pid_t pid;
    pid_t ppid;
    std::string name;
    std::string state;
    double cpu_usage_pct = 0.0;
    double read_rate_kb = 0.0;
    double write_rate_kb = 0.0;
    bool is_anomalous = false;
};

struct HistoryData {
    uint64_t cpu_ticks = 0;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    std::chrono::steady_clock::time_point last_time;
};

static struct termios orig_termios;
static bool raw_mode_enabled = false;
static std::unordered_map<pid_t, HistoryData> g_history;

static void disable_raw_mode() {
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_enabled = false;
        std::cout << "\x1b[?25h\x1b[0m" << std::flush;
    }
}

static void enable_raw_mode() {
    if (!raw_mode_enabled) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        std::atexit(disable_raw_mode);

        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_cflag |= (CS8);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;

        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        raw_mode_enabled = true;
        std::cout << "\x1b[?25l" << std::flush;
    }
}

static std::string query_daemon_pids() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "";
    
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, "/tmp/syspilot.sock", sizeof(addr.sun_path) - 1);
    
    struct timeval tv = {0, 50000}; // 50ms
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return "";
    }
    
    std::string req = "{\"request\":\"process_tree\"}";
    if (write(fd, req.c_str(), req.length()) < 0) {
        close(fd);
        return "";
    }
    
    std::string res;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        res.append(buf, n);
    }
    close(fd);
    return res;
}

static std::vector<TuiProcess> get_processes() {
    std::vector<TuiProcess> list;
    std::string daemon_res = query_daemon_pids();
    std::vector<pid_t> pids;
    std::unordered_map<pid_t, pid_t> parent_map;
    
    if (!daemon_res.empty()) {
        try {
            auto j = json::parse(daemon_res);
            if (j.value("status", "") == "ok" && j.contains("processes")) {
                for (const auto& p : j["processes"]) {
                    pid_t pid = p["pid"];
                    pids.push_back(pid);
                    parent_map[pid] = p["ppid"];
                }
            }
        } catch (...) {}
    }

    if (pids.empty()) {
        // Fallback to direct directory scan
        DIR* dir = opendir("/proc");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_type == DT_DIR) {
                    std::string name = entry->d_name;
                    if (std::all_of(name.begin(), name.end(), ::isdigit)) {
                        try {
                            pids.push_back(std::stoi(name));
                        } catch (...) {}
                    }
                }
            }
            closedir(dir);
        }
    }

    auto now = std::chrono::steady_clock::now();
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    for (pid_t pid : pids) {
        ProcessTelemetry pt = telemetry::collect_process_telemetry(pid);
        if (pt.pid == 0) continue;

        TuiProcess p;
        p.pid = pid;
        p.ppid = parent_map.count(pid) ? parent_map[pid] : 0;
        p.name = pt.name;
        p.state = pt.state;

        uint64_t total_ticks = pt.utime + pt.stime;
        auto hist_it = g_history.find(pid);
        if (hist_it != g_history.end()) {
            double elapsed_sec = std::chrono::duration<double>(now - hist_it->second.last_time).count();
            if (elapsed_sec > 0.05) {
                p.cpu_usage_pct = ((double)(total_ticks - hist_it->second.cpu_ticks) / (double)clk_tck) / elapsed_sec * 100.0;
                p.read_rate_kb = (double)(pt.read_bytes - hist_it->second.read_bytes) / 1024.0 / elapsed_sec;
                p.write_rate_kb = (double)(pt.write_bytes - hist_it->second.write_bytes) / 1024.0 / elapsed_sec;
            }
        }

        g_history[pid] = { total_ticks, pt.read_bytes, pt.write_bytes, now };

        // Simple anomaly check
        if (p.state == "D" || p.cpu_usage_pct > 80.0 || p.write_rate_kb > 5000.0) {
            p.is_anomalous = true;
        }

        list.push_back(p);
    }
    return list;
}

static std::string repeat_utf8(const std::string& pattern, int times) {
    std::string result;
    result.reserve(pattern.length() * times);
    for (int i = 0; i < times; ++i) {
        result += pattern;
    }
    return result;
}

static void draw_border(int width, int height) {
    // Clear screen
    std::cout << "\x1b[2J\x1b[H";
    // Top border
    std::cout << "\x1b[90m┌" << repeat_utf8("─", width - 2) << "┐\n";
    // Sides
    for (int i = 0; i < height - 2; ++i) {
        std::cout << "│" << std::string(width - 2, ' ') << "│\n";
    }
    // Bottom border
    std::cout << "└" << repeat_utf8("─", width - 2) << "┘\x1b[0m" << std::flush;
}

static void print_at(int row, int col, const std::string& str, const std::string& style = "") {
    std::cout << "\x1b[" << row << ";" << col << "H" << style << str << "\x1b[0m" << std::flush;
}

void run_monitor() {
    enable_raw_mode();

    int sort_column = 0; // 0: CPU%, 1: Disk I/O, 2: PID
    int selected_idx = 0;
    int scroll_offset = 0;

    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int width = w.ws_col > 10 ? w.ws_col : 80;
    int height = w.ws_row > 10 ? w.ws_row : 24;

    draw_border(width, height);

    auto last_refresh = std::chrono::steady_clock::now();
    std::vector<TuiProcess> processes = get_processes();

    while (true) {
        // Handle terminal size change
        struct winsize current_w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &current_w);
        if (current_w.ws_col != width || current_w.ws_row != height) {
            width = current_w.ws_col > 10 ? current_w.ws_col : 80;
            height = current_w.ws_row > 10 ? current_w.ws_row : 24;
            draw_border(width, height);
        }

        // Periodically refresh processes (every 1 second)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refresh).count() >= 1000) {
            processes = get_processes();
            last_refresh = now;

            // Re-sort
            if (sort_column == 0) {
                std::sort(processes.begin(), processes.end(), [](const TuiProcess& a, const TuiProcess& b) {
                    return a.cpu_usage_pct > b.cpu_usage_pct;
                });
            } else if (sort_column == 1) {
                std::sort(processes.begin(), processes.end(), [](const TuiProcess& a, const TuiProcess& b) {
                    return (a.read_rate_kb + a.write_rate_kb) > (b.read_rate_kb + b.write_rate_kb);
                });
            } else {
                std::sort(processes.begin(), processes.end(), [](const TuiProcess& a, const TuiProcess& b) {
                    return a.pid < b.pid;
                });
            }
        }

        // Draw system header
        SystemTelemetry st = telemetry::collect_system_telemetry();
        std::stringstream header_ss;
        header_ss << "🤖 SysPilot Monitor | Load: " << st.load_avg
                  << " | Mem: " << ((st.mem_total_kb - st.mem_available_kb) / 1024) 
                  << "MB / " << (st.mem_total_kb / 1024) << "MB";
        print_at(2, 3, header_ss.str(), "\x1b[1;36m");
        
        std::string sort_label = sort_column == 0 ? "CPU%" : (sort_column == 1 ? "I/O Rate" : "PID");
        print_at(2, width - 25, "[Sorting by: " + sort_label + "]", "\x1b[1;33m");

        // Draw table header
        int table_row = 4;
        print_at(table_row, 3, "  PID    PPID   STATE   CPU%     DISK READ     DISK WRITE    PROCESS NAME", "\x1b[1;90m");
        print_at(table_row + 1, 3, std::string(width - 6, '-'), "\x1b[90m");

        // Keep selection within bounds
        if (processes.empty()) {
            selected_idx = 0;
        } else if (selected_idx >= (int)processes.size()) {
            selected_idx = processes.size() - 1;
        }

        // Draw process list
        int list_height = height - 8; // Available space for process list
        if (selected_idx < scroll_offset) {
            scroll_offset = selected_idx;
        } else if (selected_idx >= scroll_offset + list_height) {
            scroll_offset = selected_idx - list_height + 1;
        }

        for (int i = 0; i < list_height; ++i) {
            int current_row = table_row + 2 + i;
            int proc_idx = scroll_offset + i;
            if (proc_idx >= (int)processes.size()) {
                // Clear unused lines
                print_at(current_row, 3, std::string(width - 6, ' '));
                continue;
            }

            const auto& p = processes[proc_idx];
            std::stringstream ss;
            ss << std::left << std::setw(8) << p.pid 
               << std::setw(8) << p.ppid 
               << std::setw(8) << p.state;

            // Highlight anomalous state or high CPU
            std::stringstream cpu_ss;
            cpu_ss << std::fixed << std::setprecision(1) << p.cpu_usage_pct << "%";
            ss << std::setw(9) << cpu_ss.str();

            std::stringstream r_ss, w_ss;
            r_ss << std::fixed << std::setprecision(1) << p.read_rate_kb << " KB/s";
            w_ss << std::fixed << std::setprecision(1) << p.write_rate_kb << " KB/s";
            ss << std::setw(14) << r_ss.str() 
               << std::setw(14) << w_ss.str()
               << p.name;

            std::string line = ss.str();
            if (line.length() > (size_t)width - 6) {
                line = line.substr(0, width - 6);
            } else {
                line += std::string(width - 6 - line.length(), ' ');
            }

            std::string style = "";
            if (proc_idx == selected_idx) {
                style = "\x1b[7m"; // Invert colors
            } else if (p.is_anomalous) {
                style = "\x1b[1;31m"; // Red for anomalies
            } else if (p.state == "R") {
                style = "\x1b[32m"; // Green for running
            }

            print_at(current_row, 3, line, style);
        }

        // Draw footer / help
        int footer_row = height - 2;
        print_at(footer_row, 3, "[Tab] Sort  [e] AI Explain  [s] SIGSTOP  [r] SIGCONT  [k] SIGKILL  [q] Quit", "\x1b[1;90m");

        // Read input (non-blocking)
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 100000}; // 100ms
        int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (ret > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 'q') {
                    break;
                } else if (c == 9) { // Tab key
                    sort_column = (sort_column + 1) % 3;
                    last_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(2); // force reload
                } else if (c == 'j') { // arrow down or j
                    selected_idx++;
                } else if (c == 'k') { // arrow up or k
                    selected_idx--;
                } else if (c == '\x1b') { // Escape sequence (arrows)
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') selected_idx--;      // Up
                            else if (seq[1] == 'B') selected_idx++; // Down
                        }
                    }
                } else if (c == 's') { // SIGSTOP
                    if (!processes.empty()) {
                        pid_t target_pid = processes[selected_idx].pid;
                        kill(target_pid, SIGSTOP);
                    }
                } else if (c == 'r') { // SIGCONT
                    if (!processes.empty()) {
                        pid_t target_pid = processes[selected_idx].pid;
                        kill(target_pid, SIGCONT);
                    }
                } else if (c == 'k') { // SIGKILL
                    if (!processes.empty()) {
                        pid_t target_pid = processes[selected_idx].pid;
                        kill(target_pid, SIGKILL);
                    }
                } else if (c == 'e' || c == '\n') { // AI Explain
                    if (!processes.empty()) {
                        pid_t target_pid = processes[selected_idx].pid;
                        std::string target_name = processes[selected_idx].name;

                        // Suspend TUI
                        disable_raw_mode();
                        std::cout << "\x1b[2J\x1b[H" << std::flush;
                        std::cout << "🧠 \x1b[1;36mQuerying SysPilot AI diagnostic explanation for PID " 
                                  << target_pid << " (" << target_name << ")... \x1b[0m\n" << std::endl;

                        Config conf = config::load();
                        json ctx;
                        ctx["current_dir"] = utils::trim(utils::run_command_output("pwd"));
                        ctx["analysis_type"] = "causal_inference_diagnostics";
                        ctx["target_process"] = target_name;
                        ctx["target_pid"] = target_pid;

                        CausalGraph graph;
                        graph.build_graph(2, false, target_pid);

                        std::string target_node_id = "pid:" + std::to_string(target_pid);
                        std::vector<std::string> path = graph.trace_root_cause(target_node_id);
                        std::string json_chain = graph.serialize_chain_to_json(path);
                        ctx["causal_chain"] = json::parse(json_chain);

                        std::string prompt = "You are a senior system reliability engineer performing root-cause analysis.\n"
                                             "Here is the structured JSON representation of the traversed causal path:\n" +
                                             ctx.dump(4) + "\n\n"
                                             "Please explain the diagnostic findings, step-by-step root cause chain, and action recommendations.";
                        
                        MdStreamer streamer;
                        ai::query_ai_stream(conf, prompt, streamer);

                        std::cout << "\n\x1b[90m" << std::string(60, '-') << "\nPress any key to return to monitor...\x1b[0m" << std::flush;
                        
                        // Wait for keypress
                        enable_raw_mode();
                        char dummy;
                        while (read(STDIN_FILENO, &dummy, 1) <= 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                        
                        // Redraw TUI border
                        draw_border(width, height);
                    }
                }
            }
        }
    }

    disable_raw_mode();
}

} // namespace ui
