#include "safety.h"
#include "utils.h"
#include <vector>
#include <algorithm>

namespace safety {

const std::vector<std::string> DENYLIST = {
    "rm", "sudo", "chmod", "chown", "kill", "pkill", "mv", "cp", "dd", "mkfs", "reboot", "shutdown"
};

bool check_command(const std::string& cmd) {
    std::string trimmed = utils::trim(cmd);
    if (trimmed.empty()) {
        return true;
    }
    
    // Find the first token (binary name)
    std::string base_cmd;
    size_t space_pos = trimmed.find_first_of(" \t\r\n");
    if (space_pos == std::string::npos) {
        base_cmd = trimmed;
    } else {
        base_cmd = trimmed.substr(0, space_pos);
    }
    
    // Strip leading path if present (e.g. /bin/rm -> rm)
    size_t slash_pos = base_cmd.find_last_of('/');
    if (slash_pos != std::string::npos) {
        base_cmd = base_cmd.substr(slash_pos + 1);
    }
    
    base_cmd = utils::to_lower(base_cmd);
    
    if (std::find(DENYLIST.begin(), DENYLIST.end(), base_cmd) != DENYLIST.end()) {
        return false;
    }
    
    return true;
}

} // namespace safety
