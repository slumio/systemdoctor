#ifndef CAUSAL_ENGINE_H
#define CAUSAL_ENGINE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <sys/types.h>

enum class NodeType {
    PROCESS,
    RESOURCE
};

enum class EdgeType {
    SPAWNED_BY,
    READS_FROM,
    WRITES_TO,
    BLOCKED_ON,
    CONTENDS_WITH
};

struct GraphNode {
    std::string id; // For PROCESS: "pid:1234", For RESOURCE: "path:/dev/nvme0n1" or "socket:5678"
    NodeType type;
    std::string name;
    pid_t pid = 0;              // 0 if resource
    std::string state = "";     // "R", "S", "D", etc. (only process)
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    double read_rate_kb = 0.0;
    double write_rate_kb = 0.0;
    double cpu_usage_pct = 0.0;
    bool is_anomalous = false;
    std::string anomaly_reason = "";
};

struct GraphEdge {
    std::string from_id;
    std::string to_id;
    EdgeType type;
    std::string details;
};

class CausalGraph {
public:
    std::unordered_map<std::string, GraphNode> nodes;
    std::vector<GraphEdge> edges;

    // Populates graph based on two proc snapshots separated by interval
    void build_graph(int interval_seconds = 2);

    // Runs reverse BFS from symptomatic node and returns a list of node IDs on the path
    std::vector<std::string> trace_root_cause(const std::string& start_node_id);

    // Serializes the causal graph segment (nodes and edges along path) to JSON string
    std::string serialize_chain_to_json(const std::vector<std::string>& path_nodes);

private:
    void add_node(const GraphNode& node);
    void add_edge(const std::string& from, const std::string& to, EdgeType type, const std::string& details = "");
    
    // Internal parser and mapper helpers
    std::unordered_map<pid_t, GraphNode> take_proc_snapshot();
};

#endif // CAUSAL_ENGINE_H
