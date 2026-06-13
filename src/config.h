#ifndef CONFIG_H
#define CONFIG_H

#include <string>

struct Config {
    std::string active_provider = "gemini";
    std::string gemini_api_key = "";
    std::string gemini_model = "gemini-3.5-flash";
    std::string syspilot_api_key = "";
    std::string syspilot_model = "syspilot-default";
    std::string ollama_url = "http://127.0.0.1:11434";
    std::string ollama_model = "llama3";
    std::string embedding_model = "qllama/bge-small-en-v1.5";
    std::string chunk_strategy = "syntactic";
};

namespace config {

Config load();
bool save(const Config& conf);

} // namespace config

#endif // CONFIG_H
