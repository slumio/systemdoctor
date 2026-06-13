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

void CausalGraph::build_graph(int interval_seconds) {
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
