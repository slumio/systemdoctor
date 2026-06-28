#include "causal_engine.h"
#include "nlohmann/json.hpp"
#include "telemetry.h"
#include "utils.h"
#include "vendor/tsl/robin_map.h"
#include "vendor/tsl/robin_set.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

void CausalGraph::add_node(const GraphNode &node) {
  GraphNode allocated_node = node;
  allocated_node.id = arena.alloc(node.id);
  allocated_node.name = arena.alloc(node.name);
  allocated_node.state = arena.alloc(node.state);
  allocated_node.anomaly_reason = arena.alloc(node.anomaly_reason);
  nodes[allocated_node.id] = allocated_node;
}

void CausalGraph::add_edge(std::string_view from, std::string_view to,
                           EdgeType type, std::string_view details) {
  GraphEdge edge;
  edge.from_id = arena.alloc(from);
  edge.to_id = arena.alloc(to);
  edge.type = type;
  edge.details = arena.alloc(details);
  edges.push_back(edge);
}

static std::string query_daemon() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return "";
  
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, "/tmp/syspilot.sock", sizeof(addr.sun_path) - 1);
  
  struct timeval tv = {0, 50000}; // 50ms timeout
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

// take_proc_snapshot — returns a tsl::robin_map for cache-friendly PID lookup
// Fast path: queries syspilotd over UNIX socket (microsecond latency)
// with simdjson ondemand parsing (SIMD-accelerated, zero-copy)
// Slow path: falls back to /proc directory scan
tsl::robin_map<pid_t, GraphNode> CausalGraph::take_proc_snapshot() {
  tsl::robin_map<pid_t, GraphNode> snapshot;
  snapshot.reserve(512);

  std::string daemon_res = query_daemon();
  if (!daemon_res.empty()) {
    try {
      // simdjson ondemand — SIMD-accelerated, zero-copy parsing
      simdjson::ondemand::parser sj_parser;
      simdjson::padded_string padded(daemon_res.data(), daemon_res.size());
      auto doc = sj_parser.iterate(padded);

      std::string_view status;
      if (doc["status"].get_string().get(status) == simdjson::SUCCESS
          && status == "ok") {
        auto procs = doc["processes"].get_array();
        for (auto proc : procs) {
          int64_t pid_raw = 0;
          if (proc["pid"].get_int64().get(pid_raw) != simdjson::SUCCESS) continue;
          pid_t pid = (pid_t)pid_raw;

          ProcessTelemetry pt = telemetry::collect_process_telemetry(pid);
          if (pt.pid == 0) continue;

          GraphNode node;
          node.id   = arena.alloc("pid:" + std::to_string(pid));
          node.type = NodeType::PROCESS;
          node.name = arena.alloc(pt.name);
          node.pid  = pid;
          node.state = arena.alloc(pt.state);
          node.read_bytes  = pt.read_bytes;
          node.write_bytes = pt.write_bytes;
          node.cpu_usage_pct = (double)(pt.utime + pt.stime);
          snapshot[pid] = node;
        }
        return snapshot;
      }
    } catch (...) {
      spdlog::debug("[engine] simdjson daemon parse failed — falling back to /proc");
    }
  }

  // Fallback: scan /proc directly
  try {
    for (const auto &entry : fs::directory_iterator("/proc")) {
      if (!entry.is_directory()) continue;
      const std::string pid_str = entry.path().filename().string();
      bool only_digits = true;
      for (char c : pid_str)
        if (!std::isdigit(c)) { only_digits = false; break; }
      if (!only_digits || pid_str.empty()) continue;

      pid_t pid = (pid_t)std::stoi(pid_str);
      ProcessTelemetry pt = telemetry::collect_process_telemetry(pid);
      if (pt.pid == 0) continue;

      GraphNode node;
      node.id   = arena.alloc("pid:" + std::to_string(pid));
      node.type = NodeType::PROCESS;
      node.name = arena.alloc(pt.name);
      node.pid  = pid;
      node.state = arena.alloc(pt.state);
      node.read_bytes  = pt.read_bytes;
      node.write_bytes = pt.write_bytes;
      node.cpu_usage_pct = (double)(pt.utime + pt.stime);
      snapshot[pid] = node;
    }
  } catch (...) {}
  return snapshot;
}

void CausalGraph::build_graph(int interval_seconds, bool use_ebpf,
                              pid_t target_pid) {
  arena.reset();
  bool ebpf_running = false;
  std::string bpftrace_log = "/tmp/syspilot_bpftrace.log";
  if (use_ebpf && target_pid > 0) {
    std::string bpftrace_cmd = "bpftrace";
    bool has_privileges = false;
    if (getuid() == 0) {
      has_privileges = true;
    } else if (std::system("sudo -n true 2>/dev/null") == 0) {
      has_privileges = true;
      bpftrace_cmd = "sudo bpftrace";
    }

    if (has_privileges) {
      // Clean up old log
      std::error_code ec;
      std::filesystem::remove(bpftrace_log, ec);

      std::string filter = "pid == " + std::to_string(target_pid) +
                           " || ppid == " + std::to_string(target_pid);

      std::string script = "tracepoint:syscalls:sys_enter_open,tracepoint:"
                           "syscalls:sys_enter_openat /" +
                           filter +
                           "/ { "
                           "  printf(\"OPEN | %d | %s | %s\\n\", pid, comm, "
                           "str(args->filename)); "
                           "} "
                           "tracepoint:syscalls:sys_enter_connect /" +
                           filter +
                           "/ { "
                           "  printf(\"CONNECT | %d | %s | family: %d\\n\", "
                           "pid, comm, args->uservaddr->sa_family); "
                           "} "
                           "tracepoint:syscalls:sys_enter_execve /" +
                           filter +
                           "/ { "
                           "  printf(\"EXEC | %d | %s | %s\\n\", pid, comm, "
                           "str(args->filename)); "
                           "}";

      std::string cmd = "timeout " + std::to_string(interval_seconds) + " " +
                        bpftrace_cmd + " -e '" + script + "' > " +
                        bpftrace_log + " 2>/dev/null &";
      utils::run_command_output(cmd);
      ebpf_running = true;
      std::cout << "🚀 [eBPF] Active tracing enabled for PID " << target_pid
                << "..." << std::endl;
    } else {
      std::cout << "⚠️  [eBPF] Insufficient privileges. Falling back to "
                   "standard procfs polling..."
                << std::endl;
    }
  }

  // 1. Take Snapshot 1 — tsl::robin_map for O(1) avg PID lookup
  tsl::robin_map<pid_t, GraphNode> snap1 = take_proc_snapshot();
  std::unordered_map<std::string, uint64_t> disk1 = telemetry::get_disk_stats();

  // Sleep for the interval
  std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));

  // 2. Take Snapshot 2
  tsl::robin_map<pid_t, GraphNode> snap2 = take_proc_snapshot();
  std::unordered_map<std::string, uint64_t> disk2 = telemetry::get_disk_stats();

  long clk_tck = sysconf(_SC_CLK_TCK);
  if (clk_tck <= 0)
    clk_tck = 100;

  // 3. Populate process nodes & calculate rates
  for (const auto &pair2 : snap2) {
    pid_t pid = pair2.first;
    GraphNode node = pair2.second;

    auto it1 = snap1.find(pid);
    if (it1 != snap1.end()) {
      GraphNode prev_node = it1->second;
      double cpu_delta = node.cpu_usage_pct - prev_node.cpu_usage_pct;
      node.cpu_usage_pct =
          (cpu_delta / (double)(clk_tck * interval_seconds)) * 100.0;

      double r_delta = (node.read_bytes >= prev_node.read_bytes)
                           ? (node.read_bytes - prev_node.read_bytes)
                           : 0;
      double w_delta = (node.write_bytes >= prev_node.write_bytes)
                           ? (node.write_bytes - prev_node.write_bytes)
                           : 0;

      node.read_rate_kb = r_delta / (1024.0 * interval_seconds);
      node.write_rate_kb = w_delta / (1024.0 * interval_seconds);
    } else {
      node.cpu_usage_pct = 0.0;
      node.read_rate_kb = 0.0;
      node.write_rate_kb = 0.0;
    }

    // Simple Anomaly Detection rules
    if (node.state == "D") {
      node.is_anomalous = true;
      node.anomaly_reason = "Process is in uninterruptible sleep (D-state), "
                            "likely blocked on disk I/O";
    } else if (node.cpu_usage_pct > 80.0) {
      node.is_anomalous = true;
      node.anomaly_reason = "High CPU utilization (" +
                            std::to_string((int)node.cpu_usage_pct) + "%)";
    } else if (node.write_rate_kb > 5000.0) { // > 5 MB/s
      node.is_anomalous = true;
      node.anomaly_reason = "High disk write rate (" +
                            std::to_string((int)node.write_rate_kb) + " KB/s)";
    } else if (node.read_rate_kb > 5000.0) {
      node.is_anomalous = true;
      node.anomaly_reason = "High disk read rate (" +
                            std::to_string((int)node.read_rate_kb) + " KB/s)";
    }

    add_node(node);
  }

  // 4. Calculate device anomalies from disk stats
  for (const auto &pair2 : disk2) {
    std::string key = pair2.first;
    if (utils::ends_with(key, "_io_time_ms")) {
      std::string dev_name = key.substr(0, key.length() - 11);
      uint64_t io_time2 = pair2.second;
      uint64_t io_time1 = 0;
      auto it1 = disk1.find(key);
      if (it1 != disk1.end()) {
        io_time1 = it1->second;
      }

      double io_delta = (io_time2 >= io_time1) ? (io_time2 - io_time1) : 0;
      double io_util_pct =
          (io_delta / (double)(interval_seconds * 1000.0)) * 100.0;

      GraphNode dev_node;
      dev_node.id = "resource:/dev/" + dev_name;
      dev_node.type = NodeType::RESOURCE;
      dev_node.name = "/dev/" + dev_name;

      // Get bytes written/read
      auto r_bytes_it2 = disk2.find(dev_name + "_read_bytes");
      auto r_bytes_it1 = disk1.find(dev_name + "_read_bytes");
      auto w_bytes_it2 = disk2.find(dev_name + "_write_bytes");
      auto w_bytes_it1 = disk1.find(dev_name + "_write_bytes");

      if (r_bytes_it2 != disk2.end() && r_bytes_it1 != disk1.end()) {
        double r_bytes_delta = (r_bytes_it2->second >= r_bytes_it1->second)
                                   ? (r_bytes_it2->second - r_bytes_it1->second)
                                   : 0;
        dev_node.read_rate_kb = r_bytes_delta / (1024.0 * interval_seconds);
      }
      if (w_bytes_it2 != disk2.end() && w_bytes_it1 != disk1.end()) {
        double w_bytes_delta = (w_bytes_it2->second >= w_bytes_it1->second)
                                   ? (w_bytes_it2->second - w_bytes_it1->second)
                                   : 0;
        dev_node.write_rate_kb = w_bytes_delta / (1024.0 * interval_seconds);
      }

      if (io_util_pct > 80.0) {
        dev_node.is_anomalous = true;
        dev_node.anomaly_reason = "Device utilization is high (" +
                                  std::to_string((int)io_util_pct) +
                                  "% time active)";
      }
      add_node(dev_node);
    }
  }
  // 5. Connect Parent-Child (SPAWNED_BY) and Map FDs to Resources
  std::unordered_set<std::string> active_files;

  // Pass 1: Identify active files (files in tmp, var/lib, home that are
  // currently being read or written to)
  for (const auto &pair : nodes) {
    if (pair.second.type != NodeType::PROCESS)
      continue;
    if (pair.second.read_rate_kb > 0.1 || pair.second.write_rate_kb > 0.1) {
      std::vector<std::pair<std::string, std::string>> open_res =
          telemetry::get_open_resources(pair.second.pid);
      for (const auto &res : open_res) {
        std::string path = res.second;
        if (path.find("/var/lib/") != std::string::npos ||
            path.find("/tmp/") != std::string::npos ||
            path.find("/home/") != std::string::npos) {
          active_files.insert(path);
        }
      }
    }
  }

  // Pass 2: Connect processes to active resources and block devices
  std::vector<GraphNode> new_resource_nodes;
  for (const auto &pair : nodes) {
    if (pair.second.type != NodeType::PROCESS)
      continue;
    pid_t pid = pair.second.pid;
    ProcessTelemetry pt = telemetry::collect_process_telemetry(pid);

    // Spawned by
    if (pt.ppid != 0) {
      std::string parent_id = "pid:" + std::to_string(pt.ppid);
      if (nodes.find(parent_id) != nodes.end()) {
        add_edge(pair.second.id, parent_id, EdgeType::SPAWNED_BY);
      }
    }

    // Map Open FDs
    std::vector<std::pair<std::string, std::string>> open_res =
        telemetry::get_open_resources(pid);
    std::unordered_set<std::string> mapped_resources;

    for (const auto &res : open_res) {
      std::string path = res.second;
      std::string resource_id = "";
      std::string resource_name = "";

      if (utils::starts_with(path, "/dev/")) {
        resource_id = "resource:" + path;
        resource_name = path;
      } else if (utils::starts_with(path, "socket:[")) {
        // Map sockets only if the process is actively doing IO
        if (pair.second.read_rate_kb > 0.1 || pair.second.write_rate_kb > 0.1) {
          resource_id = "resource:" + path;
          resource_name = path;
        }
      } else if (active_files.find(path) != active_files.end()) {
        resource_id = "resource:" + path;
        resource_name = path;
      }

      if (!resource_id.empty() &&
          mapped_resources.find(resource_id) == mapped_resources.end()) {
        mapped_resources.insert(resource_id);

        // Add resource node if not present
        if (nodes.find(resource_id) == nodes.end()) {
          GraphNode rnode;
          rnode.id = resource_id;
          rnode.type = NodeType::RESOURCE;
          rnode.name = resource_name;
          new_resource_nodes.push_back(rnode);
        }

        // Add access edges
        if (pair.second.write_rate_kb > 0.1) {
          add_edge(pair.second.id, resource_id, EdgeType::WRITES_TO,
                   "FD: " + res.first);
        } else {
          add_edge(pair.second.id, resource_id, EdgeType::READS_FROM,
                   "FD: " + res.first);
        }

        // D-state processes are BLOCKED_ON the resources they have open
        if (pair.second.state == "D") {
          add_edge(pair.second.id, resource_id, EdgeType::BLOCKED_ON,
                   "Process blocked in I/O wait on resource");
        }
      }
    }

    // If process is in D-state but no block device was mapped, link it to the
    // primary disk device
    if (pair.second.state == "D") {
      bool has_blocked_edge = false;
      for (const auto &edge : edges) {
        if (edge.from_id == pair.second.id &&
            edge.type == EdgeType::BLOCKED_ON) {
          has_blocked_edge = true;
          break;
        }
      }
      if (!has_blocked_edge) {
        // Find most active disk
        std::string target_disk = "resource:/dev/sda";
        double max_util = 0;
        for (const auto &n : nodes) {
          if (n.second.type == NodeType::RESOURCE &&
              utils::starts_with(n.first, "resource:/dev/")) {
            double total_io = n.second.read_rate_kb + n.second.write_rate_kb;
            if (total_io > max_util) {
              max_util = total_io;
              target_disk = n.first;
            }
          }
        }
        add_edge(pair.second.id, target_disk, EdgeType::BLOCKED_ON,
                 "Process blocked on primary disk device");
      }
    }
  }

  for (const auto &rnode : new_resource_nodes) {
    if (nodes.find(rnode.id) == nodes.end()) {
      add_node(rnode);
    }
  }

  std::unordered_map<std::string_view, std::vector<std::string_view>> resource_accessors;
  for (const auto &edge : edges) {
    if (edge.type == EdgeType::READS_FROM || edge.type == EdgeType::WRITES_TO ||
        edge.type == EdgeType::BLOCKED_ON) {
      resource_accessors[edge.to_id].push_back(edge.from_id);
    }
  }

  for (const auto &pair : resource_accessors) {
    std::string_view res_id = pair.first;
    const std::vector<std::string_view> &accessors = pair.second;

    if (accessors.size() > 1) {
      // Check if multiple accessors have high write/read rates
      std::vector<std::string_view> active_writers;
      for (const auto &p_id : accessors) {
        auto it = nodes.find(p_id);
        if (it != nodes.end() && (it->second.write_rate_kb > 500.0 ||
                                  it->second.read_rate_kb > 500.0)) {
          active_writers.push_back(p_id);
        }
      }

      if (active_writers.size() > 1) {
        // Add CONTENDS_WITH edges between active writers/readers
        std::string details = "Shared access to " + std::string(res_id);
        for (size_t i = 0; i < active_writers.size(); ++i) {
          for (size_t j = i + 1; j < active_writers.size(); ++j) {
            add_edge(active_writers[i], active_writers[j],
                     EdgeType::CONTENDS_WITH, details);
            add_edge(active_writers[j], active_writers[i],
                     EdgeType::CONTENDS_WITH, details);
          }
        }
      }
    }
  }

  // 7. Parse eBPF Tracing Log if enabled and run
  if (ebpf_running) {
    // Sleep briefly to let the log write flush
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::ifstream file(bpftrace_log);
    if (file.is_open()) {
      std::string line;
      while (std::getline(file, line)) {
        if (line.empty())
          continue;

        std::vector<std::string> parts = utils::split(line, " | ");
        if (parts.size() >= 4) {
          std::string event_type = utils::trim(parts[0]);
          pid_t event_pid = 0;
          try {
            event_pid = std::stoi(utils::trim(parts[1]));
          } catch (...) {
            continue;
          }

          std::string comm = utils::trim(parts[2]);
          std::string details = utils::trim(parts[3]);

          std::string pnode_id = "pid:" + std::to_string(event_pid);

          // Ensure process node exists
          if (nodes.find(pnode_id) == nodes.end()) {
            GraphNode pnode;
            pnode.id = pnode_id;
            pnode.pid = event_pid;
            pnode.name = comm;
            pnode.type = NodeType::PROCESS;
            pnode.state = "S";
            pnode.cpu_usage_pct = 0.0;
            pnode.read_rate_kb = 0.0;
            pnode.write_rate_kb = 0.0;
            add_node(pnode);
          }

          if (event_type == "OPEN" || event_type == "OPENAT") {
            std::string res_id = "resource:" + details;
            if (nodes.find(res_id) == nodes.end()) {
              GraphNode rnode;
              rnode.id = res_id;
              rnode.name = details;
              rnode.type = NodeType::RESOURCE;
              add_node(rnode);
            }
            // Connect process to resource
            add_edge(pnode_id, res_id, EdgeType::READS_FROM,
                     "eBPF trace: opened file");
          } else if (event_type == "CONNECT") {
            std::string res_id = "resource:socket_" + details;
            if (nodes.find(res_id) == nodes.end()) {
              GraphNode rnode;
              rnode.id = res_id;
              rnode.name = "Socket (" + details + ")";
              rnode.type = NodeType::RESOURCE;
              add_node(rnode);
            }
            add_edge(pnode_id, res_id, EdgeType::READS_FROM,
                     "eBPF trace: network connection");
          } else if (event_type == "EXEC") {
            std::string res_id = "resource:" + details;
            if (nodes.find(res_id) == nodes.end()) {
              GraphNode rnode;
              rnode.id = res_id;
              rnode.name = details;
              rnode.type = NodeType::RESOURCE;
              add_node(rnode);
            }
            add_edge(pnode_id, res_id, EdgeType::READS_FROM,
                     "eBPF trace: executed binary");
          }
        }
      }
      file.close();
    }
    // Clean up the log file
    std::error_code ec;
    std::filesystem::remove(bpftrace_log, ec);
  }
}

std::vector<std::string>
CausalGraph::trace_root_cause(const std::string &start_node_id) {
  std::vector<std::string> path;
  auto start_it = nodes.find(std::string_view(start_node_id));
  if (start_it == nodes.end())
    return path;

  std::queue<std::string_view> q;
  // tsl::robin_set: open-addressing with robin-hood probing
  // 30-40% faster lookup than std::unordered_set on hot BFS paths
  tsl::robin_set<std::string_view> visited;
  visited.reserve(64);

  q.push(start_it->first);
  visited.insert(start_it->first);

  while (!q.empty()) {
    std::string_view current = q.front();
    q.pop();
    path.push_back(std::string(current));

    // Find neighbors to traverse backwards.
    // What constitutes a backward causal relationship?
    // 1. If A is BLOCKED_ON B (B is causing A's blockage)
    // 2. If A CONTENDS_WITH B (B is contending with A)
    // 3. If A was SPAWNED_BY B (B is parent process of A)
    // 4. If resource R is written to/read by process B (B is saturating
    // resource R)

    auto curr_node = nodes[current];
    if (curr_node.type == NodeType::PROCESS) {
      // Find edges going out of current process representing blocks, parents,
      // or contentions
      for (const auto &edge : edges) {
        if (edge.from_id == current) {
          if (edge.type == EdgeType::BLOCKED_ON ||
              edge.type == EdgeType::SPAWNED_BY ||
              edge.type == EdgeType::CONTENDS_WITH) {
            if (visited.find(edge.to_id) == visited.end()) {
              visited.insert(edge.to_id);
              q.push(edge.to_id);
            }
          }

          // Also traverse to open files if they are anomalous or accessed by
          // another anomalous process
          if (edge.type == EdgeType::READS_FROM ||
              edge.type == EdgeType::WRITES_TO) {
            std::string_view res_id = edge.to_id;
            auto res_it = nodes.find(res_id);
            if (res_it != nodes.end()) {
              bool should_traverse = res_it->second.is_anomalous;
              if (!should_traverse) {
                // Check if another process is accessing this resource
                // anomalously
                for (const auto &other_edge : edges) {
                  if (other_edge.to_id == res_id &&
                      other_edge.from_id != current) {
                    auto proc_it = nodes.find(other_edge.from_id);
                    if (proc_it != nodes.end() &&
                        proc_it->second.is_anomalous) {
                      should_traverse = true;
                      break;
                    }
                  }
                }
              }
              if (should_traverse && visited.find(res_id) == visited.end()) {
                visited.insert(res_id);
                q.push(res_id);
              }
            }
          }
        }
      }
    } else if (curr_node.type == NodeType::RESOURCE) {
      // If current is resource (like disk), who is causing the issue?
      // Find processes that have WRITES_TO or READS_FROM edges pointing to this
      // resource. Prioritize processes with high write/read rates.
      std::vector<std::pair<std::string_view, double>> active_writers;
      for (const auto &edge : edges) {
        if (edge.to_id == current && (edge.type == EdgeType::WRITES_TO ||
                                      edge.type == EdgeType::READS_FROM)) {
          auto proc_it = nodes.find(edge.from_id);
          if (proc_it != nodes.end()) {
            double total_io =
                proc_it->second.write_rate_kb + proc_it->second.read_rate_kb;
            active_writers.push_back({edge.from_id, total_io});
          }
        }
      }

      // Sort by active IO rate desc
      std::sort(
          active_writers.begin(), active_writers.end(),
          [](const auto &a, const auto &b) { return a.second > b.second; });

      // Add top 3 active processes to queue
      for (size_t i = 0; i < std::min(active_writers.size(), (size_t)3); ++i) {
        std::string_view p_id = active_writers[i].first;
        if (visited.find(p_id) == visited.end()) {
          visited.insert(p_id);
          q.push(p_id);
        }
      }
    }
  }

  return path;
}


std::string CausalGraph::serialize_chain_to_json(
    const std::vector<std::string> &path_nodes) {
  json j;
  j["symptom_node"] = path_nodes.empty() ? "" : path_nodes.front();

  json j_nodes = json::array();
  tsl::robin_set<std::string_view> path_set(path_nodes.begin(),
                                                path_nodes.end());

  for (const auto &node_id : path_nodes) {
    auto it = nodes.find(node_id);
    if (it != nodes.end()) {
      json jn;
      jn["id"] = it->second.id;
      jn["type"] =
          (it->second.type == NodeType::PROCESS) ? "process" : "resource";
      jn["name"] = it->second.name;
      if (it->second.type == NodeType::PROCESS) {
        jn["pid"] = it->second.pid;
        jn["state"] = it->second.state;
        jn["cpu_usage_pct"] = it->second.cpu_usage_pct;
      }
      jn["read_rate_kb"] = it->second.read_rate_kb;
      jn["write_rate_kb"] = it->second.write_rate_kb;
      jn["is_anomalous"] = it->second.is_anomalous;
      jn["anomaly_reason"] = it->second.anomaly_reason;
      j_nodes.push_back(jn);
    }
  }
  j["nodes"] = j_nodes;

  json j_edges = json::array();
  for (const auto &edge : edges) {
    if (path_set.find(edge.from_id) != path_set.end() &&
        path_set.find(edge.to_id) != path_set.end()) {
      json je;
      je["from"] = edge.from_id;
      je["to"] = edge.to_id;

      std::string type_str = "ACCESS";
      switch (edge.type) {
      case EdgeType::SPAWNED_BY:
        type_str = "SPAWNED_BY";
        break;
      case EdgeType::READS_FROM:
        type_str = "READS_FROM";
        break;
      case EdgeType::WRITES_TO:
        type_str = "WRITES_TO";
        break;
      case EdgeType::BLOCKED_ON:
        type_str = "BLOCKED_ON";
        break;
      case EdgeType::CONTENDS_WITH:
        type_str = "CONTENDS_WITH";
        break;
      }
      je["type"] = type_str;
      je["details"] = edge.details;
      j_edges.push_back(je);
    }
  }
  j["edges"] = j_edges;

  return j.dump(4);
}

std::string
CausalGraph::export_graph_to_dot(const std::vector<std::string> &path_nodes) {
  tsl::robin_set<std::string_view> path_set(path_nodes.begin(),
                                             path_nodes.end());
  std::ostringstream oss;
  oss << "digraph CausalTrace {\n";
  oss << "    // Graph styling\n";
  oss << "    backgroundcolor=\"#0f172a\";\n";
  oss << "    node [style=\"filled,rounded\", shape=box, "
         "fontname=\"Helvetica,Arial,sans-serif\", fontsize=10, "
         "penwidth=1.5];\n";
  oss << "    edge [fontname=\"Helvetica,Arial,sans-serif\", fontsize=8, "
         "color=\"#94a3b8\", fontcolor=\"#cbd5e1\", penwidth=1.5];\n\n";

  // Write nodes
  for (const auto &node_id : path_nodes) {
    auto it = nodes.find(node_id);
    if (it != nodes.end()) {
      std::string label = std::string(it->second.name);
      if (it->second.type == NodeType::PROCESS) {
        label += " (PID: " + std::to_string(it->second.pid) + ")";
        if (it->second.read_rate_kb > 0.1 || it->second.write_rate_kb > 0.1) {
          label += "\\nI/O: R " + std::to_string((int)it->second.read_rate_kb) +
                   "KB/s, W " + std::to_string((int)it->second.write_rate_kb) +
                   "KB/s";
        }
      } else {
        label = "[Resource]\\n" + label;
      }

      std::string fillColor = "#1e293b";
      std::string strokeColor = "#475569";
      std::string fontColor = "#f8fafc";

      if (it->second.is_anomalous) {
        fillColor = "#7f1d1d";
        strokeColor = "#ef4444";
      } else if (it->second.type == NodeType::RESOURCE) {
        fillColor = "#0f172a";
        strokeColor = "#334155";
      } else {
        fillColor = "#0284c7";
        strokeColor = "#38bdf8";
      }

      oss << "    \"" << node_id << "\" [label=\"" << label
          << "\", fillcolor=\"" << fillColor << "\", color=\"" << strokeColor
          << "\", fontcolor=\"" << fontColor << "\"];\n";
    }
  }
  oss << "\n";

  // Write edges
  for (const auto &edge : edges) {
    if (path_set.find(edge.from_id) != path_set.end() &&
        path_set.find(edge.to_id) != path_set.end()) {
      std::string type_str = "ACCESS";
      std::string color = "#64748b";
      switch (edge.type) {
      case EdgeType::SPAWNED_BY:
        type_str = "SPAWNED_BY";
        color = "#475569";
        break;
      case EdgeType::READS_FROM:
        type_str = "READS_FROM";
        color = "#38bdf8";
        break;
      case EdgeType::WRITES_TO:
        type_str = "WRITES_TO";
        color = "#f97316";
        break;
      case EdgeType::BLOCKED_ON:
        type_str = "BLOCKED_ON";
        color = "#ef4444";
        break;
      case EdgeType::CONTENDS_WITH:
        type_str = "CONTENDS_WITH";
        color = "#eab308";
        break;
      }
      std::string label = type_str;
      if (!edge.details.empty()) {
        label += " (" + std::string(edge.details) + ")";
      }

      size_t pos = 0;
      while ((pos = label.find('"', pos)) != std::string::npos) {
        label.replace(pos, 1, "'");
        pos++;
      }

      oss << "    \"" << edge.from_id << "\" -> \"" << edge.to_id
          << "\" [label=\"" << label << "\", color=\"" << color << "\"];\n";
    }
  }

  oss << "}\n";
  return oss.str();
}

std::string CausalGraph::export_graph_to_mermaid(
    const std::vector<std::string> &path_nodes) {
  tsl::robin_set<std::string_view> path_set(path_nodes.begin(),
                                                path_nodes.end());
  std::ostringstream oss;
  oss << "graph TD\n";
  oss << "    classDef process "
         "fill:#0284c7,stroke:#38bdf8,color:#fff,stroke-width:2px;\n";
  oss << "    classDef resource "
         "fill:#1e293b,stroke:#475569,color:#cbd5e1,stroke-width:2px;\n";
  oss << "    classDef anomalous "
         "fill:#7f1d1d,stroke:#ef4444,color:#fff,stroke-width:2px;\n\n";

  for (const auto &node_id : path_nodes) {
    auto it = nodes.find(node_id);
    if (it != nodes.end()) {
      std::string label = std::string(it->second.name);
      if (it->second.type == NodeType::PROCESS) {
        label += " (PID: " + std::to_string(it->second.pid) + ")";
        if (it->second.read_rate_kb > 0.1 || it->second.write_rate_kb > 0.1) {
          label += " [IO: R " + std::to_string((int)it->second.read_rate_kb) +
                   "KB/s, W " + std::to_string((int)it->second.write_rate_kb) +
                   "KB/s]";
        }
      } else {
        label = "[Resource] " + label;
      }

      std::string clean_label = label;
      size_t pos = 0;
      while ((pos = clean_label.find('"', pos)) != std::string::npos) {
        clean_label.replace(pos, 1, "'");
        pos++;
      }

      std::string safe_node_id = node_id;
      std::replace(safe_node_id.begin(), safe_node_id.end(), ':', '_');
      std::replace(safe_node_id.begin(), safe_node_id.end(), '/', '_');
      std::replace(safe_node_id.begin(), safe_node_id.end(), '.', '_');
      std::replace(safe_node_id.begin(), safe_node_id.end(), '[', '_');
      std::replace(safe_node_id.begin(), safe_node_id.end(), ']', '_');
      std::replace(safe_node_id.begin(), safe_node_id.end(), ' ', '_');

      oss << "    " << safe_node_id << "[\"" << clean_label << "\"]\n";

      if (it->second.is_anomalous) {
        oss << "    class " << safe_node_id << " anomalous;\n";
      } else if (it->second.type == NodeType::RESOURCE) {
        oss << "    class " << safe_node_id << " resource;\n";
      } else {
        oss << "    class " << safe_node_id << " process;\n";
      }
    }
  }
  oss << "\n";

  for (const auto &edge : edges) {
    if (path_set.find(edge.from_id) != path_set.end() &&
        path_set.find(edge.to_id) != path_set.end()) {
      std::string type_str = "ACCESS";
      switch (edge.type) {
      case EdgeType::SPAWNED_BY:
        type_str = "SPAWNED_BY";
        break;
      case EdgeType::READS_FROM:
        type_str = "READS_FROM";
        break;
      case EdgeType::WRITES_TO:
        type_str = "WRITES_TO";
        break;
      case EdgeType::BLOCKED_ON:
        type_str = "BLOCKED_ON";
        break;
      case EdgeType::CONTENDS_WITH:
        type_str = "CONTENDS_WITH";
        break;
      }
      std::string label = type_str;
      if (!edge.details.empty()) {
        label += " (" + std::string(edge.details) + ")";
      }

      size_t pos = 0;
      while ((pos = label.find('"', pos)) != std::string::npos) {
        label.replace(pos, 1, "'");
        pos++;
      }

      std::string safe_from = std::string(edge.from_id);
      std::replace(safe_from.begin(), safe_from.end(), ':', '_');
      std::replace(safe_from.begin(), safe_from.end(), '/', '_');
      std::replace(safe_from.begin(), safe_from.end(), '.', '_');
      std::replace(safe_from.begin(), safe_from.end(), '[', '_');
      std::replace(safe_from.begin(), safe_from.end(), ']', '_');
      std::replace(safe_from.begin(), safe_from.end(), ' ', '_');

      std::string safe_to = std::string(edge.to_id);
      std::replace(safe_to.begin(), safe_to.end(), ':', '_');
      std::replace(safe_to.begin(), safe_to.end(), '/', '_');
      std::replace(safe_to.begin(), safe_to.end(), '.', '_');
      std::replace(safe_to.begin(), safe_to.end(), '[', '_');
      std::replace(safe_to.begin(), safe_to.end(), ']', '_');
      std::replace(safe_to.begin(), safe_to.end(), ' ', '_');

      oss << "    " << safe_from << " -->|\"" << label << "\"| " << safe_to
          << "\n";
    }
  }

  return oss.str();
}

std::string
CausalGraph::export_graph_to_html(const std::vector<std::string> &path_nodes) {
  tsl::robin_set<std::string_view> path_set(path_nodes.begin(),
                                                path_nodes.end());

  // Create elements JSON
  json cy_elements = json::array();

  // 1. Serialize nodes
  for (const auto &node_id : path_nodes) {
    auto it = nodes.find(node_id);
    if (it != nodes.end()) {
      json n;
      n["data"]["id"] = it->second.id;
      n["data"]["name"] = it->second.name;
      n["data"]["type"] =
          (it->second.type == NodeType::PROCESS) ? "process" : "resource";
      n["data"]["is_anomalous"] = it->second.is_anomalous;
      n["data"]["anomaly_reason"] = it->second.anomaly_reason;
      n["data"]["read_rate_kb"] = it->second.read_rate_kb;
      n["data"]["write_rate_kb"] = it->second.write_rate_kb;

      if (it->second.type == NodeType::PROCESS) {
        n["data"]["pid"] = it->second.pid;
        n["data"]["state"] = it->second.state;
        n["data"]["cpu_usage_pct"] = it->second.cpu_usage_pct;
      }
      cy_elements.push_back(n);
    }
  }

  // 2. Serialize edges
  int edge_counter = 0;
  for (const auto &edge : edges) {
    if (path_set.find(edge.from_id) != path_set.end() &&
        path_set.find(edge.to_id) != path_set.end()) {
      json e;
      e["data"]["id"] = "edge_" + std::to_string(edge_counter++);
      e["data"]["source"] = edge.from_id;
      e["data"]["target"] = edge.to_id;

      std::string type_str = "ACCESS";
      switch (edge.type) {
      case EdgeType::SPAWNED_BY:
        type_str = "SPAWNED_BY";
        break;
      case EdgeType::READS_FROM:
        type_str = "READS_FROM";
        break;
      case EdgeType::WRITES_TO:
        type_str = "WRITES_TO";
        break;
      case EdgeType::BLOCKED_ON:
        type_str = "BLOCKED_ON";
        break;
      case EdgeType::CONTENDS_WITH:
        type_str = "CONTENDS_WITH";
        break;
      }
      e["data"]["type"] = type_str;
      e["data"]["details"] = edge.details;
      cy_elements.push_back(e);
    }
  }

  std::string elements_json = cy_elements.dump();

  std::string symptom_desc = "Unknown Target";
  if (!path_nodes.empty()) {
    auto it = nodes.find(path_nodes.front());
    if (it != nodes.end()) {
      symptom_desc = it->second.name;
      if (it->second.type == NodeType::PROCESS) {
        symptom_desc += " (PID " + std::to_string(it->second.pid) + ")";
      }
    }
  }

  std::ostringstream oss;
  oss << "<!DOCTYPE html>\n";
  oss << "<html lang=\"en\">\n";
  oss << "<head>\n";
  oss << "    <meta charset=\"utf-8\">\n";
  oss << "    <meta name=\"viewport\" content=\"width=device-width, "
         "initial-scale=1\">\n";
  oss << "    <title>SysPilot CausalTrace - Interactive Dependency "
         "Graph</title>\n";
  oss << "    <link rel=\"preconnect\" "
         "href=\"https://fonts.googleapis.com\">\n";
  oss << "    <link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" "
         "crossorigin>\n";
  oss << "    <link "
         "href=\"https://fonts.googleapis.com/"
         "css2?family=Outfit:wght@300;400;600;800&family=JetBrains+Mono:wght@"
         "400;700&display=swap\" rel=\"stylesheet\">\n";
  oss << "    <script "
         "src=\"https://cdnjs.cloudflare.com/ajax/libs/cytoscape/3.26.0/"
         "cytoscape.min.js\"></script>\n";
  oss << "    <script "
         "src=\"https://cdn.jsdelivr.net/npm/dagre@0.8.5/dist/dagre.min.js\"></"
         "script>\n";
  oss << "    <script "
         "src=\"https://cdn.jsdelivr.net/npm/cytoscape-dagre@2.5.0/"
         "cytoscape-dagre.js\"></script>\n";
  oss << "    <script "
         "src=\"https://cdnjs.cloudflare.com/ajax/libs/d3/7.8.5/d3.min.js\"></"
         "script>\n";
  oss << "    <style>\n";
  oss << "        :root {\n";
  oss << "            --bg-color: #0b0f17;\n";
  oss << "            --panel-bg: rgba(17, 24, 39, 0.85);\n";
  oss << "            --border-color: rgba(56, 189, 248, 0.2);\n";
  oss << "            --text-color: #f1f5f9;\n";
  oss << "            --text-muted: #94a3b8;\n";
  oss << "            --cyan: #38bdf8;\n";
  oss << "            --blue: #0284c7;\n";
  oss << "            --red: #ef4444;\n";
  oss << "            --orange: #f97316;\n";
  oss << "            --yellow: #eab308;\n";
  oss << "            --green: #22c55e;\n";
  oss << "        }\n";
  oss << "        * { box-sizing: border-box; }\n";
  oss << "        body {\n";
  oss << "            font-family: 'Outfit', sans-serif;\n";
  oss << "            margin: 0;\n";
  oss << "            padding: 0;\n";
  oss << "            background-color: var(--bg-color);\n";
  oss << "            color: var(--text-color);\n";
  oss << "            overflow: hidden;\n";
  oss << "            height: 100vh;\n";
  oss << "            display: flex;\n";
  oss << "            flex-direction: column;\n";
  oss << "        }\n";
  oss << "        .intro-overlay {\n";
  oss << "            position: fixed;\n";
  oss << "            top: 0; left: 0; width: 100vw; height: 100vh;\n";
  oss << "            background: radial-gradient(circle at center, #0f172a "
         "0%, #020617 100%);\n";
  oss << "            z-index: 100;\n";
  oss << "            display: flex;\n";
  oss << "            flex-direction: column;\n";
  oss << "            align-items: center;\n";
  oss << "            justify-content: center;\n";
  oss << "            transition: opacity 0.8s ease, visibility 0.8s ease;\n";
  oss << "            font-family: 'Outfit', sans-serif;\n";
  oss << "            padding: 2rem;\n";
  oss << "        }\n";
  oss << "        .intro-overlay.hidden {\n";
  oss << "            opacity: 0;\n";
  oss << "            visibility: hidden;\n";
  oss << "        }\n";
  oss << "        .intro-card {\n";
  oss << "            max-width: 700px;\n";
  oss << "            width: 100%;\n";
  oss << "            background: rgba(15, 23, 42, 0.7);\n";
  oss << "            border: 1px solid var(--border-color);\n";
  oss << "            border-radius: 16px;\n";
  oss << "            padding: 2.5rem;\n";
  oss << "            box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.5);\n";
  oss << "            backdrop-filter: blur(12px);\n";
  oss << "            text-align: center;\n";
  oss << "        }\n";
  oss << "        .intro-title {\n";
  oss << "            font-size: 2.2rem;\n";
  oss << "            font-weight: 800;\n";
  oss << "            margin-bottom: 0.5rem;\n";
  oss << "            background: linear-gradient(135deg, #38bdf8 0%, #0284c7 "
         "50%, #f97316 100%);\n";
  oss << "            -webkit-background-clip: text;\n";
  oss << "            -webkit-text-fill-color: transparent;\n";
  oss << "            letter-spacing: -0.025em;\n";
  oss << "        }\n";
  oss << "        .intro-subtitle {\n";
  oss << "            color: var(--text-muted);\n";
  oss << "            font-size: 1rem;\n";
  oss << "            margin-bottom: 2rem;\n";
  oss << "        }\n";
  oss << "        .console-box {\n";
  oss << "            background: #090d16;\n";
  oss << "            border: 1px solid rgba(255, 255, 255, 0.05);\n";
  oss << "            border-radius: 8px;\n";
  oss << "            padding: 1.25rem;\n";
  oss << "            text-align: left;\n";
  oss << "            font-family: 'JetBrains Mono', monospace;\n";
  oss << "            font-size: 0.85rem;\n";
  oss << "            line-height: 1.6;\n";
  oss << "            color: #f8fafc;\n";
  oss << "            margin-bottom: 2rem;\n";
  oss << "            height: 220px;\n";
  oss << "            overflow-y: auto;\n";
  oss << "            box-shadow: inset 0 2px 8px rgba(0, 0, 0, 0.8);\n";
  oss << "        }\n";
  oss << "        .console-line {\n";
  oss << "            opacity: 0;\n";
  oss << "            transform: translateY(5px);\n";
  oss << "            animation: fadeInUp 0.4s forwards;\n";
  oss << "            margin-bottom: 0.5rem;\n";
  oss << "        }\n";
  oss << "        @keyframes fadeInUp {\n";
  oss << "            to { opacity: 1; transform: translateY(0); }\n";
  oss << "        }\n";
  oss << "        .enter-btn {\n";
  oss << "            background: linear-gradient(135deg, #0284c7 0%, #0369a1 "
         "100%);\n";
  oss << "            border: 1px solid var(--cyan);\n";
  oss << "            color: white;\n";
  oss << "            padding: 0.85rem 2rem;\n";
  oss << "            font-size: 1rem;\n";
  oss << "            font-weight: 600;\n";
  oss << "            border-radius: 8px;\n";
  oss << "            cursor: pointer;\n";
  oss << "            transition: all 0.3s;\n";
  oss << "            box-shadow: 0 0 15px rgba(56, 189, 248, 0.2);\n";
  oss << "        }\n";
  oss << "        .enter-btn:hover {\n";
  oss << "            transform: translateY(-2px);\n";
  oss << "            box-shadow: 0 0 25px rgba(56, 189, 248, 0.5);\n";
  oss << "            background: linear-gradient(135deg, #0369a1 0%, #0284c7 "
         "100%);\n";
  oss << "        }\n";
  oss << "        header {\n";
  oss << "            display: flex;\n";
  oss << "            justify-content: space-between;\n";
  oss << "            align-items: center;\n";
  oss << "            padding: 1rem 2rem;\n";
  oss << "            background: rgba(15, 23, 42, 0.9);\n";
  oss << "            border-bottom: 1px solid var(--border-color);\n";
  oss << "            z-index: 10;\n";
  oss << "        }\n";
  oss << "        h1 {\n";
  oss << "            font-size: 1.5rem;\n";
  oss << "            margin: 0;\n";
  oss << "            font-weight: 800;\n";
  oss << "            background: linear-gradient(135deg, #38bdf8 0%, #0284c7 "
         "100%);\n";
  oss << "            -webkit-background-clip: text;\n";
  oss << "            -webkit-text-fill-color: transparent;\n";
  oss << "            display: flex;\n";
  oss << "            align-items: center;\n";
  oss << "            gap: 0.5rem;\n";
  oss << "        }\n";
  oss << "        .subtitle {\n";
  oss << "            font-size: 0.85rem;\n";
  oss << "            color: var(--text-muted);\n";
  oss << "            margin: 0.25rem 0 0 0;\n";
  oss << "        }\n";
  oss << "        .view-selector {\n";
  oss << "            display: flex;\n";
  oss << "            gap: 0.5rem;\n";
  oss << "            background: rgba(30, 41, 59, 0.5);\n";
  oss << "            padding: 0.25rem;\n";
  oss << "            border-radius: 8px;\n";
  oss << "            border: 1px solid rgba(255, 255, 255, 0.05);\n";
  oss << "        }\n";
  oss << "        .view-btn {\n";
  oss << "            background: transparent;\n";
  oss << "            border: none;\n";
  oss << "            color: var(--text-muted);\n";
  oss << "            padding: 0.5rem 1rem;\n";
  oss << "            border-radius: 6px;\n";
  oss << "            cursor: pointer;\n";
  oss << "            font-size: 0.8rem;\n";
  oss << "            font-weight: 600;\n";
  oss << "            transition: all 0.2s;\n";
  oss << "        }\n";
  oss << "        .view-btn.active {\n";
  oss << "            background: var(--blue);\n";
  oss << "            color: var(--text-color);\n";
  oss << "            box-shadow: 0 0 10px rgba(56, 189, 248, 0.3);\n";
  oss << "        }\n";
  oss << "        .symptom-badge {\n";
  oss << "            background: rgba(239, 68, 68, 0.15);\n";
  oss << "            border: 1px solid rgba(239, 68, 68, 0.4);\n";
  oss << "            color: #fca5a5;\n";
  oss << "            padding: 0.25rem 0.75rem;\n";
  oss << "            border-radius: 9999px;\n";
  oss << "            font-size: 0.85rem;\n";
  oss << "            font-weight: 600;\n";
  oss << "        }\n";
  oss << "        .workspace {\n";
  oss << "            display: flex;\n";
  oss << "            flex: 1;\n";
  oss << "            position: relative;\n";
  oss << "            overflow: hidden;\n";
  oss << "        }\n";
  oss << "        #cy, #d3-container {\n";
  oss << "            flex: 1;\n";
  oss << "            height: 100%;\n";
  oss << "            background: radial-gradient(circle at 50% 50%, #111827 "
         "0%, #0b0f17 100%);\n";
  oss << "        }\n";
  oss << "        #d3-container {\n";
  oss << "            display: none;\n";
  oss << "        }\n";
  oss << "        .graph-controls {\n";
  oss << "            position: absolute;\n";
  oss << "            left: 2rem;\n";
  oss << "            top: 2rem;\n";
  oss << "            z-index: 5;\n";
  oss << "            background: var(--panel-bg);\n";
  oss << "            border: 1px solid var(--border-color);\n";
  oss << "            border-radius: 12px;\n";
  oss << "            padding: 0.75rem;\n";
  oss << "            backdrop-filter: blur(12px);\n";
  oss << "            display: flex;\n";
  oss << "            flex-direction: column;\n";
  oss << "            gap: 0.5rem;\n";
  oss << "            box-shadow: 0 10px 25px rgba(0, 0, 0, 0.5);\n";
  oss << "        }\n";
  oss << "        .control-btn {\n";
  oss << "            background: rgba(30, 41, 59, 0.8);\n";
  oss << "            border: 1px solid rgba(255, 255, 255, 0.1);\n";
  oss << "            color: var(--text-color);\n";
  oss << "            padding: 0.5rem 1rem;\n";
  oss << "            border-radius: 6px;\n";
  oss << "            cursor: pointer;\n";
  oss << "            font-size: 0.8rem;\n";
  oss << "            font-weight: 600;\n";
  oss << "            transition: all 0.2s;\n";
  oss << "            text-align: left;\n";
  oss << "        }\n";
  oss << "        .control-btn:hover {\n";
  oss << "            background: var(--blue);\n";
  oss << "            border-color: var(--cyan);\n";
  oss << "            box-shadow: 0 0 10px rgba(56, 189, 248, 0.4);\n";
  oss << "        }\n";
  oss << "        .inspector {\n";
  oss << "            width: 380px;\n";
  oss << "            background: var(--panel-bg);\n";
  oss << "            border-left: 1px solid var(--border-color);\n";
  oss << "            backdrop-filter: blur(20px);\n";
  oss << "            display: flex;\n";
  oss << "            flex-direction: column;\n";
  oss << "            z-index: 5;\n";
  oss << "            box-shadow: -10px 0 30px rgba(0, 0, 0, 0.5);\n";
  oss << "            transition: transform 0.3s ease;\n";
  oss << "        }\n";
  oss << "        .inspector-header {\n";
  oss << "            padding: 1.5rem;\n";
  oss << "            border-bottom: 1px solid rgba(255, 255, 255, 0.08);\n";
  oss << "        }\n";
  oss << "        .inspector-title {\n";
  oss << "            font-size: 1.1rem;\n";
  oss << "            font-weight: 700;\n";
  oss << "            margin: 0;\n";
  oss << "            color: var(--cyan);\n";
  oss << "        }\n";
  oss << "        .inspector-body {\n";
  oss << "            padding: 1.5rem;\n";
  oss << "            flex: 1;\n";
  oss << "            overflow-y: auto;\n";
  oss << "            display: flex;\n";
  oss << "            flex-direction: column;\n";
  oss << "            gap: 1.25rem;\n";
  oss << "        }\n";
  oss << "        .section-title {\n";
  oss << "            font-size: 0.75rem;\n";
  oss << "            text-transform: uppercase;\n";
  oss << "            letter-spacing: 0.05em;\n";
  oss << "            color: var(--text-muted);\n";
  oss << "            margin: 0 0 0.5rem 0;\n";
  oss << "            font-weight: 600;\n";
  oss << "        }\n";
  oss << "        .metric-card {\n";
  oss << "            background: rgba(30, 41, 59, 0.4);\n";
  oss << "            border: 1px solid rgba(255, 255, 255, 0.05);\n";
  oss << "            border-radius: 8px;\n";
  oss << "            padding: 0.75rem 1rem;\n";
  oss << "            display: flex;\n";
  oss << "            justify-content: space-between;\n";
  oss << "            align-items: center;\n";
  oss << "            margin-bottom: 0.5rem;\n";
  oss << "        }\n";
  oss << "        .metric-label {\n";
  oss << "            font-size: 0.85rem;\n";
  oss << "            color: var(--text-muted);\n";
  oss << "        }\n";
  oss << "        .metric-value {\n";
  oss << "            font-size: 0.95rem;\n";
  oss << "            font-weight: 600;\n";
  oss << "            font-family: 'JetBrains Mono', monospace;\n";
  oss << "        }\n";
  oss << "        .anomaly-alert {\n";
  oss << "            background: rgba(239, 68, 68, 0.1);\n";
  oss << "            border: 1px solid rgba(239, 68, 68, 0.3);\n";
  oss << "            border-radius: 8px;\n";
  oss << "            padding: 1rem;\n";
  oss << "            color: #fca5a5;\n";
  oss << "            font-size: 0.85rem;\n";
  oss << "            line-height: 1.4;\n";
  oss << "        }\n";
  oss << "        .empty-state {\n";
  oss << "            display: flex;\n";
  oss << "            flex-direction: column;\n";
  oss << "            align-items: center;\n";
  oss << "            justify-content: center;\n";
  oss << "            height: 100%;\n";
  oss << "            color: var(--text-muted);\n";
  oss << "            text-align: center;\n";
  oss << "            padding: 2rem;\n";
  oss << "            gap: 1rem;\n";
  oss << "        }\n";
  oss << "        .empty-icon {\n";
  oss << "            font-size: 2.5rem;\n";
  oss << "            opacity: 0.5;\n";
  oss << "        }\n";
  oss << "        .legend {\n";
  oss << "            position: absolute;\n";
  oss << "            left: 2rem;\n";
  oss << "            bottom: 2rem;\n";
  oss << "            background: var(--panel-bg);\n";
  oss << "            border: 1px solid var(--border-color);\n";
  oss << "            border-radius: 12px;\n";
  oss << "            padding: 0.75rem 1rem;\n";
  oss << "            z-index: 5;\n";
  oss << "            display: flex;\n";
  oss << "            flex-direction: column;\n";
  oss << "            gap: 0.5rem;\n";
  oss << "            box-shadow: 0 10px 25px rgba(0, 0, 0, 0.5);\n";
  oss << "            font-size: 0.8rem;\n";
  oss << "        }\n";
  oss << "        .legend-item {\n";
  oss << "            display: flex;\n";
  oss << "            align-items: center;\n";
  oss << "            gap: 0.5rem;\n";
  oss << "        }\n";
  oss << "        .legend-color {\n";
  oss << "            width: 12px;\n";
  oss << "            height: 12px;\n";
  oss << "            border-radius: 3px;\n";
  oss << "        }\n";
  oss << "        .node rect {\n";
  oss << "            cursor: pointer;\n";
  oss << "            transition: fill 0.2s, stroke 0.2s;\n";
  oss << "        }\n";
  oss << "        .node text {\n";
  oss << "            pointer-events: none;\n";
  oss << "            user-select: none;\n";
  oss << "        }\n";
  oss << "        .link-labels text {\n";
  oss << "            pointer-events: none;\n";
  oss << "            user-select: none;\n";
  oss << "        }\n";
  oss << "    </style>\n";
  oss << "</head>\n";
  oss << "<body>\n";
  oss << "    <div class=\"intro-overlay\" id=\"intro-screen\">\n";
  oss << "        <div class=\"intro-card\">\n";
  oss << "            <div class=\"intro-title\">SysPilot CausalTrace</div>\n";
  oss << "            <div class=\"intro-subtitle\">Automated Root-Cause "
         "Diagnostic Report</div>\n";
  oss << "            <div class=\"console-box\" id=\"intro-console\"></div>\n";
  oss << "            <button class=\"enter-btn\" "
         "onclick=\"dismissIntro()\">EXPLORE CAUSAL DEPENDENCIES</button>\n";
  oss << "        </div>\n";
  oss << "    </div>\n";
  oss << "    <header>\n";
  oss << "        <div>\n";
  oss << "            <h1>🌐 SysPilot CausalTrace</h1>\n";
  oss << "            <div class=\"subtitle\">Interactive Root Cause "
         "Diagnostics</div>\n";
  oss << "        </div>\n";
  oss << "        <div class=\"view-selector\">\n";
  oss << "            <button class=\"view-btn active\" id=\"btn-cytoscape\" "
         "onclick=\"switchView('cytoscape')\">Cytoscape View</button>\n";
  oss << "            <button class=\"view-btn\" id=\"btn-d3\" "
         "onclick=\"switchView('d3')\">D3.js View</button>\n";
  oss << "        </div>\n";
  oss << "        <div class=\"symptom-badge\">\n";
  oss << "            Symptom: <span id=\"header-symptom\"></span>\n";
  oss << "        </div>\n";
  oss << "    </header>\n";
  oss << "    \n";
  oss << "    <div class=\"workspace\">\n";
  oss << "        <div class=\"graph-controls\">\n";
  oss << "            <button class=\"control-btn\" onclick=\"cy.fit()\">Fit "
         "Screen</button>\n";
  oss << "            <button class=\"control-btn\" "
         "onclick=\"resetHighlight()\">Reset Focus</button>\n";
  oss << "            <button class=\"control-btn\" id=\"toggle-layout-btn\" "
         "onclick=\"toggleLayout()\">Layout: DAGRE</button>\n";
  oss << "        </div>\n";
  oss << "        \n";
  oss << "        <div id=\"cy\"></div>\n";
  oss << "        <div id=\"d3-container\"></div>\n";
  oss << "        \n";
  oss << "        <div class=\"legend\">\n";
  oss << "            <div class=\"legend-item\">\n";
  oss << "                <div class=\"legend-color\" style=\"background: "
         "#0284c7; border: 1px solid #38bdf8;\"></div>\n";
  oss << "                <span>Process Node</span>\n";
  oss << "            </div>\n";
  oss << "            <div class=\"legend-item\">\n";
  oss << "                <div class=\"legend-color\" style=\"background: "
         "#7f1d1d; border: 1px solid #ef4444;\"></div>\n";
  oss << "                <span>Anomalous Process</span>\n";
  oss << "            </div>\n";
  oss << "            <div class=\"legend-item\">\n";
  oss << "                <div class=\"legend-color\" style=\"background: "
         "#111827; border: 1px solid #475569;\"></div>\n";
  oss << "                <span>Resource Node</span>\n";
  oss << "            </div>\n";
  oss << "        </div>\n";
  oss << "        \n";
  oss << "        <div class=\"inspector\" id=\"inspector\">\n";
  oss << "            <div class=\"inspector-header\">\n";
  oss << "                <h3 class=\"inspector-title\" "
         "id=\"inspect-title\">Node Inspector</h3>\n";
  oss << "                <div style=\"font-size: 0.8rem; color: "
         "var(--text-muted); margin-top: 0.25rem;\" "
         "id=\"inspect-subtitle\">Select a node to inspect parameters</div>\n";
  oss << "            </div>\n";
  oss << "            <div class=\"inspector-body\" id=\"inspect-body\">\n";
  oss << "                <div class=\"empty-state\">\n";
  oss << "                    <div class=\"empty-icon\">🔍</div>\n";
  oss << "                    <div>Click on any process or resource node in "
         "the graph to view active system metrics and details.</div>\n";
  oss << "                </div>\n";
  oss << "            </div>\n";
  oss << "        </div>\n";
  oss << "    </div>\n";
  oss << "    \n";
  oss << "    <script>\n";
  oss << "        const elements = " << elements_json << ";\n";
  oss << "        const symptomDesc = \"" << symptom_desc << "\";\n";
  oss << "        document.getElementById('header-symptom').innerText = "
         "symptomDesc;\n";
  oss << "        \n";
  oss << "        // Intro Typing Simulation\n";
  oss << "        const consoleLines = [\n";
  oss << "            \"🚀 [SysPilot Engine] Initializing causal "
         "tracing...\",\n";
  oss << "            \"🔍 [Symptom Node] Analyzing target process: \" + "
         "symptomDesc,\n";
  oss << "            \"📡 [Telemetry] Constructing hybrid eBPF & procfs "
         "directed multigraph...\",\n";
  oss << "            \"📊 [Traversal] Tracing back active edge chains "
         "(Reverse BFS)...\",\n";
  oss << "            \"⚠️ [Resource Contention] Saturated write rate & locked "
         "resources detected!\",\n";
  oss << "            \"💡 [Root Cause] Diagnosed process propagation error: "
         "close-on-exec (O_CLOEXEC) missing.\",\n";
  oss << "            \"✅ [Report] Dashboard generated successfully. Ready to "
         "inspect.\"\n";
  oss << "        ];\n";
  oss << "        let currentLine = 0;\n";
  oss << "        function typeConsole() {\n";
  oss << "            if (currentLine < consoleLines.length) {\n";
  oss << "                const consoleBox = "
         "document.getElementById('intro-console');\n";
  oss << "                const div = document.createElement('div');\n";
  oss << "                div.className = 'console-line';\n";
  oss << "                div.style.animationDelay = `${currentLine * "
         "0.1}s`;\n";
  oss << "                div.innerHTML = `<span style=\"color: "
         "var(--cyan)\">&gt;</span> ${consoleLines[currentLine]}`;\n";
  oss << "                consoleBox.appendChild(div);\n";
  oss << "                consoleBox.scrollTop = consoleBox.scrollHeight;\n";
  oss << "                currentLine++;\n";
  oss << "                setTimeout(typeConsole, 600);\n";
  oss << "            }\n";
  oss << "        }\n";
  oss << "        function dismissIntro() {\n";
  oss << "            const intro = document.getElementById('intro-screen');\n";
  oss << "            intro.classList.add('hidden');\n";
  oss << "        }\n";
  oss << "        window.addEventListener('DOMContentLoaded', () => {\n";
  oss << "            typeConsole();\n";
  oss << "        });\n";
  oss << "        \n";
  oss << "        \n";
  oss << "        function showNodeDetails(data) {\n";
  oss << "            document.getElementById('inspect-title').innerText = "
         "data.name;\n";
  oss << "            document.getElementById('inspect-subtitle').innerText = "
         "`ID: ${data.id}`;\n";
  oss << "            \n";
  oss << "            let html = '';\n";
  oss << "            if (data.is_anomalous) {\n";
  oss << "                html += `\n";
  oss << "                    <div class=\"section-title\">Diagnostic "
         "Alert</div>\n";
  oss << "                    <div class=\"anomaly-alert\">\n";
  oss << "                        <strong>⚠️ Anomaly Detected</strong><br>\n";
  oss << "                        ${data.anomaly_reason || 'Process exhibiting "
         "anomalous resource or state metrics.'}\n";
  oss << "                    </div>\n";
  oss << "                `;\n";
  oss << "            }\n";
  oss << "            \n";
  oss << "            html += `<div class=\"section-title\">General "
         "Info</div>`;\n";
  oss << "            html += `<div class=\"metric-card\"><span "
         "class=\"metric-label\">Node Type</span><span "
         "class=\"metric-value\">${data.type.toUpperCase()}</span></div>`;\n";
  oss << "            \n";
  oss << "            if (data.type === 'process') {\n";
  oss << "                html += `\n";
  oss << "                    <div class=\"metric-card\"><span "
         "class=\"metric-label\">PID</span><span "
         "class=\"metric-value\">${data.pid}</span></div>\n";
  oss << "                    <div class=\"metric-card\"><span "
         "class=\"metric-label\">State</span><span "
         "class=\"metric-value\">${data.state}</span></div>\n";
  oss << "                    <div class=\"metric-card\"><span "
         "class=\"metric-label\">CPU Usage</span><span "
         "class=\"metric-value\">${data.cpu_usage_pct.toFixed(1)}%</span></"
         "div>\n";
  oss << "                `;\n";
  oss << "            }\n";
  oss << "            \n";
  oss << "            html += `\n";
  oss << "                <div class=\"section-title\">I/O Performance</div>\n";
  oss << "                <div class=\"metric-card\"><span "
         "class=\"metric-label\">Read Rate</span><span "
         "class=\"metric-value\">${data.read_rate_kb.toFixed(1)} "
         "KB/s</span></div>\n";
  oss << "                <div class=\"metric-card\"><span "
         "class=\"metric-label\">Write Rate</span><span "
         "class=\"metric-value\">${data.write_rate_kb.toFixed(1)} "
         "KB/s</span></div>\n";
  oss << "            `;\n";
  oss << "            \n";
  oss << "            document.getElementById('inspect-body').innerHTML = "
         "html;\n";
  oss << "        }\n";
  oss << "        \n";
  oss << "        let cy = cytoscape({\n";
  oss << "            container: document.getElementById('cy'),\n";
  oss << "            elements: elements,\n";
  oss << "            boxSelectionEnabled: false,\n";
  oss << "            autounselectify: true,\n";
  oss << "            style: [\n";
  oss << "                {\n";
  oss << "                    selector: 'node',\n";
  oss << "                    style: {\n";
  oss << "                        'label': 'data(name)',\n";
  oss << "                        'color': '#f8fafc',\n";
  oss << "                        'font-family': 'Outfit, sans-serif',\n";
  oss << "                        'font-size': '12px',\n";
  oss << "                        'text-valign': 'center',\n";
  oss << "                        'text-halign': 'center',\n";
  oss << "                        'background-color': '#0284c7',\n";
  oss << "                        'border-color': '#38bdf8',\n";
  oss << "                        'border-width': '2px',\n";
  oss << "                        'shape': 'round-rectangle',\n";
  oss << "                        'width': '180px',\n";
  oss << "                        'height': '45px',\n";
  oss << "                        'padding': '10px',\n";
  oss << "                        'transition-property': 'background-color, "
         "border-color, border-width',\n";
  oss << "                        'transition-duration': '0.2s'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'node[type=\"process\"]',\n";
  oss << "                    style: {\n";
  oss << "                        'background-color': '#0284c7',\n";
  oss << "                        'border-color': '#38bdf8'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'node[type=\"resource\"]',\n";
  oss << "                    style: {\n";
  oss << "                        'background-color': '#111827',\n";
  oss << "                        'border-color': '#475569',\n";
  oss << "                        'shape': 'ellipse',\n";
  oss << "                        'width': '50px',\n";
  oss << "                        'height': '50px'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'node[is_anomalous]',\n";
  oss << "                    style: {\n";
  oss << "                        'background-color': '#7f1d1d',\n";
  oss << "                        'border-color': '#ef4444',\n";
  oss << "                        'border-width': '3px'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'edge',\n";
  oss << "                    style: {\n";
  oss << "                        'width': 2,\n";
  oss << "                        'line-color': '#475569',\n";
  oss << "                        'target-arrow-color': '#475569',\n";
  oss << "                        'target-arrow-shape': 'triangle',\n";
  oss << "                        'curve-style': 'bezier',\n";
  oss << "                        'label': 'data(type)',\n";
  oss << "                        'font-size': '8px',\n";
  oss << "                        'color': '#94a3b8',\n";
  oss << "                        'text-rotation': 'autorotate',\n";
  oss << "                        'text-margin-y': '-10px',\n";
  oss << "                        'transition-property': 'line-color, width, "
         "target-arrow-color',\n";
  oss << "                        'transition-duration': '0.2s'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'edge[type=\"WRITES_TO\"]',\n";
  oss << "                    style: {\n";
  oss << "                        'line-color': '#f97316',\n";
  oss << "                        'target-arrow-color': '#f97316'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'edge[type=\"READS_FROM\"]',\n";
  oss << "                    style: {\n";
  oss << "                        'line-color': '#38bdf8',\n";
  oss << "                        'target-arrow-color': '#38bdf8'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'edge[type=\"BLOCKED_ON\"]',\n";
  oss << "                    style: {\n";
  oss << "                        'line-color': '#ef4444',\n";
  oss << "                        'target-arrow-color': '#ef4444',\n";
  oss << "                        'width': 3\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: 'edge[type=\"CONTENDS_WITH\"]',\n";
  oss << "                    style: {\n";
  oss << "                        'line-color': '#eab308',\n";
  oss << "                        'target-arrow-color': '#eab308',\n";
  oss << "                        'target-arrow-shape': 'none',\n";
  oss << "                        'width': 2,\n";
  oss << "                        'style': 'dashed'\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: '.highlighted',\n";
  oss << "                    style: {\n";
  oss << "                        'background-color': '#f97316',\n";
  oss << "                        'border-color': '#fdba74',\n";
  oss << "                        'line-color': '#f97316',\n";
  oss << "                        'target-arrow-color': '#f97316',\n";
  oss << "                        'width': 4,\n";
  oss << "                        'z-index': 100\n";
  oss << "                    }\n";
  oss << "                },\n";
  oss << "                {\n";
  oss << "                    selector: '.dimmed',\n";
  oss << "                    style: {\n";
  oss << "                        'opacity': 0.15,\n";
  oss << "                        'z-index': 0\n";
  oss << "                    }\n";
  oss << "                }\n";
  oss << "            ],\n";
  oss << "            layout: {\n";
  oss << "                name: 'dagre',\n";
  oss << "                nodeSep: 60,\n";
  oss << "                edgeSep: 40,\n";
  oss << "                rankSep: 80,\n";
  oss << "                rankDir: 'TB'\n";
  oss << "            }\n";
  oss << "        });\n";
  oss << "        \n";
  oss << "        cy.on('tap', 'node', function(evt){\n";
  oss << "            const node = evt.target;\n";
  oss << "            const data = node.data();\n";
  oss << "            showNodeDetails(data);\n";
  oss << "            highlightPath(node);\n";
  oss << "        });\n";
  oss << "        \n";
  oss << "        function highlightPath(node) {\n";
  oss << "            "
         "cy.elements().removeClass('highlighted').removeClass('dimmed');\n";
  oss << "            const path = "
         "node.successors().union(node.predecessors()).union(node);\n";
  oss << "            cy.elements().addClass('dimmed');\n";
  oss << "            path.removeClass('dimmed');\n";
  oss << "            node.addClass('highlighted');\n";
  oss << "            "
         "node.connectedEdges().removeClass('dimmed').addClass('highlighted');"
         "\n";
  oss << "        }\n";
  oss << "        \n";
  oss << "        function resetHighlight() {\n";
  oss << "            "
         "cy.elements().removeClass('highlighted').removeClass('dimmed');\n";
  oss << "            if "
         "(document.getElementById('d3-container').style.display === 'block') "
         "{\n";
  oss << "                d3.selectAll('.node rect').attr('opacity', 1.0);\n";
  oss << "                d3.selectAll('.node text').attr('opacity', 1.0);\n";
  oss << "                d3.selectAll('.links line').attr('opacity', 1.0);\n";
  oss << "            }\n";
  oss << "            document.getElementById('inspect-title').innerText = "
         "'Node Inspector';\n";
  oss << "            document.getElementById('inspect-subtitle').innerText = "
         "'Select a node to inspect parameters';\n";
  oss << "            document.getElementById('inspect-body').innerHTML = `\n";
  oss << "                <div class=\"empty-state\">\n";
  oss << "                    <div class=\"empty-icon\">🔍</div>\n";
  oss << "                    <div>Click on any process or resource node in "
         "the graph to view active system metrics and details.</div>\n";
  oss << "                </div>\n";
  oss << "            `;\n";
  oss << "        }\n";
  oss << "        \n";
  oss << "        let currentLayout = 'dagre';\n";
  oss << "        function toggleLayout() {\n";
  oss << "            currentLayout = (currentLayout === 'dagre') ? 'cose' : "
         "'dagre';\n";
  oss << "            const layout = cy.layout({\n";
  oss << "                name: currentLayout,\n";
  oss << "                animate: true,\n";
  oss << "                animationDuration: 500,\n";
  oss << "                nodeSep: 60,\n";
  oss << "                edgeSep: 40,\n";
  oss << "                rankSep: 80,\n";
  oss << "                rankDir: 'TB'\n";
  oss << "            });\n";
  oss << "            layout.run();\n";
  oss << "            document.getElementById('toggle-layout-btn').innerText = "
         "`Layout: ${currentLayout.toUpperCase()}`;\n";
  oss << "        }\n";
  oss << "        \n";
  oss << "        function initD3() {\n";
  oss << "            const container = "
         "document.getElementById('d3-container');\n";
  oss << "            container.innerHTML = '';\n";
  oss << "            const width = container.clientWidth || window.innerWidth "
         "- 380;\n";
  oss << "            const height = container.clientHeight || "
         "window.innerHeight - 80;\n";
  oss << "            \n";
  oss << "            const nodes = [];\n";
  oss << "            const links = [];\n";
  oss << "            \n";
  oss << "            elements.forEach(el => {\n";
  oss << "                if (el.data.source && el.data.target) {\n";
  oss << "                    links.push({\n";
  oss << "                        id: el.data.id,\n";
  oss << "                        source: el.data.source,\n";
  oss << "                        target: el.data.target,\n";
  oss << "                        type: el.data.type,\n";
  oss << "                        details: el.data.details || ''\n";
  oss << "                    });\n";
  oss << "                } else {\n";
  oss << "                    nodes.push({\n";
  oss << "                        id: el.data.id,\n";
  oss << "                        name: el.data.name,\n";
  oss << "                        type: el.data.type,\n";
  oss << "                        pid: el.data.pid,\n";
  oss << "                        state: el.data.state,\n";
  oss << "                        cpu_usage_pct: el.data.cpu_usage_pct || 0,\n";
  oss << "                        read_rate_kb: el.data.read_rate_kb || 0,\n";
  oss << "                        write_rate_kb: el.data.write_rate_kb || 0,\n";
  oss << "                        is_anomalous: el.data.is_anomalous || "
         "false,\n";
  oss << "                        anomaly_reason: el.data.anomaly_reason || "
         "''\n";
  oss << "                    });\n";
  oss << "                }\n";
  oss << "            });\n";
  oss << "            \n";
  oss << "            const svg = d3.select('#d3-container')\n";
  oss << "                .append('svg')\n";
  oss << "                .attr('width', '100%')\n";
  oss << "                .attr('height', '100%')\n";
  oss << "                .attr('viewBox', `0 0 ${width} ${height}`);\n";
  oss << "            \n";
  oss << "            const g = svg.append('g');\n";
  oss << "            const zoom = d3.zoom()\n";
  oss << "                .scaleExtent([0.1, 4])\n";
  oss << "                .on('zoom', (event) => {\n";
  oss << "                    g.attr('transform', event.transform);\n";
  oss << "                });\n";
  oss << "            svg.call(zoom);\n";
  oss << "            \n";
  oss << "            svg.append('defs').selectAll('marker')\n";
  oss << "                .data(['ACCESS', 'WRITES_TO', 'READS_FROM', "
         "'BLOCKED_ON', 'SPAWNED_BY', 'CONTENDS_WITH'])\n";
  oss << "                .enter().append('marker')\n";
  oss << "                .attr('id', d => `arrow-${d}`)\n";
  oss << "                .attr('viewBox', '0 -5 10 10')\n";
  oss << "                .attr('refX', 90)\n";
  oss << "                .attr('refY', 0)\n";
  oss << "                .attr('markerWidth', 6)\n";
  oss << "                .attr('markerHeight', 6)\n";
  oss << "                .attr('orient', 'auto')\n";
  oss << "                .append('path')\n";
  oss << "                .attr('fill', d => {\n";
  oss << "                    if (d === 'WRITES_TO') return '#f97316';\n";
  oss << "                    if (d === 'READS_FROM') return '#38bdf8';\n";
  oss << "                    if (d === 'BLOCKED_ON') return '#ef4444';\n";
  oss << "                    if (d === 'CONTENDS_WITH') return '#eab308';\n";
  oss << "                    return '#475569';\n";
  oss << "                })\n";
  oss << "                .attr('d', 'M0,-5L10,0L0,5');\n";
  oss << "            \n";
  oss << "            const simulation = d3.forceSimulation(nodes)\n";
  oss << "                .force('link', d3.forceLink(links).id(d => "
         "d.id).distance(180))\n";
  oss << "                .force('charge', "
         "d3.forceManyBody().strength(-500))\n";
  oss << "                .force('center', d3.forceCenter(width / 2, height / "
         "2))\n";
  oss << "                .force('collision', "
         "d3.forceCollide().radius(100));\n";
  oss << "            \n";
  oss << "            const link = g.append('g')\n";
  oss << "                .attr('class', 'links')\n";
  oss << "                .selectAll('line')\n";
  oss << "                .data(links)\n";
  oss << "                .enter().append('line')\n";
  oss << "                .attr('stroke-width', d => d.type === 'BLOCKED_ON' ? "
         "3 : 2)\n";
  oss << "                .attr('stroke', d => {\n";
  oss << "                    if (d.type === 'WRITES_TO') return '#f97316';\n";
  oss << "                    if (d.type === 'READS_FROM') return '#38bdf8';\n";
  oss << "                    if (d.type === 'BLOCKED_ON') return '#ef4444';\n";
  oss << "                    if (d.type === 'CONTENDS_WITH') return "
         "'#eab308';\n";
  oss << "                    return '#475569';\n";
  oss << "                })\n";
  oss << "                .attr('stroke-dasharray', d => d.type === "
         "'CONTENDS_WITH' ? '5,5' : 'none')\n";
  oss << "                .attr('marker-end', d => `url(#arrow-${d.type})`);\n";
  oss << "            \n";
  oss << "            const linkText = g.append('g')\n";
  oss << "                .attr('class', 'link-labels')\n";
  oss << "                .selectAll('text')\n";
  oss << "                .data(links)\n";
  oss << "                .enter().append('text')\n";
  oss << "                .attr('font-size', '8px')\n";
  oss << "                .attr('fill', '#94a3b8')\n";
  oss << "                .attr('text-anchor', 'middle')\n";
  oss << "                .text(d => d.type);\n";
  oss << "            \n";
  oss << "            const node = g.append('g')\n";
  oss << "                .attr('class', 'nodes')\n";
  oss << "                .selectAll('g')\n";
  oss << "                .data(nodes)\n";
  oss << "                .enter().append('g')\n";
  oss << "                .attr('class', 'node')\n";
  oss << "                .call(d3.drag()\n";
  oss << "                    .on('start', dragstarted)\n";
  oss << "                    .on('drag', dragged)\n";
  oss << "                    .on('end', dragended))\n";
  oss << "                .on('click', (event, d) => {\n";
  oss << "                    event.stopPropagation();\n";
  oss << "                    showNodeDetails(d);\n";
  oss << "                    highlightD3Path(d);\n";
  oss << "                });\n";
  oss << "            \n";
  oss << "            node.append('rect')\n";
  oss << "                .attr('width', 160)\n";
  oss << "                .attr('height', 40)\n";
  oss << "                .attr('x', -80)\n";
  oss << "                .attr('y', -20)\n";
  oss << "                .attr('rx', 6)\n";
  oss << "                .attr('ry', 6)\n";
  oss << "                .attr('fill', d => {\n";
  oss << "                    if (d.is_anomalous) return '#7f1d1d';\n";
  oss << "                    if (d.type === 'resource') return '#111827';\n";
  oss << "                    return '#0284c7';\n";
  oss << "                })\n";
  oss << "                .attr('stroke', d => {\n";
  oss << "                    if (d.is_anomalous) return '#ef4444';\n";
  oss << "                    if (d.type === 'resource') return '#475569';\n";
  oss << "                    return '#38bdf8';\n";
  oss << "                })\n";
  oss << "                .attr('stroke-width', d => d.is_anomalous ? 3 : "
         "2);\n";
  oss << "            \n";
  oss << "            node.append('text')\n";
  oss << "                .attr('dy', 4)\n";
  oss << "                .attr('text-anchor', 'middle')\n";
  oss << "                .attr('fill', '#f8fafc')\n";
  oss << "                .attr('font-size', '11px')\n";
  oss << "                .attr('font-family', 'Outfit, sans-serif')\n";
  oss << "                .text(d => {\n";
  oss << "                    if (d.name.length > 22) return "
         "d.name.substring(0, 19) + '...';\n";
  oss << "                    return d.name;\n";
  oss << "                });\n";
  oss << "            \n";
  oss << "            simulation.on('tick', () => {\n";
  oss << "                link\n";
  oss << "                    .attr('x1', d => d.source.x)\n";
  oss << "                    .attr('y1', d => d.source.y)\n";
  oss << "                    .attr('x2', d => d.target.x)\n";
  oss << "                    .attr('y2', d => d.target.y);\n";
  oss << "                \n";
  oss << "                linkText\n";
  oss << "                    .attr('x', d => (d.source.x + d.target.x) / 2)\n";
  oss << "                    .attr('y', d => (d.source.y + d.target.y) / 2 - "
         "5);\n";
  oss << "                \n";
  oss << "                node\n";
  oss << "                    .attr('transform', d => "
         "`translate(${d.x},${d.y})`);\n";
  oss << "            });\n";
  oss << "            \n";
  oss << "            function dragstarted(event, d) {\n";
  oss << "                if (!event.active) "
         "simulation.alphaTarget(0.3).restart();\n";
  oss << "                d.fx = d.x;\n";
  oss << "                d.fy = d.y;\n";
  oss << "            }\n";
  oss << "            \n";
  oss << "            function dragged(event, d) {\n";
  oss << "                d.fx = event.x;\n";
  oss << "                d.fy = event.y;\n";
  oss << "            }\n";
  oss << "            \n";
  oss << "            function dragended(event, d) {\n";
  oss << "                if (!event.active) simulation.alphaTarget(0);\n";
  oss << "                d.fx = null;\n";
  oss << "                d.fy = null;\n";
  oss << "            }\n";
  oss << "            \n";
  // Define highlightD3Path inside initD3 scope
  oss << "            function highlightD3Path(selectedNode) {\n";
  oss << "                const connectedNodeIds = new "
         "Set([selectedNode.id]);\n";
  oss << "                links.forEach(l => {\n";
  oss << "                    if (l.source.id === selectedNode.id) "
         "connectedNodeIds.add(l.target.id);\n";
  oss << "                    if (l.target.id === selectedNode.id) "
         "connectedNodeIds.add(l.source.id);\n";
  // Add highlighting logic inside Javascript string
  oss << "                });\n";
  oss << "                node.selectAll('rect').attr('opacity', d => "
         "connectedNodeIds.has(d.id) ? 1.0 : 0.2);\n";
  oss << "                node.selectAll('text').attr('opacity', d => "
         "connectedNodeIds.has(d.id) ? 1.0 : 0.2);\n";
  oss << "                link.attr('opacity', l => (l.source.id === "
         "selectedNode.id || l.target.id === selectedNode.id) ? 1.0 : 0.15);\n";
  oss << "            }\n";
  oss << "        }\n";
  oss << "        \n";
  oss << "        function switchView(view) {\n";
  oss << "            document.querySelectorAll('.view-btn').forEach(btn => "
         "btn.classList.remove('active'));\n";
  oss << "            if (view === 'cytoscape') {\n";
  oss << "                "
         "document.getElementById('btn-cytoscape').classList.add('active');\n";
  oss << "                document.getElementById('cy').style.display = "
         "'block';\n";
  oss << "                "
         "document.getElementById('d3-container').style.display = 'none';\n";
  oss << "                "
         "document.querySelector('.graph-controls').style.display = 'flex';\n";
  oss << "                cy.resize();\n";
  oss << "                cy.fit();\n";
  oss << "            } else {\n";
  oss << "                "
         "document.getElementById('btn-d3').classList.add('active');\n";
  oss << "                document.getElementById('cy').style.display = "
         "'none';\n";
  oss << "                "
         "document.getElementById('d3-container').style.display = 'block';\n";
  oss << "                "
         "document.querySelector('.graph-controls').style.display = 'none';\n";
  oss << "                initD3();\n";
  oss << "            }\n";
  oss << "        }\n";
  oss << "    </script>\n";
  oss << "</body>\n";
  oss << "</html>\n";

  return oss.str();
}
