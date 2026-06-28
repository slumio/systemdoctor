#ifndef CAUSAL_ENGINE_H
#define CAUSAL_ENGINE_H

// ─────────────────────────────────────────────────────────────────────────────
//  Causal Graph Engine
//
//  HPC upgrades:
//   · tsl::robin_map  — open-addressing hash map (3–5× better cache locality
//                       than std::unordered_map; powers node lookup)
//   · StringArena     — bump allocator; all string data in one contiguous slab
//   · std::string_view — zero-copy node/edge identifiers referencing arena
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

// tsl::robin_map — cache-friendly open-addressing table
// Significantly outperforms std::unordered_map on lookup-heavy workloads
#include "vendor/tsl/robin_map.h"
#include "vendor/tsl/robin_set.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Types
// ─────────────────────────────────────────────────────────────────────────────

enum class NodeType { PROCESS, RESOURCE };

enum class EdgeType {
    SPAWNED_BY,
    READS_FROM,
    WRITES_TO,
    BLOCKED_ON,
    CONTENDS_WITH
};

// ─────────────────────────────────────────────────────────────────────────────
//  StringArena — bump allocator (128 KiB default slab)
//  All strings stored here; GraphNode/GraphEdge hold string_views into slabs.
//  Eliminates per-string heap allocation during graph construction.
// ─────────────────────────────────────────────────────────────────────────────
class StringArena {
private:
    std::vector<std::vector<char>> chunks;
    size_t chunk_size;
    size_t current_chunk;
    size_t current_offset;

public:
    explicit StringArena(size_t slab = 256 * 1024)
        : chunk_size(slab), current_chunk(0), current_offset(0) {
        chunks.emplace_back(slab);
    }

    std::string_view alloc(std::string_view str) {
        if (str.empty()) return {};
        size_t len = str.size();
        if (current_offset + len > chunk_size) {
            size_t next = std::max(chunk_size, len);
            chunks.emplace_back(next);
            current_chunk = chunks.size() - 1;
            current_offset = 0;
        }
        char* dest = &chunks[current_chunk][current_offset];
        std::memcpy(dest, str.data(), len);
        current_offset += len;
        return {dest, len};
    }

    void reset() { current_chunk = 0; current_offset = 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Graph primitives
// ─────────────────────────────────────────────────────────────────────────────

struct GraphNode {
    std::string_view id;           // "pid:1234" or "resource:/path"
    NodeType         type;
    std::string_view name;
    pid_t            pid          = 0;
    std::string_view state        = "";
    uint64_t         read_bytes   = 0;
    uint64_t         write_bytes  = 0;
    double           read_rate_kb = 0.0;
    double           write_rate_kb= 0.0;
    double           cpu_usage_pct= 0.0;
    bool             is_anomalous = false;
    std::string_view anomaly_reason = "";
};

struct GraphEdge {
    std::string_view from_id;
    std::string_view to_id;
    EdgeType         type;
    std::string_view details;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CausalGraph
// ─────────────────────────────────────────────────────────────────────────────

class CausalGraph {
public:
    StringArena arena;

    // tsl::robin_map: open-addressing with robin-hood probing
    // ~3–5× faster lookup than std::unordered_map on dense integer/string keys
    tsl::robin_map<std::string_view, GraphNode> nodes;
    std::vector<GraphEdge>                       edges;

    void build_graph(int interval_seconds = 2, bool use_ebpf = false,
                     pid_t target_pid = 0);

    std::vector<std::string> trace_root_cause(const std::string& start_node_id);

    std::string serialize_chain_to_json(const std::vector<std::string>& path_nodes);

    std::string export_graph_to_dot    (const std::vector<std::string>& path_nodes);
    std::string export_graph_to_mermaid(const std::vector<std::string>& path_nodes);
    std::string export_graph_to_html   (const std::vector<std::string>& path_nodes);

private:
    void add_node(const GraphNode& node);
    void add_edge(std::string_view from, std::string_view to,
                  EdgeType type, std::string_view details = "");

    tsl::robin_map<pid_t, GraphNode> take_proc_snapshot();
};

#endif // CAUSAL_ENGINE_H
