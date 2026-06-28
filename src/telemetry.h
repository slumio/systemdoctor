#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

struct ProcessTelemetry {
  pid_t pid = 0;
  std::string name = "";
  pid_t ppid = 0;
  std::string state = "";
  int thread_count = 0;

  // CPU user/sys time in clock ticks
  uint64_t utime = 0;
  uint64_t stime = 0;

  // Memory
  uint64_t rss_bytes = 0;
  uint64_t vsize_bytes = 0;
  uint64_t minflt = 0; // Minor faults
  uint64_t majflt = 0; // Major faults

  // I/O
  uint64_t read_bytes = 0;
  uint64_t write_bytes = 0;
  uint64_t read_char = 0;
  uint64_t write_char = 0;
  uint64_t syscr = 0; // Read syscalls
  uint64_t syscw = 0; // Write syscalls

  // Scheduler
  uint64_t voluntary_ctxt_switches = 0;
  uint64_t involuntary_ctxt_switches = 0;
  uint64_t sched_run_ticks = 0;  // Time spent on CPU
  uint64_t sched_wait_ticks = 0; // Time spent waiting in runqueue
  uint64_t sched_timeslices = 0; // Timeslices run

  std::vector<pid_t> child_pids;
  std::vector<std::string> env;
};

struct SystemTelemetry {
  std::string load_avg = "";
  uint64_t mem_total_kb = 0;
  uint64_t mem_free_kb = 0;
  uint64_t mem_available_kb = 0;
  uint64_t mem_cached_kb = 0;
  uint64_t mem_buffers_kb = 0;

  // CPU cycles from /proc/stat
  uint64_t cpu_user = 0;
  uint64_t cpu_nice = 0;
  uint64_t cpu_system = 0;
  uint64_t cpu_idle = 0;
  uint64_t cpu_iowait = 0;
  uint64_t cpu_irq = 0;
  uint64_t cpu_softirq = 0;

  std::string disk_usage_summary = "";
};

namespace telemetry {

pid_t find_pid_by_name(const std::string &name);
ProcessTelemetry collect_process_telemetry(pid_t pid);
SystemTelemetry collect_system_telemetry();
std::string collect_ebpf_telemetry(pid_t pid, int duration_seconds = 5);
std::string serialize_telemetry_to_json_string(const ProcessTelemetry &pt,
                                               const SystemTelemetry &st);
std::vector<std::pair<std::string, std::string>> get_open_resources(pid_t pid);
std::unordered_map<std::string, uint64_t> get_disk_stats();

} // namespace telemetry

#endif // TELEMETRY_H
