#include "telemetry.h"
#include "utils.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

static uint64_t safe_stoull(const std::string& str, uint64_t default_val = 0) {
    if (str.empty()) return default_val;
    try {
        return std::stoull(str);
    } catch (...) {
        return default_val;
    }
}

static int safe_stoi(const std::string& str, int default_val = 0) {
    if (str.empty()) return default_val;
    try {
        return std::stoi(str);
    } catch (...) {
        return default_val;
    }
}

static std::vector<std::string> split_whitespace(const std::string& str) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream stream(str);
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

namespace telemetry {

pid_t find_pid_by_name(const std::string& name) {
    if (name.empty()) return 0;
    
    // Check if the input is already a PID
    bool is_digits = true;
    for (char c : name) {
        if (!std::isdigit(c)) {
            is_digits = false;
            break;
        }
    }
    if (is_digits) {
        pid_t pid = safe_stoi(name);
        if (utils::file_exists("/proc/" + std::to_string(pid))) {
            return pid;
        }
    }

    try {
        for (const auto& entry : fs::directory_iterator("/proc")) {
            if (entry.is_directory()) {
                std::string pid_str = entry.path().filename().string();
                bool only_digits = true;
                for (char c : pid_str) {
                    if (!std::isdigit(c)) {
                        only_digits = false;
                        break;
                    }
                }
                if (only_digits) {
                    std::string comm_path = entry.path().string() + "/comm";
                    std::string comm = utils::trim(utils::read_file_content(comm_path));
                    if (comm == name) {
                        return safe_stoi(pid_str);
                    }
                    
                    // Fallback to checking cmdline
                    std::string cmd_path = entry.path().string() + "/cmdline";
                    std::string cmd = utils::read_file_content(cmd_path);
                    if (!cmd.empty()) {
                        // cmdline is null-terminated, read the first argument
                        size_t null_pos = cmd.find('\0');
                        std::string binary = cmd.substr(0, null_pos);
                        size_t slash_pos = binary.find_last_of('/');
                        if (slash_pos != std::string::npos) {
                            binary = binary.substr(slash_pos + 1);
                        }
                        if (binary == name) {
                            return safe_stoi(pid_str);
                        }
                    }
                }
            }
        }
    } catch (...) {}
    
    return 0;
}

ProcessTelemetry collect_process_telemetry(pid_t pid) {
    ProcessTelemetry pt;
    pt.pid = pid;
    
    std::string pid_dir = "/proc/" + std::to_string(pid);
    if (!utils::file_exists(pid_dir)) {
        return pt;
    }
    
    // 1. Read /proc/[pid]/comm
    pt.name = utils::trim(utils::read_file_content(pid_dir + "/comm"));
    
    // 2. Read /proc/[pid]/stat
    std::string stat_content = utils::read_file_content(pid_dir + "/stat");
    if (!stat_content.empty()) {
        size_t close_paren = stat_content.rfind(')');
        if (close_paren != std::string::npos && close_paren + 2 < stat_content.length()) {
            std::string rest = stat_content.substr(close_paren + 2);
            std::vector<std::string> fields = split_whitespace(rest);
            
            // Offset fields since we skipped the PID and COMM name (which are fields 1 and 2)
            // field index in rest is (original index - 3)
            
            if (fields.size() > 0) pt.state = fields[0];
            if (fields.size() > 1) pt.ppid = safe_stoi(fields[1]);
            if (fields.size() > 7) pt.minflt = safe_stoull(fields[7]);
            if (fields.size() > 9) pt.majflt = safe_stoull(fields[9]);
            if (fields.size() > 11) pt.utime = safe_stoull(fields[11]);
            if (fields.size() > 12) pt.stime = safe_stoull(fields[12]);
            if (fields.size() > 17) pt.thread_count = safe_stoi(fields[17]);
            if (fields.size() > 20) pt.vsize_bytes = safe_stoull(fields[20]);
            if (fields.size() > 21) {
                // RSS is in pages, multiply by page size (typically 4096)
                long page_size = sysconf(_SC_PAGESIZE);
                if (page_size <= 0) page_size = 4096;
                pt.rss_bytes = safe_stoull(fields[21]) * page_size;
            }
        }
    }
    
    // 3. Read /proc/[pid]/status (for context switches)
    std::ifstream status_file(pid_dir + "/status");
    if (status_file.is_open()) {
        std::string line;
        while (std::getline(status_file, line)) {
            if (utils::starts_with(line, "voluntary_ctxt_switches:")) {
                std::vector<std::string> parts = utils::split(line, ':');
                if (parts.size() > 1) pt.voluntary_ctxt_switches = safe_stoull(utils::trim(parts[1]));
            } else if (utils::starts_with(line, "nonvoluntary_ctxt_switches:")) {
                std::vector<std::string> parts = utils::split(line, ':');
                if (parts.size() > 1) pt.involuntary_ctxt_switches = safe_stoull(utils::trim(parts[1]));
            }
        }
    }
    
    // 4. Read /proc/[pid]/schedstat
    std::string schedstat_content = utils::read_file_content(pid_dir + "/schedstat");
    if (!schedstat_content.empty()) {
        std::vector<std::string> fields = split_whitespace(schedstat_content);
        if (fields.size() >= 3) {
            pt.sched_run_ticks = safe_stoull(fields[0]);
            pt.sched_wait_ticks = safe_stoull(fields[1]);
            pt.sched_timeslices = safe_stoull(fields[2]);
        }
    }
    
    // 5. Read /proc/[pid]/io
    std::ifstream io_file(pid_dir + "/io");
    if (io_file.is_open()) {
        std::string line;
        while (std::getline(io_file, line)) {
            std::vector<std::string> parts = utils::split(line, ':');
            if (parts.size() > 1) {
                std::string key = utils::trim(parts[0]);
                uint64_t val = safe_stoull(utils::trim(parts[1]));
                if (key == "rchar") pt.read_char = val;
                else if (key == "wchar") pt.write_char = val;
                else if (key == "syscr") pt.syscr = val;
                else if (key == "syscw") pt.syscw = val;
                else if (key == "read_bytes") pt.read_bytes = val;
                else if (key == "write_bytes") pt.write_bytes = val;
            }
        }
    }
    
    // 6. Read environment variables from /proc/[pid]/environ
    std::string env_content = utils::read_file_content(pid_dir + "/environ");
    if (!env_content.empty()) {
        size_t prev = 0;
        for (size_t i = 0; i < env_content.length(); ++i) {
            if (env_content[i] == '\0') {
                std::string var = env_content.substr(prev, i - prev);
                if (!var.empty()) {
                    pt.env.push_back(var);
                }
                prev = i + 1;
            }
        }
    }
    
    // 7. Find child PIDs by iterating /proc and looking for PPID
    try {
        for (const auto& entry : fs::directory_iterator("/proc")) {
            if (entry.is_directory()) {
                std::string sub_pid_str = entry.path().filename().string();
                bool only_digits = true;
                for (char c : sub_pid_str) {
                    if (!std::isdigit(c)) {
                        only_digits = false;
                        break;
                    }
                }
                if (only_digits) {
                    std::ifstream sub_stat_file(entry.path().string() + "/stat");
                    if (sub_stat_file.is_open()) {
                        std::string sub_stat;
                        std::getline(sub_stat_file, sub_stat);
                        size_t close_p = sub_stat.rfind(')');
                        if (close_p != std::string::npos && close_p + 2 < sub_stat.length()) {
                            std::string sub_rest = sub_stat.substr(close_p + 2);
                            std::vector<std::string> sub_fields = split_whitespace(sub_rest);
                            if (sub_fields.size() > 1) {
                                pid_t sub_ppid = safe_stoi(sub_fields[1]);
                                if (sub_ppid == pid) {
                                    pt.child_pids.push_back(safe_stoi(sub_pid_str));
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {}
    
    return pt;
}

SystemTelemetry collect_system_telemetry() {
    SystemTelemetry st;
    
    // 1. Read /proc/loadavg
    st.load_avg = utils::trim(utils::read_file_content("/proc/loadavg"));
    
    // 2. Read /proc/meminfo
    std::ifstream mem_file("/proc/meminfo");
    if (mem_file.is_open()) {
        std::string line;
        while (std::getline(mem_file, line)) {
            std::vector<std::string> parts = utils::split(line, ':');
            if (parts.size() > 1) {
                std::string key = utils::trim(parts[0]);
                std::string val_str = utils::trim(parts[1]);
                // Strip " kB"
                size_t kb_pos = val_str.find(" kB");
                if (kb_pos != std::string::npos) {
                    val_str = val_str.substr(0, kb_pos);
                }
                uint64_t val = safe_stoull(val_str);
                
                if (key == "MemTotal") st.mem_total_kb = val;
                else if (key == "MemFree") st.mem_free_kb = val;
                else if (key == "MemAvailable") st.mem_available_kb = val;
                else if (key == "Cached") st.mem_cached_kb = val;
                else if (key == "Buffers") st.mem_buffers_kb = val;
            }
        }
    }
    
    // 3. Read /proc/stat
    std::ifstream stat_file("/proc/stat");
    if (stat_file.is_open()) {
        std::string line;
        if (std::getline(stat_file, line)) {
            std::vector<std::string> fields = split_whitespace(line);
            if (fields.size() >= 8 && fields[0] == "cpu") {
                // fields[1] user, fields[2] nice, fields[3] system, fields[4] idle,
                // fields[5] iowait, fields[6] irq, fields[7] softirq
                st.cpu_user = safe_stoull(fields[1]);
                st.cpu_nice = safe_stoull(fields[2]);
                st.cpu_system = safe_stoull(fields[3]);
                st.cpu_idle = safe_stoull(fields[4]);
                st.cpu_iowait = safe_stoull(fields[5]);
                st.cpu_irq = safe_stoull(fields[6]);
                st.cpu_softirq = safe_stoull(fields[7]);
            }
        }
    }
    
    // 4. Read disk usage
    st.disk_usage_summary = utils::trim(utils::run_command_output("df -h / 2>/dev/null"));
    
    return st;
}

std::string serialize_telemetry_to_json_string(const ProcessTelemetry& pt, const SystemTelemetry& st) {
    json j;
    
    // Process telemetry
    json jp;
    jp["pid"] = pt.pid;
    jp["name"] = pt.name;
    jp["ppid"] = pt.ppid;
    jp["state"] = pt.state;
    jp["thread_count"] = pt.thread_count;
    jp["cpu_user_ticks"] = pt.utime;
    jp["cpu_system_ticks"] = pt.stime;
    jp["rss_bytes"] = pt.rss_bytes;
    jp["vsize_bytes"] = pt.vsize_bytes;
    jp["minor_page_faults"] = pt.minflt;
    jp["major_page_faults"] = pt.majflt;
    jp["voluntary_context_switches"] = pt.voluntary_ctxt_switches;
    jp["involuntary_context_switches"] = pt.involuntary_ctxt_switches;
    jp["io_read_bytes"] = pt.read_bytes;
    jp["io_write_bytes"] = pt.write_bytes;
    jp["io_read_chars"] = pt.read_char;
    jp["io_write_chars"] = pt.write_char;
    jp["io_syscr"] = pt.syscr;
    jp["io_syscw"] = pt.syscw;
    jp["sched_run_ticks"] = pt.sched_run_ticks;
    jp["sched_wait_ticks"] = pt.sched_wait_ticks;
    jp["sched_timeslices"] = pt.sched_timeslices;
    jp["child_pids"] = pt.child_pids;
    
    // Limit environment variables to avoid blowing LLM context
    json jenv = json::array();
    for (size_t i = 0; i < std::min(pt.env.size(), (size_t)30); ++i) {
        jenv.push_back(pt.env[i]);
    }
    if (pt.env.size() > 30) {
        jenv.push_back("... (truncated " + std::to_string(pt.env.size() - 30) + " environment variables)");
    }
    jp["environment"] = jenv;
    j["process"] = jp;
    
    // System telemetry
    json js;
    js["load_average"] = st.load_avg;
    js["memory_total_kb"] = st.mem_total_kb;
    js["memory_free_kb"] = st.mem_free_kb;
    js["memory_available_kb"] = st.mem_available_kb;
    js["memory_cached_kb"] = st.mem_cached_kb;
    js["memory_buffers_kb"] = st.mem_buffers_kb;
    
    json jcpu;
    jcpu["user"] = st.cpu_user;
    jcpu["nice"] = st.cpu_nice;
    jcpu["system"] = st.cpu_system;
    jcpu["idle"] = st.cpu_idle;
    jcpu["iowait"] = st.cpu_iowait;
    jcpu["irq"] = st.cpu_irq;
    jcpu["softirq"] = st.cpu_softirq;
    js["cpu_ticks"] = jcpu;
    
    js["disk_usage"] = st.disk_usage_summary;
    j["system"] = js;
    
    return j.dump(4);
}

std::string collect_ebpf_telemetry(pid_t pid, int duration_seconds) {
    bool has_privileges = false;
    std::string bpftrace_cmd = "bpftrace";

    if (getuid() == 0) {
        has_privileges = true;
    } else if (std::system("sudo -n true 2>/dev/null") == 0) {
        has_privileges = true;
        bpftrace_cmd = "sudo bpftrace";
    }

    if (!has_privileges) {
        std::cerr << "\n⚠️  eBPF tracing requires root privileges. Please run SysPilot with sudo:" << std::endl;
        std::cerr << "   sudo ./syspilot explain --pid " << pid << " --ebpf" << std::endl;
        return "Error: Insufficient privileges to run eBPF bpftrace.";
    }

    std::cout << "🚀 Starting real-time eBPF event tracing on PID " << pid << " (" << duration_seconds << "s)..." << std::endl;

    std::string filter = "pid == " + std::to_string(pid) + " || ppid == " + std::to_string(pid);
    ProcessTelemetry pt = collect_process_telemetry(pid);
    for (pid_t child_pid : pt.child_pids) {
        filter += " || pid == " + std::to_string(child_pid);
    }

    // Build the inline bpftrace script
    std::string script = 
        "tracepoint:syscalls:sys_enter_open,tracepoint:syscalls:sys_enter_openat /" + filter + "/ { "
        "  printf(\"OPEN | %s | %s\\n\", comm, str(args->filename)); "
        "} "
        "tracepoint:syscalls:sys_enter_connect /" + filter + "/ { "
        "  printf(\"CONNECT | %s | family: %d\\n\", comm, args->uservaddr->sa_family); "
        "} "
        "tracepoint:syscalls:sys_enter_execve /" + filter + "/ { "
        "  printf(\"EXEC | %s | %s\\n\", comm, str(args->filename)); "
        "}";

    std::string cmd = "timeout " + std::to_string(duration_seconds) + " " + bpftrace_cmd + " -e '" + script + "' 2>/dev/null";
    std::string output = utils::run_command_output(cmd);

    std::vector<std::string> lines = utils::split(output, '\n');
    std::vector<std::string> opens;
    std::vector<std::string> connects;
    std::vector<std::string> execs;

    for (const auto& line : lines) {
        if (utils::starts_with(line, "OPEN | ")) {
            opens.push_back(line.substr(7));
        } else if (utils::starts_with(line, "CONNECT | ")) {
            connects.push_back(line.substr(10));
        } else if (utils::starts_with(line, "EXEC | ")) {
            execs.push_back(line.substr(7));
        }
    }

    std::string summary = "=== eBPF Kernel Event Log (Duration: " + std::to_string(duration_seconds) + "s) ===\n";
    if (!opens.empty()) {
        summary += "📂 File Operations (Opened):\n";
        for (const auto& file : opens) {
            summary += "  - " + file + "\n";
        }
    }
    if (!connects.empty()) {
        summary += "🌐 Network Operations:\n";
        for (const auto& conn : connects) {
            summary += "  - " + conn + "\n";
        }
    }
    if (!execs.empty()) {
        summary += "🚀 Process Executions:\n";
        for (const auto& exec : execs) {
            summary += "  - " + exec + "\n";
        }
    }
    if (opens.empty() && connects.empty() && execs.empty()) {
        summary += "No traced events detected during the profiling window.\n";
    }

    return summary;
}

std::vector<std::pair<std::string, std::string>> get_open_resources(pid_t pid) {
    std::vector<std::pair<std::string, std::string>> resources;
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    if (!utils::file_exists(fd_dir)) return resources;
    
    try {
        for (const auto& entry : fs::directory_iterator(fd_dir)) {
            std::string fd_name = entry.path().filename().string();
            std::error_code ec;
            fs::path target = fs::read_symlink(entry.path(), ec);
            if (!ec) {
                resources.push_back({fd_name, target.string()});
            }
        }
    } catch (...) {}
    return resources;
}

std::unordered_map<std::string, uint64_t> get_disk_stats() {
    std::unordered_map<std::string, uint64_t> stats;
    std::ifstream file("/proc/diskstats");
    if (!file.is_open()) return stats;
    
    std::string line;
    while (std::getline(file, line)) {
        std::vector<std::string> fields = split_whitespace(line);
        if (fields.size() >= 14) {
            std::string dev_name = fields[2];
            uint64_t reads = safe_stoull(fields[3]);
            uint64_t sectors_read = safe_stoull(fields[5]);
            uint64_t writes = safe_stoull(fields[7]);
            uint64_t sectors_written = safe_stoull(fields[9]);
            uint64_t io_time = safe_stoull(fields[12]);
            
            stats[dev_name + "_reads"] = reads;
            stats[dev_name + "_read_bytes"] = sectors_read * 512;
            stats[dev_name + "_writes"] = writes;
            stats[dev_name + "_write_bytes"] = sectors_written * 512;
            stats[dev_name + "_io_time_ms"] = io_time;
        }
    }
    return stats;
}

} // namespace telemetry

