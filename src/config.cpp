#include "config.h"
#include "utils.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <cstdlib>
#include <iostream>

using json = nlohmann::json;

namespace config {

Config load() {
    Config conf;
    std::string dir = utils::get_syspilot_directory();
    std::string path = dir + "/config.json";
    
    if (utils::file_exists(path)) {
        try {
            std::string content = utils::read_file_content(path);
            if (!content.empty()) {
                json j = json::parse(content);
                
                // Read active_provider
                if (j.contains("active_provider") && j["active_provider"].is_string()) {
                    conf.active_provider = j["active_provider"].get<std::string>();
                }
                
                // Read gemini_api_key
                if (j.contains("gemini_api_key") && j["gemini_api_key"].is_string()) {
                    conf.gemini_api_key = j["gemini_api_key"].get<std::string>();
                }
                
                // Read gemini_model
                if (j.contains("gemini_model") && j["gemini_model"].is_string()) {
                    conf.gemini_model = j["gemini_model"].get<std::string>();
                }
                
                // Read syspilot_api_key
                if (j.contains("syspilot_api_key") && j["syspilot_api_key"].is_string()) {
                    conf.syspilot_api_key = j["syspilot_api_key"].get<std::string>();
                }
                
                // Read syspilot_model
                if (j.contains("syspilot_model") && j["syspilot_model"].is_string()) {
                    conf.syspilot_model = j["syspilot_model"].get<std::string>();
                }
                
                // Read ollama_url
                if (j.contains("ollama_url") && j["ollama_url"].is_string()) {
                    conf.ollama_url = j["ollama_url"].get<std::string>();
                }
                
                // Read ollama_model
                if (j.contains("ollama_model") && j["ollama_model"].is_string()) {
                    conf.ollama_model = j["ollama_model"].get<std::string>();
                }
                
                // Read embedding_model
                if (j.contains("embedding_model") && j["embedding_model"].is_string()) {
                    conf.embedding_model = j["embedding_model"].get<std::string>();
                }
                
                // Read chunk_strategy
                if (j.contains("chunk_strategy") && j["chunk_strategy"].is_string()) {
                    conf.chunk_strategy = j["chunk_strategy"].get<std::string>();
                }
                
                // Support old format fallback
                if (conf.gemini_api_key.empty() && j.contains("model") && j["model"].is_string()) {
                    conf.gemini_model = j["model"].get<std::string>();
                }
            }
        } catch (...) {
            // If parsing fails, use defaults
        }
    }
    
    // Override with environment variables if present
    const char* gemini_env = std::getenv("GEMINI_API_KEY");
    if (gemini_env) {
        conf.gemini_api_key = std::string(gemini_env);
    }
    
    const char* syspilot_env = std::getenv("SYSPILOT_API_KEY");
    if (syspilot_env) {
        conf.syspilot_api_key = std::string(syspilot_env);
    }
    
    return conf;
}

bool save(const Config& conf) {
    std::string dir = utils::get_syspilot_directory();
    utils::create_directory_recursive(dir);
    std::string path = dir + "/config.json";
    
    try {
        json j;
        j["active_provider"] = conf.active_provider;
        j["gemini_api_key"] = conf.gemini_api_key;
        j["gemini_model"] = conf.gemini_model;
        j["syspilot_api_key"] = conf.syspilot_api_key;
        j["syspilot_model"] = conf.syspilot_model;
        j["ollama_url"] = conf.ollama_url;
        j["ollama_model"] = conf.ollama_model;
        j["embedding_model"] = conf.embedding_model;
        j["chunk_strategy"] = conf.chunk_strategy;
        
        return utils::write_file_content(path, j.dump(4));
    } catch (...) {
        return false;
    }
}

} // namespace config
