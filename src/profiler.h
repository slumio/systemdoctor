#ifndef PROFILER_H
#define PROFILER_H

#include <string>
#include <vector>
#include <sys/types.h>

struct StackTrace {
    pid_t tid = 0;
    std::vector<std::string> frames;
};

struct ProfileReport {
    bool perf_available = false;
    std::vector<std::pair<std::string, double>> top_symbols; // Name, percentage
    std::string call_graph_summary = "";
    std::vector<StackTrace> active_stacks;
};

namespace profiler {

ProfileReport profile_process(pid_t pid, bool run_perf = false);
std::string serialize_profile_to_json_string(const ProfileReport& report);

} // namespace profiler

#endif // PROFILER_H
