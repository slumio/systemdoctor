#include "causal_engine.h"
#include "telemetry.h"
#include "utils.h"
#include "nlohmann/json.hpp"
#include <iostream>
#include <vector>
#include <queue>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <unistd.h>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;
using json = nlohmann::json;

void CausalGraph::add_node(const GraphNode& node) {
    nodes[node.id] = node;
}

void CausalGraph::add_edge(const std::string& from, const std::string& to, EdgeType type, const std::string& details) {
    GraphEdge edge;
    edge.from_id = from;
    edge.to_id = to;
    edge.type = type;
    edge.details = details;
    edges.push_back(edge);
}

std::unordered_map<pid_t, GraphNode> CausalGraph::take_proc_snapshot() {
    std::unordered_map<pid_t, GraphNode> snapshot;
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
                    pid_t pid = std::stoi(pid_str);
                    ProcessTelemetry pt = telemetry::collect_process_telemetry(pid);
                    if (pt.pid != 0) {
                        GraphNode node;
                        node.id = "pid:" + std::to_string(pid);
                        node.type = NodeType::PROCESS;
                        node.name = pt.name;
                        node.pid = pid;
                        node.state = pt.state;
                        node.read_bytes = pt.read_bytes;
                        node.write_bytes = pt.write_bytes;
                        // Store cpu ticks in cpu_usage_pct temporarily for delta calculation
                        node.cpu_usage_pct = (double)(pt.utime + pt.stime);
                        snapshot[pid] = node;
                    }
                }
            }
        }
    } catch (...) {}
    return snapshot;
}

void CausalGraph::build_graph(int interval_seconds, bool use_ebpf, pid_t target_pid) {
    // Check and initialize eBPF tracing if requested
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
            
            std::string filter = "pid == " + std::to_string(target_pid) + " || ppid == " + std::to_string(target_pid);
            
            std::string script = 
                "tracepoint:syscalls:sys_enter_open,tracepoint:syscalls:sys_enter_openat /" + filter + "/ { "
                "  printf(\"OPEN | %d | %s | %s\\n\", pid, comm, str(args->filename)); "
                "} "
                "tracepoint:syscalls:sys_enter_connect /" + filter + "/ { "
                "  printf(\"CONNECT | %d | %s | family: %d\\n\", pid, comm, args->uservaddr->sa_family); "
                "} "
                "tracepoint:syscalls:sys_enter_execve /" + filter + "/ { "
                "  printf(\"EXEC | %d | %s | %s\\n\", pid, comm, str(args->filename)); "
                "}";
                
            std::string cmd = "timeout " + std::to_string(interval_seconds) + " " + bpftrace_cmd + " -e '" + script + "' > " + bpftrace_log + " 2>/dev/null &";
            utils::run_command_output(cmd);
            ebpf_running = true;
            std::cout << "🚀 [eBPF] Active tracing enabled for PID " << target_pid << "..." << std::endl;
        } else {
            std::cout << "⚠️  [eBPF] Insufficient privileges. Falling back to standard procfs polling..." << std::endl;
        }
    }

    // 1. Take Snapshot 1
    std::unordered_map<pid_t, GraphNode> snap1 = take_proc_snapshot();
    std::unordered_map<std::string, uint64_t> disk1 = telemetry::get_disk_stats();
    
    // Sleep for the interval
    std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
    
    // 2. Take Snapshot 2
    std::unordered_map<pid_t, GraphNode> snap2 = take_proc_snapshot();
    std::unordered_map<std::string, uint64_t> disk2 = telemetry::get_disk_stats();
    
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    
    // 3. Populate process nodes & calculate rates
    for (const auto& pair2 : snap2) {
        pid_t pid = pair2.first;
        GraphNode node = pair2.second;
        
        auto it1 = snap1.find(pid);
        if (it1 != snap1.end()) {
            GraphNode prev_node = it1->second;
            double cpu_delta = node.cpu_usage_pct - prev_node.cpu_usage_pct;
            node.cpu_usage_pct = (cpu_delta / (double)(clk_tck * interval_seconds)) * 100.0;
            
            double r_delta = (node.read_bytes >= prev_node.read_bytes) ? (node.read_bytes - prev_node.read_bytes) : 0;
            double w_delta = (node.write_bytes >= prev_node.write_bytes) ? (node.write_bytes - prev_node.write_bytes) : 0;
            
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
            node.anomaly_reason = "Process is in uninterruptible sleep (D-state), likely blocked on disk I/O";
        } else if (node.cpu_usage_pct > 80.0) {
            node.is_anomalous = true;
            node.anomaly_reason = "High CPU utilization (" + std::to_string((int)node.cpu_usage_pct) + "%)";
        } else if (node.write_rate_kb > 5000.0) { // > 5 MB/s
            node.is_anomalous = true;
            node.anomaly_reason = "High disk write rate (" + std::to_string((int)node.write_rate_kb) + " KB/s)";
        } else if (node.read_rate_kb > 5000.0) {
            node.is_anomalous = true;
            node.anomaly_reason = "High disk read rate (" + std::to_string((int)node.read_rate_kb) + " KB/s)";
        }
        
        add_node(node);
    }
    
    // 4. Calculate device anomalies from disk stats
    for (const auto& pair2 : disk2) {
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
            double io_util_pct = (io_delta / (double)(interval_seconds * 1000.0)) * 100.0;
            
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
                double r_bytes_delta = (r_bytes_it2->second >= r_bytes_it1->second) ? (r_bytes_it2->second - r_bytes_it1->second) : 0;
                dev_node.read_rate_kb = r_bytes_delta / (1024.0 * interval_seconds);
            }
            if (w_bytes_it2 != disk2.end() && w_bytes_it1 != disk1.end()) {
                double w_bytes_delta = (w_bytes_it2->second >= w_bytes_it1->second) ? (w_bytes_it2->second - w_bytes_it1->second) : 0;
                dev_node.write_rate_kb = w_bytes_delta / (1024.0 * interval_seconds);
            }
            
            if (io_util_pct > 80.0) {
                dev_node.is_anomalous = true;
                dev_node.anomaly_reason = "Device utilization is high (" + std::to_string((int)io_util_pct) + "% time active)";
            }
            add_node(dev_node);
        }
    }
    // 5. Connect Parent-Child (SPAWNED_BY) and Map FDs to Resources
    std::unordered_set<std::string> active_files;
    
    // Pass 1: Identify active files (files in tmp, var/lib, home that are currently being read or written to)
    for (const auto& pair : nodes) {
        if (pair.second.type != NodeType::PROCESS) continue;
        if (pair.second.read_rate_kb > 0.1 || pair.second.write_rate_kb > 0.1) {
            std::vector<std::pair<std::string, std::string>> open_res = telemetry::get_open_resources(pair.second.pid);
            for (const auto& res : open_res) {
                std::string path = res.second;
                if (path.find("/var/lib/") != std::string::npos || path.find("/tmp/") != std::string::npos || path.find("/home/") != std::string::npos) {
                    active_files.insert(path);
                }
            }
        }
    }
    
    // Pass 2: Connect processes to active resources and block devices
    std::vector<GraphNode> new_resource_nodes;
    for (const auto& pair : nodes) {
        if (pair.second.type != NodeType::PROCESS) continue;
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
        std::vector<std::pair<std::string, std::string>> open_res = telemetry::get_open_resources(pid);
        std::unordered_set<std::string> mapped_resources;
        
        for (const auto& res : open_res) {
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
            
            if (!resource_id.empty() && mapped_resources.find(resource_id) == mapped_resources.end()) {
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
                    add_edge(pair.second.id, resource_id, EdgeType::WRITES_TO, "FD: " + res.first);
                } else {
                    add_edge(pair.second.id, resource_id, EdgeType::READS_FROM, "FD: " + res.first);
                }
                
                // D-state processes are BLOCKED_ON the resources they have open
                if (pair.second.state == "D") {
                    add_edge(pair.second.id, resource_id, EdgeType::BLOCKED_ON, "Process blocked in I/O wait on resource");
                }
            }
        }
        
        // If process is in D-state but no block device was mapped, link it to the primary disk device
        if (pair.second.state == "D") {
            bool has_blocked_edge = false;
            for (const auto& edge : edges) {
                if (edge.from_id == pair.second.id && edge.type == EdgeType::BLOCKED_ON) {
                    has_blocked_edge = true;
                    break;
                }
            }
            if (!has_blocked_edge) {
                // Find most active disk
                std::string target_disk = "resource:/dev/sda";
                double max_util = 0;
                for (const auto& n : nodes) {
                    if (n.second.type == NodeType::RESOURCE && utils::starts_with(n.first, "resource:/dev/")) {
                        double total_io = n.second.read_rate_kb + n.second.write_rate_kb;
                        if (total_io > max_util) {
                            max_util = total_io;
                            target_disk = n.first;
                        }
                    }
                }
                add_edge(pair.second.id, target_disk, EdgeType::BLOCKED_ON, "Process blocked on primary disk device");
            }
        }
    }
    
    for (const auto& rnode : new_resource_nodes) {
        if (nodes.find(rnode.id) == nodes.end()) {
            add_node(rnode);
        }
    }
    
    // 6. Contention Detection
    // Group processes accessing the same resource
    std::unordered_map<std::string, std::vector<std::string>> resource_accessors;
    for (const auto& edge : edges) {
        if (edge.type == EdgeType::READS_FROM || edge.type == EdgeType::WRITES_TO || edge.type == EdgeType::BLOCKED_ON) {
            resource_accessors[edge.to_id].push_back(edge.from_id);
        }
    }
    
    for (const auto& pair : resource_accessors) {
        const std::string& res_id = pair.first;
        const std::vector<std::string>& accessors = pair.second;
        
        if (accessors.size() > 1) {
            // Check if multiple accessors have high write/read rates
            std::vector<std::string> active_writers;
            for (const auto& p_id : accessors) {
                auto it = nodes.find(p_id);
                if (it != nodes.end() && (it->second.write_rate_kb > 500.0 || it->second.read_rate_kb > 500.0)) {
                    active_writers.push_back(p_id);
                }
            }
            
            if (active_writers.size() > 1) {
                // Add CONTENDS_WITH edges between active writers/readers
                for (size_t i = 0; i < active_writers.size(); ++i) {
                    for (size_t j = i + 1; j < active_writers.size(); ++j) {
                        add_edge(active_writers[i], active_writers[j], EdgeType::CONTENDS_WITH, "Shared access to " + res_id);
                        add_edge(active_writers[j], active_writers[i], EdgeType::CONTENDS_WITH, "Shared access to " + res_id);
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
                if (line.empty()) continue;
                
                std::vector<std::string> parts = utils::split(line, " | ");
                if (parts.size() >= 4) {
                    std::string event_type = utils::trim(parts[0]);
                    pid_t event_pid = 0;
                    try {
                        event_pid = std::stoi(utils::trim(parts[1]));
                    } catch (...) { continue; }
                    
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
                        add_edge(pnode_id, res_id, EdgeType::READS_FROM, "eBPF trace: opened file");
                    } 
                    else if (event_type == "CONNECT") {
                        std::string res_id = "resource:socket_" + details;
                        if (nodes.find(res_id) == nodes.end()) {
                            GraphNode rnode;
                            rnode.id = res_id;
                            rnode.name = "Socket (" + details + ")";
                            rnode.type = NodeType::RESOURCE;
                            add_node(rnode);
                        }
                        add_edge(pnode_id, res_id, EdgeType::READS_FROM, "eBPF trace: network connection");
                    }
                    else if (event_type == "EXEC") {
                        std::string res_id = "resource:" + details;
                        if (nodes.find(res_id) == nodes.end()) {
                            GraphNode rnode;
                            rnode.id = res_id;
                            rnode.name = details;
                            rnode.type = NodeType::RESOURCE;
                            add_node(rnode);
                        }
                        add_edge(pnode_id, res_id, EdgeType::READS_FROM, "eBPF trace: executed binary");
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

std::vector<std::string> CausalGraph::trace_root_cause(const std::string& start_node_id) {
    std::vector<std::string> path;
    if (nodes.find(start_node_id) == nodes.end()) return path;
    
    std::queue<std::string> q;
    std::unordered_set<std::string> visited;
    
    q.push(start_node_id);
    visited.insert(start_node_id);
    
    while (!q.empty()) {
        std::string current = q.front();
        q.pop();
        path.push_back(current);
        
        // Find neighbors to traverse backwards.
        // What constitutes a backward causal relationship?
        // 1. If A is BLOCKED_ON B (B is causing A's blockage)
        // 2. If A CONTENDS_WITH B (B is contending with A)
        // 3. If A was SPAWNED_BY B (B is parent process of A)
        // 4. If resource R is written to/read by process B (B is saturating resource R)
        
        auto curr_node = nodes[current];
        if (curr_node.type == NodeType::PROCESS) {
            // Find edges going out of current process representing blocks, parents, or contentions
            for (const auto& edge : edges) {
                if (edge.from_id == current) {
                    if (edge.type == EdgeType::BLOCKED_ON || edge.type == EdgeType::SPAWNED_BY || edge.type == EdgeType::CONTENDS_WITH) {
                        if (visited.find(edge.to_id) == visited.end()) {
                            visited.insert(edge.to_id);
                            q.push(edge.to_id);
                        }
                    }
                    
                    // Also traverse to open files if they are anomalous or accessed by another anomalous process
                    if (edge.type == EdgeType::READS_FROM || edge.type == EdgeType::WRITES_TO) {
                        std::string res_id = edge.to_id;
                        auto res_it = nodes.find(res_id);
                        if (res_it != nodes.end()) {
                            bool should_traverse = res_it->second.is_anomalous;
                            if (!should_traverse) {
                                // Check if another process is accessing this resource anomalously
                                for (const auto& other_edge : edges) {
                                    if (other_edge.to_id == res_id && other_edge.from_id != current) {
                                        auto proc_it = nodes.find(other_edge.from_id);
                                        if (proc_it != nodes.end() && proc_it->second.is_anomalous) {
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
            // Find processes that have WRITES_TO or READS_FROM edges pointing to this resource.
            // Prioritize processes with high write/read rates.
            std::vector<std::pair<std::string, double>> active_writers;
            for (const auto& edge : edges) {
                if (edge.to_id == current && (edge.type == EdgeType::WRITES_TO || edge.type == EdgeType::READS_FROM)) {
                    auto proc_it = nodes.find(edge.from_id);
                    if (proc_it != nodes.end()) {
                        double total_io = proc_it->second.write_rate_kb + proc_it->second.read_rate_kb;
                        active_writers.push_back({edge.from_id, total_io});
                    }
                }
            }
            
            // Sort by active IO rate desc
            std::sort(active_writers.begin(), active_writers.end(), [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            
            // Add top 3 active processes to queue
            for (size_t i = 0; i < std::min(active_writers.size(), (size_t)3); ++i) {
                std::string p_id = active_writers[i].first;
                if (visited.find(p_id) == visited.end()) {
                    visited.insert(p_id);
                    q.push(p_id);
                }
            }
        }
    }
    
    return path;
}

std::string CausalGraph::serialize_chain_to_json(const std::vector<std::string>& path_nodes) {
    json j;
    j["symptom_node"] = path_nodes.empty() ? "" : path_nodes.front();
    
    json j_nodes = json::array();
    std::unordered_set<std::string> path_set(path_nodes.begin(), path_nodes.end());
    
    for (const auto& node_id : path_nodes) {
        auto it = nodes.find(node_id);
        if (it != nodes.end()) {
            json jn;
            jn["id"] = it->second.id;
            jn["type"] = (it->second.type == NodeType::PROCESS) ? "process" : "resource";
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
    for (const auto& edge : edges) {
        if (path_set.find(edge.from_id) != path_set.end() && path_set.find(edge.to_id) != path_set.end()) {
            json je;
            je["from"] = edge.from_id;
            je["to"] = edge.to_id;
            
            std::string type_str = "ACCESS";
            switch (edge.type) {
                case EdgeType::SPAWNED_BY: type_str = "SPAWNED_BY"; break;
                case EdgeType::READS_FROM: type_str = "READS_FROM"; break;
                case EdgeType::WRITES_TO: type_str = "WRITES_TO"; break;
                case EdgeType::BLOCKED_ON: type_str = "BLOCKED_ON"; break;
                case EdgeType::CONTENDS_WITH: type_str = "CONTENDS_WITH"; break;
            }
            je["type"] = type_str;
            je["details"] = edge.details;
            j_edges.push_back(je);
        }
    }
    j["edges"] = j_edges;
    
    return j.dump(4);
}

std::string CausalGraph::export_graph_to_dot(const std::vector<std::string>& path_nodes) {
    std::unordered_set<std::string> path_set(path_nodes.begin(), path_nodes.end());
    std::ostringstream oss;
    oss << "digraph CausalTrace {\n";
    oss << "    // Graph styling\n";
    oss << "    backgroundcolor=\"#0f172a\";\n";
    oss << "    node [style=\"filled,rounded\", shape=box, fontname=\"Helvetica,Arial,sans-serif\", fontsize=10, penwidth=1.5];\n";
    oss << "    edge [fontname=\"Helvetica,Arial,sans-serif\", fontsize=8, color=\"#94a3b8\", fontcolor=\"#cbd5e1\", penwidth=1.5];\n\n";

    // Write nodes
    for (const auto& node_id : path_nodes) {
        auto it = nodes.find(node_id);
        if (it != nodes.end()) {
            std::string label = it->second.name;
            if (it->second.type == NodeType::PROCESS) {
                label += " (PID: " + std::to_string(it->second.pid) + ")";
                if (it->second.read_rate_kb > 0.1 || it->second.write_rate_kb > 0.1) {
                    label += "\\nI/O: R " + std::to_string((int)it->second.read_rate_kb) + "KB/s, W " + std::to_string((int)it->second.write_rate_kb) + "KB/s";
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
                << "\", fillcolor=\"" << fillColor 
                << "\", color=\"" << strokeColor 
                << "\", fontcolor=\"" << fontColor << "\"];\n";
        }
    }
    oss << "\n";

    // Write edges
    for (const auto& edge : edges) {
        if (path_set.find(edge.from_id) != path_set.end() && path_set.find(edge.to_id) != path_set.end()) {
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
                label += " (" + edge.details + ")";
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

std::string CausalGraph::export_graph_to_mermaid(const std::vector<std::string>& path_nodes) {
    std::unordered_set<std::string> path_set(path_nodes.begin(), path_nodes.end());
    std::ostringstream oss;
    oss << "graph TD\n";
    oss << "    classDef process fill:#0284c7,stroke:#38bdf8,color:#fff,stroke-width:2px;\n";
    oss << "    classDef resource fill:#1e293b,stroke:#475569,color:#cbd5e1,stroke-width:2px;\n";
    oss << "    classDef anomalous fill:#7f1d1d,stroke:#ef4444,color:#fff,stroke-width:2px;\n\n";

    for (const auto& node_id : path_nodes) {
        auto it = nodes.find(node_id);
        if (it != nodes.end()) {
            std::string label = it->second.name;
            if (it->second.type == NodeType::PROCESS) {
                label += " (PID: " + std::to_string(it->second.pid) + ")";
                if (it->second.read_rate_kb > 0.1 || it->second.write_rate_kb > 0.1) {
                    label += " [IO: R " + std::to_string((int)it->second.read_rate_kb) + "KB/s, W " + std::to_string((int)it->second.write_rate_kb) + "KB/s]";
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

    for (const auto& edge : edges) {
        if (path_set.find(edge.from_id) != path_set.end() && path_set.find(edge.to_id) != path_set.end()) {
            std::string type_str = "ACCESS";
            switch (edge.type) {
                case EdgeType::SPAWNED_BY: type_str = "SPAWNED_BY"; break;
                case EdgeType::READS_FROM: type_str = "READS_FROM"; break;
                case EdgeType::WRITES_TO: type_str = "WRITES_TO"; break;
                case EdgeType::BLOCKED_ON: type_str = "BLOCKED_ON"; break;
                case EdgeType::CONTENDS_WITH: type_str = "CONTENDS_WITH"; break;
            }
            std::string label = type_str;
            if (!edge.details.empty()) {
                label += " (" + edge.details + ")";
            }
            
            size_t pos = 0;
            while ((pos = label.find('"', pos)) != std::string::npos) {
                label.replace(pos, 1, "'");
                pos++;
            }

            std::string safe_from = edge.from_id;
            std::replace(safe_from.begin(), safe_from.end(), ':', '_');
            std::replace(safe_from.begin(), safe_from.end(), '/', '_');
            std::replace(safe_from.begin(), safe_from.end(), '.', '_');
            std::replace(safe_from.begin(), safe_from.end(), '[', '_');
            std::replace(safe_from.begin(), safe_from.end(), ']', '_');
            std::replace(safe_from.begin(), safe_from.end(), ' ', '_');

            std::string safe_to = edge.to_id;
            std::replace(safe_to.begin(), safe_to.end(), ':', '_');
            std::replace(safe_to.begin(), safe_to.end(), '/', '_');
            std::replace(safe_to.begin(), safe_to.end(), '.', '_');
            std::replace(safe_to.begin(), safe_to.end(), '[', '_');
            std::replace(safe_to.begin(), safe_to.end(), ']', '_');
            std::replace(safe_to.begin(), safe_to.end(), ' ', '_');

            oss << "    " << safe_from << " -->|\"" << label << "\"| " << safe_to << "\n";
        }
    }

    return oss.str();
}

std::string CausalGraph::export_graph_to_html(const std::vector<std::string>& path_nodes) {
    std::string mermaid_code = export_graph_to_mermaid(path_nodes);
    
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
    oss << "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    oss << "    <title>SysPilot CausalTrace Graph</title>\n";
    oss << "    <script type=\"module\">\n";
    oss << "        import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@10/dist/mermaid.esm.min.mjs';\n";
    oss << "        mermaid.initialize({\n";
    oss << "            startOnLoad: true,\n";
    oss << "            theme: 'dark',\n";
    oss << "            securityLevel: 'loose'\n";
    oss << "        });\n";
    oss << "    </script>\n";
    oss << "    <style>\n";
    oss << "        body {\n";
    oss << "            font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, \"Helvetica Neue\", Arial, sans-serif;\n";
    oss << "            margin: 0;\n";
    oss << "            padding: 2rem;\n";
    oss << "            background: #0b0f19;\n";
    oss << "            color: #f1f5f9;\n";
    oss << "        }\n";
    oss << "        .container {\n";
    oss << "            max-width: 1000px;\n";
    oss << "            margin: 0 auto;\n";
    oss << "        }\n";
    oss << "        header {\n";
    oss << "            margin-bottom: 2rem;\n";
    oss << "            border-bottom: 1px solid #1e293b;\n";
    oss << "            padding-bottom: 1rem;\n";
    oss << "        }\n";
    oss << "        h1 {\n";
    oss << "            font-size: 1.75rem;\n";
    oss << "            font-weight: 700;\n";
    oss << "            color: #38bdf8;\n";
    oss << "            margin: 0 0 0.5rem 0;\n";
    oss << "        }\n";
    oss << "        .subtitle {\n";
    oss << "            color: #94a3b8;\n";
    oss << "            font-size: 0.95rem;\n";
    oss << "            margin: 0;\n";
    oss << "        }\n";
    oss << "        .symptom-tag {\n";
    oss << "            background: #0369a1;\n";
    oss << "            color: #e0f2fe;\n";
    oss << "            padding: 0.15rem 0.5rem;\n";
    oss << "            border-radius: 0.25rem;\n";
    oss << "            font-weight: 600;\n";
    oss << "            font-family: monospace;\n";
    oss << "        }\n";
    oss << "        .graph-wrapper {\n";
    oss << "            background: #111827;\n";
    oss << "            border: 1px solid #1f2937;\n";
    oss << "            border-radius: 0.75rem;\n";
    oss << "            padding: 2rem;\n";
    oss << "            box-shadow: 0 10px 15px -3px rgba(0, 0, 0, 0.3);\n";
    oss << "            overflow-x: auto;\n";
    oss << "            display: flex;\n";
    oss << "            justify-content: center;\n";
    oss << "        }\n";
    oss << "        footer {\n";
    oss << "            margin-top: 2rem;\n";
    oss << "            text-align: center;\n";
    oss << "            font-size: 0.8rem;\n";
    oss << "            color: #4b5563;\n";
    oss << "        }\n";
    oss << "    </style>\n";
    oss << "</head>\n";
    oss << "<body>\n";
    oss << "    <div class=\"container\">\n";
    oss << "        <header>\n";
    oss << "            <h1>🌐 SysPilot CausalTrace</h1>\n";
    oss << "            <p class=\"subtitle\">Causal Dependency Diagnostics for Symptom: <span class=\"symptom-tag\">" << symptom_desc << "</span></p>\n";
    oss << "        </header>\n";
    oss << "        <main class=\"graph-wrapper\">\n";
    oss << "            <pre class=\"mermaid\">\n";
    oss << mermaid_code << "\n";
    oss << "            </pre>\n";
    oss << "        </main>\n";
    oss << "        <footer>\n";
    oss << "            Generated automatically by SysPilot &bull; Kernel-Level Causal Observability Engine\n";
    oss << "        </footer>\n";
    oss << "    </div>\n";
    oss << "</body>\n";
    oss << "</html>\n";
    return oss.str();
}
