// ─────────────────────────────────────────────────────────────────────────────
//  HPC Global Allocator — mimalloc (Microsoft Research)
//  Must be FIRST include to intercept all new/delete/malloc/free calls.
//  Provides: 40-70% faster allocation vs glibc malloc, thread-local heaps,
//  reduced fragmentation, and better cache locality.
// ─────────────────────────────────────────────────────────────────────────────
#include <mimalloc-new-delete.h>

#include "ai.h"
#include "causal_engine.h"
#include "codebase.h"
#include "config.h"
#include "daemon.h"
#include "install.h"
#include "nlohmann/json.hpp"
#include "profiler.h"
#include "safety.h"
#include "telemetry.h"
#include "ui/streamer.h"
#include "ui/tui.h"
#include "utils.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;

struct LogEntry {
  std::string timestamp;
  std::string directory;
  std::string command;
  int exit_code = 0;
};

// Reads recent entries from ~/.syspilot/context.log
std::vector<LogEntry> read_recent_entries(size_t count) {
  std::vector<LogEntry> entries;
  std::string path = utils::get_syspilot_directory() + "/context.log";
  if (!utils::file_exists(path))
    return entries;

  std::ifstream file(path);
  if (!file.is_open())
    return entries;

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty())
      lines.push_back(line);
  }

  // Parse backwards
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    std::vector<std::string> parts = utils::split(*it, " | ");
    if (parts.size() == 4) {
      std::string cmd = utils::trim(parts[2]);
      if (utils::starts_with(cmd, "syspilot"))
        continue;

      LogEntry entry;
      entry.timestamp = parts[0];
      entry.directory = parts[1];
      entry.command = cmd;
      try {
        entry.exit_code = std::stoi(parts[3]);
      } catch (...) {
        entry.exit_code = 0;
      }
      entries.push_back(entry);
      if (entries.size() >= count)
        break;
    }
  }

  return entries;
}

// Tails ~/.syspilot/session.log
std::string tail_session(size_t max_lines) {
  std::string path = utils::get_syspilot_directory() + "/session.log";
  if (!utils::file_exists(path))
    return "";

  std::ifstream file(path);
  if (!file.is_open())
    return "";

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(line);
  }

  std::string result = "";
  size_t start = lines.size() > max_lines ? lines.size() - max_lines : 0;
  for (size_t i = start; i < lines.size(); ++i) {
    result += lines[i] + "\n";
  }
  return result;
}

void print_help() {
  std::cout << "🤖 SysPilot: Operating System Reasoning Agent (C++ Edition)\n\n"
            << "Usage: syspilot <command> [options]\n\n"
            << "Commands:\n"
            << "  install                       Install terminal hooks and "
               "configuration\n"
            << "  uninstall                     Remove terminal hooks\n"
            << "  status                        Check integration status\n"
            << "  daemon                        Start the background SysPilot netlink daemon\n"
            << "  monitor                       Open the real-time diagnostic TUI\n"
            << "  provider <name>               Set active AI provider "
               "(gemini, ollama, syspilot)\n"
            << "  model <name>                  Set active model name\n"
            << "  pull <model> [--set-active]   Pull a model using Ollama\n"
            << "  index [--force]               Index current codebase for "
               "vector search\n"
            << "  config <action>               Manage settings\n"
            << "     set-key <provider> <key>   Set provider API key\n"
            << "     set-url <provider> <url>   Set provider API endpoint URL\n"
            << "     set <option> <value>       Set option (chunk_strategy, "
               "embedding_model)\n"
            << "  ask \"<question>\" [options]    Ask general tech or codebase "
               "questions\n"
            << "     --file <path>              Provide file content context\n"
            << "     --no-index                 Skip codebase vector search\n"
            << "  explain [options]             Analyze system telemetry and "
               "execution\n"
            << "     --pid <pid/name>           Target active process "
               "telemetry & profiling\n"
            << "     --deep                     Include perf CPU profiler / "
               "system telemetry\n"
            << "     --ebpf                     Enable real-time eBPF event "
               "tracing (requires root/sudo)\n"
            << "     --causal                   Run real-time causal graph "
               "reasoning (CausalTrace)\n"
            << "     --number <N>               Use N-th last failed command "
               "(default: 1)\n"
            << "     --no-index                 Skip codebase vector search\n\n"
            << "Examples:\n"
            << "  syspilot explain --pid my_server --causal\n"
            << "  syspilot explain --pid my_server --deep\n"
            << "  syspilot explain\n"
            << "  syspilot ask \"why does my DB query block?\"\n";
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_help();
    return 0;
  }

  Config conf = config::load();
  std::string cmd = argv[1];

  if (cmd == "install") {
    install::install();
  } else if (cmd == "uninstall") {
    install::uninstall();
  } else if (cmd == "status") {
    install::status();
  } else if (cmd == "daemon") {
    return daemon_service::run_daemon();
  } else if (cmd == "monitor") {
    ui::run_monitor();
    return 0;
  } else if (cmd == "provider") {
    if (argc < 3) {
      std::cerr << "❌ Expected provider name (gemini, ollama, syspilot)"
                << std::endl;
      return 1;
    }
    std::string prov = utils::to_lower(argv[2]);
    if (prov != "gemini" && prov != "ollama" && prov != "syspilot") {
      std::cerr << "❌ Unknown provider: " << prov
                << ". Use: gemini, ollama, syspilot." << std::endl;
      return 1;
    }
    conf.active_provider = prov;
    config::save(conf);
    std::cout << "✅ Active provider set to " << prov << std::endl;
  } else if (cmd == "model") {
    if (argc < 3) {
      std::cerr << "❌ Expected model name." << std::endl;
      return 1;
    }
    std::string model = argv[2];
    if (conf.active_provider == "gemini")
      conf.gemini_model = model;
    else if (conf.active_provider == "ollama")
      conf.ollama_model = model;
    else if (conf.active_provider == "syspilot")
      conf.syspilot_model = model;

    config::save(conf);
    std::cout << "✅ Model set to " << model << " for " << conf.active_provider
              << std::endl;
  } else if (cmd == "pull") {
    if (argc < 3) {
      std::cerr << "❌ Expected model name." << std::endl;
      return 1;
    }
    std::string model = argv[2];
    bool set_active = false;
    if (argc >= 4 && std::string(argv[3]) == "--set-active") {
      set_active = true;
    }

    if (conf.active_provider != "ollama") {
      std::cout << "⚠️ Current provider is " << conf.active_provider
                << ". Model pulling only works with Ollama." << std::endl;
      std::cout << "Switch to Ollama? [Y/n] ";
      std::string input;
      std::getline(std::cin, input);
      std::string trimmed = utils::to_lower(utils::trim(input));
      if (trimmed != "y" && !trimmed.empty()) {
        std::cout << "Pull aborted." << std::endl;
        return 0;
      }
      conf.active_provider = "ollama";
      config::save(conf);
    }

    if (ai::pull_ollama_model(conf, model)) {
      if (set_active) {
        conf.ollama_model = model;
        config::save(conf);
        std::cout << "🔧 Set as active Ollama model." << std::endl;
      }
    }
  } else if (cmd == "index") {
    bool force = false;
    if (argc >= 3 && std::string(argv[2]) == "--force") {
      force = true;
    }
    std::string pwd = utils::run_command_output("pwd");
    pwd = utils::trim(pwd);
    codebase::update_index(pwd, conf, force);
  } else if (cmd == "config") {
    if (argc < 3) {
      std::cerr << "❌ Expected config action (set-key, set-url, set)"
                << std::endl;
      return 1;
    }
    std::string action = argv[2];
    if (action == "set-key") {
      if (argc < 5) {
        std::cerr << "❌ Expected: syspilot config set-key <provider> <key>"
                  << std::endl;
        return 1;
      }
      std::string prov = utils::to_lower(argv[3]);
      std::string key = argv[4];
      if (prov == "gemini")
        conf.gemini_api_key = key;
      else if (prov == "syspilot")
        conf.syspilot_api_key = key;
      else {
        std::cerr << "❌ Provider " << prov << " does not use API keys."
                  << std::endl;
        return 1;
      }
      config::save(conf);
      std::cout << "✅ API key set for " << prov << std::endl;
    } else if (action == "set-url") {
      if (argc < 5) {
        std::cerr << "❌ Expected: syspilot config set-url <provider> <url>"
                  << std::endl;
        return 1;
      }
      std::string prov = utils::to_lower(argv[3]);
      std::string url = argv[4];
      if (prov == "ollama")
        conf.ollama_url = url;
      else {
        std::cerr << "❌ Provider " << prov << " does not support custom URLs."
                  << std::endl;
        return 1;
      }
      config::save(conf);
      std::cout << "✅ URL set for " << prov << std::endl;
    } else if (action == "set") {
      if (argc < 5) {
        std::cerr << "❌ Expected: syspilot config set <option> <value>"
                  << std::endl;
        return 1;
      }
      std::string option = utils::to_lower(argv[3]);
      std::string val = argv[4];
      if (option == "chunk_strategy" || option == "strategy") {
        if (val != "syntactic" && val != "line") {
          std::cerr << "❌ Invalid chunk strategy: " << val
                    << ". Options: syntactic, line" << std::endl;
          return 1;
        }
        conf.chunk_strategy = val;
      } else if (option == "embedding_model" || option == "model") {
        conf.embedding_model = val;
      } else {
        std::cerr << "❌ Unknown option: " << option
                  << ". Options: chunk_strategy, embedding_model" << std::endl;
        return 1;
      }
      config::save(conf);
      std::cout << "✅ Config " << option << " set to " << val << std::endl;
    } else {
      std::cerr << "❌ Unknown config action: " << action << std::endl;
      return 1;
    }
  } else if (cmd == "ask") {
    if (argc < 3) {
      std::cerr << "❌ Expected question." << std::endl;
      return 1;
    }
    std::string question = argv[2];
    std::string file_path = "";
    bool no_index = false;

    for (int i = 3; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--file" && i + 1 < argc) {
        file_path = argv[++i];
      } else if (arg == "--no-index") {
        no_index = true;
      }
    }

    std::cout << "\n🧠 \x1b[1;32mSysPilot Answer:\x1b[0m\n" << std::endl;

    // Build Context
    json ctx;
    ctx["current_dir"] = utils::trim(utils::run_command_output("pwd"));
    ctx["file_list"] =
        utils::trim(utils::run_command_output("ls -lA 2>/dev/null"));

    if (utils::file_exists(".git")) {
      ctx["git_branch"] = utils::trim(
          utils::run_command_output("git branch --show-current 2>/dev/null"));
      ctx["git_status"] = utils::trim(
          utils::run_command_output("git status --porcelain 2>/dev/null"));
    }

    if (!file_path.empty()) {
      bool ok = false;
      std::string content = utils::read_file_content(file_path, &ok);
      if (ok) {
        // Truncate if long
        std::vector<std::string> lines = utils::split(content, '\n');
        std::string truncated = "";
        for (size_t i = 0; i < std::min(lines.size(), (size_t)300); ++i) {
          truncated += lines[i] + "\n";
        }
        if (lines.size() > 300) {
          truncated += "\n... (file truncated to 300 lines)\n";
        }
        ctx["file_content"] =
            "File '" + file_path + "' contents:\n" + truncated;
      } else {
        ctx["file_content"] = "Could not read file '" + file_path + "'";
      }
    }

    if (!no_index) {
      std::cout << "🔍 Searching local codebase context..." << std::endl;
      ctx["codebase_context"] =
          codebase::query_context(ctx["current_dir"], question, conf);
    }

    std::string prompt =
        "Terminal Context:\n" + ctx.dump(4) + "\n\nQuestion: " + question;

    MdStreamer streamer;
    ai::query_ai_stream(conf, prompt, streamer);
    std::cout << "\n\x1b[90m" << std::string(60, '-') << "\x1b[0m" << std::endl;
  } else if (cmd == "explain") {
    std::string target_pid_or_name = "";
    bool deep = false;
    bool ebpf = false;
    bool causal = false;
    int offset = 1;
    bool no_index = false;

    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--pid" && i + 1 < argc) {
        target_pid_or_name = argv[++i];
      } else if (arg == "--deep") {
        deep = true;
      } else if (arg == "--ebpf") {
        ebpf = true;
      } else if (arg == "--causal") {
        causal = true;
      } else if (arg == "--number" && i + 1 < argc) {
        try {
          offset = std::stoi(argv[++i]);
        } catch (...) {
          offset = 1;
        }
      } else if (arg == "--no-index") {
        no_index = true;
      }
    }

    std::cout << "\n🤖 \x1b[1;36mSysPilot Explanation:\x1b[0m\n" << std::endl;

    json ctx;
    ctx["current_dir"] = utils::trim(utils::run_command_output("pwd"));

    if (!target_pid_or_name.empty()) {
      pid_t pid = telemetry::find_pid_by_name(target_pid_or_name);
      if (pid == 0) {
        std::cerr << "❌ Could not find process PID for: " << target_pid_or_name
                  << std::endl;
        return 1;
      }

      if (causal) {
        std::cout << "🌐 Constructing real-time Causal Dependency Graph "
                     "(CausalTrace)..."
                  << std::endl;
        CausalGraph graph;
        graph.build_graph(2, ebpf, pid); // 2-second snapshot window

        std::string target_node_id = "pid:" + std::to_string(pid);
        std::cout << "🔍 Tracing root causes starting from "
                  << target_pid_or_name << " (" << target_node_id << ")..."
                  << std::endl;

        std::vector<std::string> path = graph.trace_root_cause(target_node_id);
        std::string json_chain = graph.serialize_chain_to_json(path);

        ctx["causal_chain"] = json::parse(json_chain);
        ctx["analysis_type"] = "causal_inference_diagnostics";
        ctx["target_process"] = target_pid_or_name;
        ctx["target_pid"] = pid;

        // Create directory for graph visual reports (non-hidden so
        // snap-confined browsers can access it)
        const char *home_env = std::getenv("HOME");
        std::string reports_dir =
            (home_env ? std::string(home_env) : ".") + "/syspilot_reports";
        utils::run_command_output("mkdir -p " + reports_dir);

        // Write DOT and HTML reports
        std::string ts = std::to_string(std::time(nullptr));
        std::string dot_path = reports_dir + "/causal_graph_" + ts + ".dot";
        std::string html_path = reports_dir + "/causal_graph_" + ts + ".html";

        std::ofstream dot_file(dot_path);
        if (dot_file.is_open()) {
          dot_file << graph.export_graph_to_dot(path);
          dot_file.close();
        }

        std::ofstream html_file(html_path);
        if (html_file.is_open()) {
          html_file << graph.export_graph_to_html(path);
          html_file.close();
        }

        std::cout << "💾 Saved causal dependency graph to:" << std::endl;
        std::cout << "   - DOT format:  " << dot_path << std::endl;
        std::cout << "   - HTML format: " << html_path << "\n" << std::endl;

        if (!no_index) {
          std::cout << "🔍 Mapping causal nodes back to codebase..."
                    << std::endl;
          ctx["codebase_context"] = codebase::query_context(
              ctx["current_dir"], target_pid_or_name, conf);
        }
      } else {
        // ==========================================
        // 1. New Operating System Reasoning Agent Mode
        // ==========================================
        std::cout << "📊 Gathering low-level telemetry for PID " << pid << " ("
                  << target_pid_or_name << ")..." << std::endl;
        ProcessTelemetry pt = telemetry::collect_process_telemetry(pid);
        SystemTelemetry st = telemetry::collect_system_telemetry();

        if (ebpf) {
          std::string ebpf_log = telemetry::collect_ebpf_telemetry(pid, 5);
          ctx["ebpf_events"] = ebpf_log;
        }

        std::cout << "🔬 Extracting thread stack traces & active profile..."
                  << std::endl;
        ProfileReport pr = profiler::profile_process(
            pid, deep); // Run perf hotspot profiler if deep

        ctx["telemetry"] =
            json::parse(telemetry::serialize_telemetry_to_json_string(pt, st));
        ctx["execution_profile"] =
            json::parse(profiler::serialize_profile_to_json_string(pr));

        // Search codebase for top functions/symbols in call stack if available
        std::string query_terms = pt.name;
        for (const auto &sym : pr.top_symbols) {
          query_terms += " " + sym.first;
        }
        for (const auto &stack : pr.active_stacks) {
          for (size_t i = 0; i < std::min(stack.frames.size(), (size_t)3);
               ++i) {
            query_terms += " " + stack.frames[i];
          }
        }

        if (!no_index) {
          std::cout << "🔍 Mapping execution profile back to codebase..."
                    << std::endl;
          ctx["codebase_context"] =
              codebase::query_context(ctx["current_dir"], query_terms, conf);
        }

        ctx["analysis_type"] = "process_telemetry_and_profiling";
        ctx["target_process"] = target_pid_or_name;
        ctx["target_pid"] = pid;
      }
    } else {
      // ==========================================
      // 2. Classic Terminal Command Debugging Mode
      // ==========================================
      std::vector<LogEntry> recent = read_recent_entries(offset);
      LogEntry target_entry;
      if (!recent.empty()) {
        target_entry = recent.back();
      } else {
        target_entry.command = "No recent command found";
        target_entry.exit_code = 0;
      }

      ctx["last_command"] = target_entry.command;
      ctx["exit_code"] = target_entry.exit_code;
      ctx["last_session_snippet"] = tail_session(100);
      ctx["file_list"] =
          utils::trim(utils::run_command_output("ls -lA 2>/dev/null"));

      if (utils::file_exists(".git")) {
        ctx["git_branch"] = utils::trim(
            utils::run_command_output("git branch --show-current 2>/dev/null"));
        ctx["git_status"] = utils::trim(
            utils::run_command_output("git status --porcelain 2>/dev/null"));
      }

      if (deep) {
        ctx["disk_usage"] =
            utils::trim(utils::run_command_output("df -h 2>/dev/null"));
        ctx["memory_usage"] =
            utils::trim(utils::run_command_output("free -h 2>/dev/null"));
        ctx["open_ports"] =
            utils::trim(utils::run_command_output("ss -tlnp 2>/dev/null"));
      }

      if (!no_index) {
        std::cout << "🔍 Searching codebase for command context..."
                  << std::endl;
        std::string fallback_query =
            target_entry.command + "\n" + tail_session(10);
        ctx["codebase_context"] =
            codebase::query_context(ctx["current_dir"], fallback_query, conf);
      }

      ctx["analysis_type"] = "command_failure_diagnostics";
    }

    std::string prompt;
    if (causal) {
      prompt = "You are a senior system reliability engineer performing "
               "root-cause analysis.\n"
               "We have built a directed dependency graph of the operating "
               "system processes and resources in real-time.\n"
               "When a performance anomaly was detected or requested, we "
               "performed a reverse Breadth-First Search (BFS) "
               "starting from the symptomatic process node to trace back to "
               "potential root causes.\n\n"
               "Here is the structured JSON representation of the traversed "
               "causal path (nodes and edges):\n" +
               ctx.dump(4) +
               "\n\n"
               "Please explain the diagnostic findings to the user:\n"
               "1. User-facing symptom (the starting process and what is "
               "anomalous about it)\n"
               "2. Step-by-step root cause chain (how the symptom maps back to "
               "the resources, blockages, or other processes)\n"
               "3. Which specific process is the root cause, and why\n"
               "4. Recommended actionable mitigation (e.g. kill -STOP <PID>, "
               "optimize disk access, scale resources)\n"
               "Be extremely specific, detail I/O rates or CPU usage where "
               "applicable. Provide a Confidence Score (0-100%).";
    } else {
      prompt = "Perform causal reasoning on the following OS telemetry and "
               "execution context to diagnose the system state.\n\n"
               "OS Telemetry and Context:\n" +
               ctx.dump(4);
    }

    MdStreamer streamer;
    ai::query_ai_stream(conf, prompt, streamer);
    std::cout << "\n\x1b[90m" << std::string(60, '-') << "\x1b[0m" << std::endl;
  } else {
    print_help();
    return 1;
  }

  return 0;
}
