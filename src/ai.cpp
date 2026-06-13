#include "ai.h"
#include "utils.h"
#include "nlohmann/json.hpp"
#include <iostream>
#include <fstream>
#include <cstdio>
#include <memory>
#include <array>

using json = nlohmann::json;

namespace ai {

const std::string REASONING_SYSTEM_PROMPT = 
    "You are the SysPilot Operating System Reasoning Agent, an expert in kernel subsystems, hardware architecture, "
    "program analysis, and systems performance engineering.\n"
    "Your job is to analyze low-level operating system telemetry, thread scheduling, memory management, I/O operations, "
    "performance counters, and execution stacks, and explain the underlying causes of the observed behavior.\n"
    "Always seek to answer:\n"
    "1. What happened inside the machine?\n"
    "2. Why did it happen? What was the root cause?\n"
    "3. Which process, thread, function, or source code location triggered it?\n"
    "4. How did the effects propagate through the OS subsystems (scheduler, memory manager, VFS, block layer, network stack) and hardware?\n"
    "5. What was the impact on performance, latency, throughput, stability, and resource utilization?\n"
    "6. What specific recommendations (code modifications, configuration changes, architectural adjustments) will resolve the root cause?\n\n"
    "Connect low-level metrics to high-level software constructs. For instance, do not just say 'cache misses increased'; "
    "explain that a specific data structure or access pattern caused cache thrashing, stalling the pipeline. "
    "Do not just report 'high CPU'; trace it to lock contention or busy waiting.\n\n"
    "IMPORTANT TERMINAL FORMATTING RULES:\n"
    "1. When outputting mathematical formulas or equations, NEVER use raw LaTeX syntax like \\frac or \\sqrt.\n"
    "2. Instead, you MUST use user-friendly, plain-text ASCII math (e.g., (a + b) / c, sqrt(x)).\n"
    "3. Wrap code or math snippets in backticks (`).\n"
    "4. Keep explanations concise, clear, and highly technical.";

bool query_ai_stream(const Config& config, const std::string& prompt, MdStreamer& streamer) {
    std::string temp_payload_path = utils::get_syspilot_directory() + "/temp_ai_req.json";
    std::string url = "";
    std::string headers = "-H \"Content-Type: application/json\"";
    
    if (config.active_provider == "gemini") {
        if (config.gemini_api_key.empty()) {
            std::cerr << "❌ Gemini API key is not set. Run `syspilot config set-key gemini YOUR_KEY`" << std::endl;
            return false;
        }
        url = "https://generativelanguage.googleapis.com/v1beta/models/" + config.gemini_model + ":streamGenerateContent?alt=sse&key=" + config.gemini_api_key;
        
        json jreq;
        jreq["contents"] = json::array({ {{"parts", json::array({ {{"text", prompt}} })}} });
        jreq["systemInstruction"] = {{"parts", json::array({ {{"text", REASONING_SYSTEM_PROMPT}} })}};
        
        utils::write_file_content(temp_payload_path, jreq.dump());
    } 
    else if (config.active_provider == "ollama") {
        url = config.ollama_url + "/api/chat";
        
        json jreq;
        jreq["model"] = config.ollama_model;
        jreq["messages"] = json::array({
            {{"role", "system"}, {"content", REASONING_SYSTEM_PROMPT}},
            {{"role", "user"}, {"content", prompt}}
        });
        jreq["stream"] = true;
        
        utils::write_file_content(temp_payload_path, jreq.dump());
    }
    else if (config.active_provider == "syspilot") {
        if (config.syspilot_api_key.empty()) {
            std::cerr << "❌ SysPilot API key is not set. Run `syspilot config set-key syspilot YOUR_KEY`" << std::endl;
            return false;
        }
        url = "https://api.syspilot.dev/v1/chat/completions";
        headers += " -H \"Authorization: Bearer " + config.syspilot_api_key + "\"";
        
        json jreq;
        jreq["model"] = config.syspilot_model;
        jreq["messages"] = json::array({
            {{"role", "system"}, {"content", REASONING_SYSTEM_PROMPT}},
            {{"role", "user"}, {"content", prompt}}
        });
        jreq["stream"] = true;
        
        utils::write_file_content(temp_payload_path, jreq.dump());
    }
    else {
        std::cerr << "❌ Unknown AI provider: " << config.active_provider << std::endl;
        return false;
    }
    
    std::string cmd = "curl -s -N -X POST " + headers + " -d @" + temp_payload_path + " \"" + url + "\"";
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        std::cerr << "❌ Failed to start curl process." << std::endl;
        utils::delete_file(temp_payload_path);
        return false;
    }
    
    std::array<char, 1024> buffer;
    std::string line_buffer = "";
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        line_buffer += buffer.data();
        
        size_t newline_pos = 0;
        while ((newline_pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = utils::trim(line_buffer.substr(0, newline_pos));
            line_buffer = line_buffer.substr(newline_pos + 1);
            
            if (line.empty()) continue;
            
            if (config.active_provider == "gemini") {
                if (utils::starts_with(line, "data: ")) {
                    std::string data = utils::trim(line.substr(6));
                    if (data.empty()) continue;
                    try {
                        json jdata = json::parse(data);
                        if (jdata.contains("candidates") && jdata["candidates"].is_array() && !jdata["candidates"].empty()) {
                            auto& cand = jdata["candidates"][0];
                            if (cand.contains("content") && cand["content"].contains("parts")) {
                                for (auto& part : cand["content"]["parts"]) {
                                    if (part.contains("text") && part["text"].is_string()) {
                                        streamer.print(part["text"].get<std::string>());
                                    }
                                }
                            }
                        }
                    } catch (...) {}
                }
            } 
            else if (config.active_provider == "ollama") {
                try {
                    json jdata = json::parse(line);
                    if (jdata.contains("message") && jdata["message"].contains("content") && jdata["message"]["content"].is_string()) {
                        streamer.print(jdata["message"]["content"].get<std::string>());
                    }
                } catch (...) {}
            }
            else if (config.active_provider == "syspilot") {
                if (utils::starts_with(line, "data: ")) {
                    std::string data = utils::trim(line.substr(6));
                    if (data == "[DONE]") break;
                    if (data.empty()) continue;
                    try {
                        json jdata = json::parse(data);
                        if (jdata.contains("choices") && jdata["choices"].is_array() && !jdata["choices"].empty()) {
                            auto& choice = jdata["choices"][0];
                            if (choice.contains("delta") && choice["delta"].contains("content") && choice["delta"]["content"].is_string()) {
                                streamer.print(choice["delta"]["content"].get<std::string>());
                            }
                        }
                    } catch (...) {}
                }
            }
        }
    }
    
    // Print remainder if any
    if (!line_buffer.empty()) {
        std::string line = utils::trim(line_buffer);
        if (config.active_provider == "ollama") {
            try {
                json jdata = json::parse(line);
                if (jdata.contains("message") && jdata["message"].contains("content") && jdata["message"]["content"].is_string()) {
                    streamer.print(jdata["message"]["content"].get<std::string>());
                }
            } catch (...) {}
        }
    }
    
    streamer.flush();
    std::cout << std::endl;
    
    utils::delete_file(temp_payload_path);
    return true;
}

bool pull_ollama_model(const Config& config, const std::string& model_name) {
    std::string temp_payload_path = utils::get_syspilot_directory() + "/temp_pull_req.json";
    
    json jreq;
    jreq["name"] = model_name;
    jreq["stream"] = true;
    
    utils::write_file_content(temp_payload_path, jreq.dump());
    
    std::string url = config.ollama_url + "/api/pull";
    std::string cmd = "curl -s -N -X POST -H \"Content-Type: application/json\" -d @" + temp_payload_path + " \"" + url + "\"";
    
    std::cout << "⬇️ Pulling model '" << model_name << "'..." << std::endl;
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        std::cerr << "❌ Failed to start curl process." << std::endl;
        utils::delete_file(temp_payload_path);
        return false;
    }
    
    std::array<char, 512> buffer;
    std::string line_buffer = "";
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        line_buffer += buffer.data();
        
        size_t newline_pos = 0;
        while ((newline_pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = utils::trim(line_buffer.substr(0, newline_pos));
            line_buffer = line_buffer.substr(newline_pos + 1);
            
            if (line.empty()) continue;
            
            try {
                json jdata = json::parse(line);
                if (jdata.contains("status") && jdata["status"].is_string()) {
                    std::string status = jdata["status"].get<std::string>();
                    
                    if (jdata.contains("completed") && jdata.contains("total")) {
                        double completed = jdata["completed"].get<double>();
                        double total = jdata["total"].get<double>();
                        if (total > 0) {
                            double pct = (completed / total) * 100.0;
                            std::printf("\r%-40s [%.2f%%]", status.c_str(), pct);
                            std::fflush(stdout);
                        } else {
                            std::printf("\r%-50s", status.c_str());
                            std::fflush(stdout);
                        }
                    } else {
                        std::printf("\r%-50s", status.c_str());
                        std::fflush(stdout);
                    }
                }
            } catch (...) {}
        }
    }
    std::cout << std::endl;
    
    utils::delete_file(temp_payload_path);
    return true;
}

} // namespace ai
