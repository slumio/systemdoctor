#ifndef CODEBASE_H
#define CODEBASE_H

#include "config.h"
#include <string>
#include <vector>
#include <cstdint>

struct DbChunk {
    std::string file_path = "";
    std::string content = "";
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    std::vector<float> embedding;
};

struct FileRegistry {
    std::string file_path = "";
    uint64_t last_modified = 0;
    uint64_t size = 0;
};

struct VectorDb {
    std::string workspace_path = "";
    std::vector<FileRegistry> files;
    std::vector<DbChunk> chunks;
    
    bool load_from_binary(const std::string& path);
    bool save_to_binary(const std::string& path);
};

namespace codebase {

// Vector operations
void normalize_vector(std::vector<float>& vec);
float cosine_similarity(const std::vector<float>& v1, const std::vector<float>& v2);

// Chunker
struct RawChunk {
    std::string content;
    uint32_t start_line;
    uint32_t end_line;
};
std::vector<RawChunk> chunk_file(const std::string& path, const std::string& strategy);

// Indexing & Querying
std::string get_db_path(const std::string& workspace_path);
bool update_index(const std::string& workspace_path, const Config& config, bool force = false);
std::string query_context(const std::string& workspace_path, const std::string& query, const Config& config);

} // namespace codebase

#endif // CODEBASE_H
