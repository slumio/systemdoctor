#ifndef AI_H
#define AI_H

#include "config.h"
#include "ui/streamer.h"
#include <string>

namespace ai {

extern const std::string REASONING_SYSTEM_PROMPT;

bool query_ai_stream(const Config& config, const std::string& prompt, MdStreamer& streamer);
bool pull_ollama_model(const Config& config, const std::string& model_name);

} // namespace ai

#endif // AI_H
