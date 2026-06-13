#include "codebase.h"
#include "utils.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_set>

using json = nlohmann::json;

// Binary file serialization helpers
static void write_string(std::ofstream& out, const std::string& str) {
    uint32_t len = str.length();
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write(str.data(), len);
}

static std::string read_string(std::ifstream& in) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in || len > 10 * 1024 * 1024) { // Safety limit 10MB
        return "";
    }
    std::string str(len, '\0');
    in.read(&str[0], len);
    return str;
}

bool VectorDb::load_from_binary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    
    char magic[14];
    in.read(magic, 14);
    if (std::string(magic, 14) != "SYSPILOT_VDB_2") {
        return false;
    }
    
    workspace_path = read_string(in);
    
    uint32_t files_count = 0;
    in.read(reinterpret_cast<char*>(&files_count), sizeof(files_count));
    files.resize(files_count);
    for (uint32_t i = 0; i < files_count; ++i) {
        files[i].file_path = read_string(in);
        in.read(reinterpret_cast<char*>(&files[i].last_modified), sizeof(files[i].last_modified));
        in.read(reinterpret_cast<char*>(&files[i].size), sizeof(files[i].size));
    }
    
    uint32_t chunks_count = 0;
    in.read(reinterpret_cast<char*>(&chunks_count), sizeof(chunks_count));
    chunks.resize(chunks_count);
    for (uint32_t i = 0; i < chunks_count; ++i) {
        chunks[i].file_path = read_string(in);
        chunks[i].content = read_string(in);
        in.read(reinterpret_cast<char*>(&chunks[i].start_line), sizeof(chunks[i].start_line));
        in.read(reinterpret_cast<char*>(&chunks[i].end_line), sizeof(chunks[i].end_line));
        
        uint32_t embed_len = 0;
        in.read(reinterpret_cast<char*>(&embed_len), sizeof(embed_len));
        if (embed_len > 8192) return false; // Safety check
        
        chunks[i].embedding.resize(embed_len);
        in.read(reinterpret_cast<char*>(chunks[i].embedding.data()), embed_len * sizeof(float));
    }
    
    return true;
}

bool VectorDb::save_to_binary(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    
    out.write("SYSPILOT_VDB_2", 14);
    write_string(out, workspace_path);
    
    uint32_t files_count = files.size();
    out.write(reinterpret_cast<const char*>(&files_count), sizeof(files_count));
    for (const auto& file : files) {
        write_string(out, file.file_path);
        out.write(reinterpret_cast<const char*>(&file.last_modified), sizeof(file.last_modified));
        out.write(reinterpret_cast<const char*>(&file.size), sizeof(file.size));
    }
    
    uint32_t chunks_count = chunks.size();
    out.write(reinterpret_cast<const char*>(&chunks_count), sizeof(chunks_count));
    for (const auto& chunk : chunks) {
        write_string(out, chunk.file_path);
        write_string(out, chunk.content);
        out.write(reinterpret_cast<const char*>(&chunk.start_line), sizeof(chunk.start_line));
        out.write(reinterpret_cast<const char*>(&chunk.end_line), sizeof(chunk.end_line));
        
        uint32_t embed_len = chunk.embedding.size();
        out.write(reinterpret_cast<const char*>(&embed_len), sizeof(embed_len));
        out.write(reinterpret_cast<const char*>(chunk.embedding.data()), embed_len * sizeof(float));
    }
    
    return true;
}

namespace codebase {

void normalize_vector(std::vector<float>& vec) {
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-9f) {
        for (float& v : vec) v /= norm;
    }
}

float cosine_similarity(const std::vector<float>& v1, const std::vector<float>& v2) {
    if (v1.size() != v2.size() || v1.empty()) return 0.0f;
    float dot = 0.0f;
    float norm1 = 0.0f;
    float norm2 = 0.0f;
    for (size_t i = 0; i < v1.size(); ++i) {
        dot += v1[i] * v2[i];
        norm1 += v1[i] * v1[i];
        norm2 += v2[i] * v2[i];
    }
    if (norm1 == 0.0f || norm2 == 0.0f) return 0.0f;
    return dot / (std::sqrt(norm1) * std::sqrt(norm2));
}

static std::vector<RawChunk> post_process_chunks(const std::vector<RawChunk>& raw_chunks) {
    std::vector<RawChunk> processed;
    RawChunk temp_chunk;
    bool has_temp = false;
    
    for (const auto& rc : raw_chunks) {
        uint32_t lines_count = rc.end_line - rc.start_line + 1;
        
        if (has_temp) {
            temp_chunk.content += "\n" + rc.content;
            temp_chunk.end_line = rc.end_line;
            uint32_t merged_lines = temp_chunk.end_line - temp_chunk.start_line + 1;
            
            if (merged_lines < 8) {
                // Keep accumulating
            } else if (merged_lines > 120) {
                // Too big, split
                processed.push_back(temp_chunk);
                has_temp = false;
            } else {
                processed.push_back(temp_chunk);
                has_temp = false;
            }
        } else {
            if (lines_count < 8) {
                temp_chunk = rc;
                has_temp = true;
            } else {
                processed.push_back(rc);
            }
        }
    }
    if (has_temp) {
        processed.push_back(temp_chunk);
    }
    return processed;
}

std::vector<RawChunk> chunk_file(const std::string& path, const std::string& strategy) {
    std::vector<RawChunk> chunks;
    uint64_t size = utils::get_file_size(path);
    if (size > 1024 * 1024) return chunks; // Skip > 1MB
    
    bool read_ok = false;
    std::string content = utils::read_file_content(path, &read_ok);
    if (!read_ok || content.find('\0') != std::string::npos) return chunks; // Skip binary
    
    std::vector<std::string> lines = utils::split(content, '\n');
    if (lines.empty()) return chunks;
    
    std::string ext = "";
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos != std::string::npos) {
        ext = path.substr(dot_pos + 1);
    }
    
    if (strategy == "syntactic") {
        if (ext == "rs" || ext == "js" || ext == "ts" || ext == "go" || ext == "c" || ext == "cpp" || ext == "h" || ext == "hpp" || ext == "java") {
            std::vector<RawChunk> raw_chunks;
            std::vector<std::string> current_chunk;
            uint32_t start_line = 1;
            int depth = 0;
            
            for (size_t i = 0; i < lines.size(); ++i) {
                std::string line = lines[i];
                std::string trimmed = utils::trim(line);
                
                int open_braces = 0;
                int close_braces = 0;
                for (char c : trimmed) {
                    if (c == '{') open_braces++;
                    else if (c == '}') close_braces++;
                }
                
                bool is_definition_start = (depth <= 1) && (
                    utils::starts_with(trimmed, "fn ") ||
                    utils::starts_with(trimmed, "pub fn ") ||
                    utils::starts_with(trimmed, "struct ") ||
                    utils::starts_with(trimmed, "pub struct ") ||
                    utils::starts_with(trimmed, "enum ") ||
                    utils::starts_with(trimmed, "pub enum ") ||
                    utils::starts_with(trimmed, "impl ") ||
                    utils::starts_with(trimmed, "pub impl ") ||
                    utils::starts_with(trimmed, "class ") ||
                    utils::starts_with(trimmed, "function ") ||
                    utils::starts_with(trimmed, "export function ") ||
                    utils::starts_with(trimmed, "async function ")
                );
                
                if (is_definition_start && !current_chunk.empty()) {
                    std::string chunk_text = "";
                    for (const auto& l : current_chunk) chunk_text += l + "\n";
                    raw_chunks.push_back({chunk_text, start_line, (uint32_t)i});
                    current_chunk.clear();
                    start_line = i + 1;
                }
                
                current_chunk.push_back(line);
                depth += open_braces - close_braces;
                if (depth < 0) depth = 0;
            }
            if (!current_chunk.empty()) {
                std::string chunk_text = "";
                for (const auto& l : current_chunk) chunk_text += l + "\n";
                raw_chunks.push_back({chunk_text, start_line, (uint32_t)lines.size()});
            }
            return post_process_chunks(raw_chunks);
        } else if (ext == "py") {
            std::vector<RawChunk> raw_chunks;
            std::vector<std::string> current_chunk;
            uint32_t start_line = 1;
            
            for (size_t i = 0; i < lines.size(); ++i) {
                std::string line = lines[i];
                std::string trimmed = utils::trim(line);
                bool is_def = utils::starts_with(trimmed, "def ") || utils::starts_with(trimmed, "class ");
                bool is_root = is_def && (!line.empty() && !std::isspace(line[0]));
                
                if (is_root && !current_chunk.empty()) {
                    std::string chunk_text = "";
                    for (const auto& l : current_chunk) chunk_text += l + "\n";
                    raw_chunks.push_back({chunk_text, start_line, (uint32_t)i});
                    current_chunk.clear();
                    start_line = i + 1;
                }
                current_chunk.push_back(line);
            }
            if (!current_chunk.empty()) {
                std::string chunk_text = "";
                for (const auto& l : current_chunk) chunk_text += l + "\n";
                raw_chunks.push_back({chunk_text, start_line, (uint32_t)lines.size()});
            }
            return post_process_chunks(raw_chunks);
        } else if (ext == "md") {
            std::vector<RawChunk> raw_chunks;
            std::vector<std::string> current_chunk;
            uint32_t start_line = 1;
            for (size_t i = 0; i < lines.size(); ++i) {
                std::string line = lines[i];
                if (utils::starts_with(line, "#") && !current_chunk.empty()) {
                    std::string chunk_text = "";
                    for (const auto& l : current_chunk) chunk_text += l + "\n";
                    raw_chunks.push_back({chunk_text, start_line, (uint32_t)i});
                    current_chunk.clear();
                    start_line = i + 1;
                }
                current_chunk.push_back(line);
            }
            if (!current_chunk.empty()) {
                std::string chunk_text = "";
                for (const auto& l : current_chunk) chunk_text += l + "\n";
                raw_chunks.push_back({chunk_text, start_line, (uint32_t)lines.size()});
            }
            return post_process_chunks(raw_chunks);
        }
    }
    
    // Default chunker (sliding window)
    uint32_t chunk_size = 40;
    uint32_t overlap = 10;
    uint32_t start = 0;
    while (start < lines.size()) {
        uint32_t end = std::min(start + chunk_size, (uint32_t)lines.size());
        std::string chunk_text = "";
        for (uint32_t i = start; i < end; ++i) {
            chunk_text += lines[i] + "\n";
        }
        chunks.push_back({chunk_text, start + 1, end});
        if (end == lines.size()) break;
        start += chunk_size - overlap;
    }
    
    return chunks;
}

std::string get_db_path(const std::string& workspace_path) {
    std::string syspilot_dir = utils::get_syspilot_directory();
    std::string db_dir = syspilot_dir + "/vector_dbs";
    utils::create_directory_recursive(db_dir);
    
    std::string safe_name = "";
    for (char c : workspace_path) {
        if (std::isalnum(c)) safe_name += c;
        else safe_name += '_';
    }
    return db_dir + "/" + safe_name + ".bin";
}

// Generate embeddings via curl subprocess
static std::vector<std::vector<float>> fetch_embeddings_api(const std::vector<std::string>& texts, const Config& config) {
    std::vector<std::vector<float>> results;
    if (texts.empty()) return results;
    
    std::string temp_payload_path = utils::get_syspilot_directory() + "/temp_embed_req.json";
    
    if (config.active_provider == "gemini") {
        if (config.gemini_api_key.empty()) {
            std::cerr << "⚠️ Gemini API key not set for embedding generation." << std::endl;
            return results;
        }
        
        // Gemini batch embedding API
        std::string model = config.embedding_model;
        if (!utils::starts_with(model, "models/")) {
            model = "models/" + model;
        }
        if (model.find("gemini") == std::string::npos && model.find("embedding") == std::string::npos) {
            model = "models/text-embedding-004";
        }
        
        json jreq = json::object();
        json jrequests = json::array();
        for (const auto& text : texts) {
            json jitem;
            jitem["model"] = model;
            jitem["content"]["parts"] = json::array({ {{"text", text}} });
            jrequests.push_back(jitem);
        }
        jreq["requests"] = jrequests;
        
        utils::write_file_content(temp_payload_path, jreq.dump());
        
        std::string url = "https://generativelanguage.googleapis.com/v1beta/" + model + ":batchEmbedContents?key=" + config.gemini_api_key;
        std::string cmd = "curl -s -X POST \"" + url + "\" -H \"Content-Type: application/json\" -d @" + temp_payload_path;
        
        std::string resp = utils::run_command_output(cmd);
        utils::delete_file(temp_payload_path);
        
        try {
            if (!resp.empty()) {
                json jresp = json::parse(resp);
                if (jresp.contains("embeddings") && jresp["embeddings"].is_array()) {
                    for (const auto& item : jresp["embeddings"]) {
                        if (item.contains("values") && item["values"].is_array()) {
                            std::vector<float> values;
                            for (const auto& v : item["values"]) {
                                values.push_back(v.get<float>());
                            }
                            results.push_back(values);
                        }
                    }
                }
            }
        } catch (...) {}
        
    } else if (config.active_provider == "ollama") {
        // Ollama embedding API - one by one since Ollama batching can overload local models context size
        for (const auto& text : texts) {
            // Truncate to avoid context window blowup
            std::string truncated = text;
            if (truncated.length() > 1000) {
                truncated = truncated.substr(0, 1000);
            }
            
            json jreq;
            jreq["model"] = config.embedding_model;
            jreq["input"] = truncated;
            
            utils::write_file_content(temp_payload_path, jreq.dump());
            
            std::string url = config.ollama_url + "/api/embed";
            std::string cmd = "curl -s -X POST \"" + url + "\" -H \"Content-Type: application/json\" -d @" + temp_payload_path;
            
            std::string resp = utils::run_command_output(cmd);
            
            try {
                if (!resp.empty()) {
                    json jresp = json::parse(resp);
                    if (jresp.contains("embeddings") && jresp["embeddings"].is_array()) {
                        for (const auto& item : jresp["embeddings"]) {
                            if (item.is_array()) {
                                std::vector<float> values;
                                for (const auto& v : item) {
                                    values.push_back(v.get<float>());
                                }
                                results.push_back(values);
                            }
                        }
                    }
                }
            } catch (...) {}
        }
        utils::delete_file(temp_payload_path);
    }
    
    return results;
}

bool update_index(const std::string& workspace_path, const Config& config, bool force) {
    std::string db_file = get_db_path(workspace_path);
    VectorDb db;
    if (utils::file_exists(db_file) && !force) {
        db.load_from_binary(db_file);
    }
    db.workspace_path = workspace_path;
    
    std::vector<std::string> wfiles = utils::list_directory(workspace_path, true);
    std::vector<std::pair<std::string, std::string>> files_to_index; // Absolute path, Relative path
    std::unordered_set<std::string> active_rel_paths;
    
    for (const auto& f : wfiles) {
        std::string rel_path = f;
        if (utils::starts_with(rel_path, workspace_path)) {
            rel_path = rel_path.substr(workspace_path.length());
            if (!rel_path.empty() && rel_path[0] == '/') {
                rel_path = rel_path.substr(1);
            }
        }
        active_rel_paths.insert(rel_path);
        
        uint64_t modified = utils::get_last_modified_time(f);
        uint64_t size = utils::get_file_size(f);
        
        bool modified_check = true;
        for (const auto& reg : db.files) {
            if (reg.file_path == rel_path) {
                if (reg.last_modified == modified && reg.size == size) {
                    modified_check = false;
                }
                break;
            }
        }
        if (modified_check) {
            files_to_index.push_back({f, rel_path});
        }
    }
    
    // Purge deleted files
    db.files.erase(std::remove_if(db.files.begin(), db.files.end(), [&](const FileRegistry& r) {
        return active_rel_paths.find(r.file_path) == active_rel_paths.end();
    }), db.files.end());
    
    db.chunks.erase(std::remove_if(db.chunks.begin(), db.chunks.end(), [&](const DbChunk& c) {
        return active_rel_paths.find(c.file_path) == active_rel_paths.end();
    }), db.chunks.end());
    
    if (!files_to_index.empty()) {
        std::cout << "🔍 Indexing " << files_to_index.size() << " new/modified files..." << std::endl;
        
        std::vector<DbChunk> new_chunks;
        std::vector<FileRegistry> new_registries;
        
        for (const auto& p : files_to_index) {
            // Remove old chunks of this file
            db.chunks.erase(std::remove_if(db.chunks.begin(), db.chunks.end(), [&](const DbChunk& c) {
                return c.file_path == p.second;
            }), db.chunks.end());
            
            std::vector<RawChunk> file_chunks = chunk_file(p.first, config.chunk_strategy);
            for (const auto& fc : file_chunks) {
                DbChunk chunk;
                chunk.file_path = p.second;
                chunk.content = fc.content;
                chunk.start_line = fc.start_line;
                chunk.end_line = fc.end_line;
                new_chunks.push_back(chunk);
            }
            
            FileRegistry reg;
            reg.file_path = p.second;
            reg.last_modified = utils::get_last_modified_time(p.first);
            reg.size = utils::get_file_size(p.first);
            
            // Update or add registry
            auto it = std::find_if(db.files.begin(), db.files.end(), [&](const FileRegistry& r) {
                return r.file_path == p.second;
            });
            if (it != db.files.end()) {
                *it = reg;
            } else {
                db.files.push_back(reg);
            }
        }
        
        if (!new_chunks.empty()) {
            std::cout << "⚡ Batch embedding " << new_chunks.size() << " chunks..." << std::endl;
            
            std::vector<std::string> texts;
            for (const auto& c : new_chunks) texts.push_back(c.content);
            
            // Segment batch embedding into blocks of 50 to avoid big payloads
            size_t batch_size = 50;
            std::vector<std::vector<float>> all_embeddings;
            for (size_t i = 0; i < texts.size(); i += batch_size) {
                size_t end = std::min(i + batch_size, texts.size());
                std::vector<std::string> sub_batch(texts.begin() + i, texts.begin() + end);
                std::vector<std::vector<float>> sub_embeds = fetch_embeddings_api(sub_batch, config);
                all_embeddings.insert(all_embeddings.end(), sub_embeds.begin(), sub_embeds.end());
            }
            
            if (all_embeddings.size() == new_chunks.size()) {
                for (size_t i = 0; i < new_chunks.size(); ++i) {
                    new_chunks[i].embedding = all_embeddings[i];
                    normalize_vector(new_chunks[i].embedding);
                    db.chunks.push_back(new_chunks[i]);
                }
                std::cout << "✅ Embedding complete." << std::endl;
            } else {
                std::cerr << "⚠️ Failed to fetch all embeddings. Batch size mismatch: expected " 
                          << new_chunks.size() << ", got " << all_embeddings.size() << std::endl;
            }
        }
        
        db.save_to_binary(db_file);
        std::cout << "✅ Vector index stored safely." << std::endl;
    }
    
    return true;
}

std::string query_context(const std::string& workspace_path, const std::string& query, const Config& config) {
    update_index(workspace_path, config, false);
    
    std::string db_file = get_db_path(workspace_path);
    VectorDb db;
    if (!db.load_from_binary(db_file) || db.chunks.empty()) {
        return "No indexed files found.";
    }
    
    std::vector<std::vector<float>> q_embeds = fetch_embeddings_api({query}, config);
    if (q_embeds.empty()) {
        return "Failed to embed search query.";
    }
    
    std::vector<float> query_vector = q_embeds[0];
    normalize_vector(query_vector);
    
    std::vector<std::pair<size_t, float>> scores;
    for (size_t i = 0; i < db.chunks.size(); ++i) {
        if (db.chunks[i].embedding.size() != query_vector.size()) continue;
        float sim = cosine_similarity(db.chunks[i].embedding, query_vector);
        scores.push_back({i, sim});
    }
    
    std::sort(scores.begin(), scores.end(), [](const std::pair<size_t, float>& a, const std::pair<size_t, float>& b) {
        return a.second > b.second;
    });
    
    std::string context_str = "";
    size_t top_k = std::min(scores.size(), (size_t)8);
    size_t char_budget = 8000;
    
    for (size_t i = 0; i < top_k; ++i) {
        size_t idx = scores[i].first;
        float sim = scores[i].second;
        if (sim < 0.2f) continue; // Skip weak matches
        
        std::string chunk_format = "--- Chunk " + std::to_string(i + 1) + " from '" + db.chunks[idx].file_path 
            + "' (Lines " + std::to_string(db.chunks[idx].start_line) + "-" + std::to_string(db.chunks[idx].end_line)
            + ", Similarity: " + std::to_string(sim).substr(0, 4) + ") ---\n" + db.chunks[idx].content + "\n\n";
            
        if (context_str.length() + chunk_format.length() > char_budget) {
            break;
        }
        context_str += chunk_format;
    }
    
    return context_str.empty() ? "No matching context found." : context_str;
}

} // namespace codebase
