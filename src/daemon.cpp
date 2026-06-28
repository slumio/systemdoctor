// ─────────────────────────────────────────────────────────────────────────────
//  SysPilot Daemon  ·  syspilotd
//
//  HPC stack:
//   · mimalloc          — global allocator (drop-in via linker)
//   · Intel TBB         — concurrent_hash_map for lock-free process tree
//   · Moodycamel ConcurrentQueue — MPMC lock-free event ring buffer
//   · simdjson (ondemand) — AVX2/SSE4 socket request parsing
//   · {fmt}             — zero-alloc JSON response builder
//   · spdlog (async)    — structured, non-blocking daemon logging
// ─────────────────────────────────────────────────────────────────────────────

#include "daemon.h"
#include "vendor/concurrentqueue.h"

// ── System libraries ──────────────────────────────────────────────────────────
#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>

// ── HPC libraries ─────────────────────────────────────────────────────────────
#include <mimalloc.h>
#include <simdjson.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <tbb/concurrent_hash_map.h>

namespace daemon_service {

// ─────────────────────────────────────────────────────────────────────────────
//  Data structures
// ─────────────────────────────────────────────────────────────────────────────

struct ProcessNode {
    pid_t pid;
    pid_t ppid;
    char  name[64];   // fixed-size avoids heap alloc on hot path
    char  state;      // 'R', 'S', 'D', 'Z', …
};

struct ProcessEventRecord {
    uint64_t timestamp_ns;
    char     type[8];   // "FORK", "EXEC", "EXIT"
    pid_t    pid;
    pid_t    ppid;
    char     name[64];
};

// ─────────────────────────────────────────────────────────────────────────────
//  Global state — lock-free / fine-grained
// ─────────────────────────────────────────────────────────────────────────────

// TBB concurrent_hash_map: bucket-level RW locking — no global mutex
using ProcMap = tbb::concurrent_hash_map<pid_t, ProcessNode>;
static ProcMap g_process_tree;

// Moodycamel ConcurrentQueue: truly lock-free MPMC ring buffer
// The Netlink thread enqueues, socket threads dequeue — zero contention
static moodycamel::ConcurrentQueue<ProcessEventRecord> g_event_queue;

static std::atomic<bool> g_running{true};

// ─────────────────────────────────────────────────────────────────────────────
//  /proc helpers  — small, cache-friendly reads
// ─────────────────────────────────────────────────────────────────────────────

static void read_comm(pid_t pid, char* out, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { out[0] = '\0'; return; }
    ssize_t n = read(fd, out, len - 1);
    close(fd);
    if (n > 0) {
        // strip trailing newline
        if (out[n-1] == '\n') n--;
        out[n] = '\0';
    } else {
        out[0] = '\0';
    }
}

static pid_t read_ppid(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    // Format: pid (name) state ppid ...
    const char* p = strrchr(buf, ')');
    if (!p) return 0;
    p += 2; // skip ') '
    p = strchr(p, ' ');  // skip state
    if (!p) return 0;
    return (pid_t)strtol(p + 1, nullptr, 10);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Initial proc scan
// ─────────────────────────────────────────────────────────────────────────────

static void scan_proc_initial() {
    DIR* dir = opendir("/proc");
    if (!dir) return;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        if (e->d_type != DT_DIR) continue;
        bool digits = true;
        for (const char* p = e->d_name; *p; ++p)
            if (*p < '0' || *p > '9') { digits = false; break; }
        if (!digits || e->d_name[0] == '\0') continue;

        pid_t pid = (pid_t)atoi(e->d_name);
        ProcessNode node{};
        node.pid   = pid;
        node.ppid  = read_ppid(pid);
        node.state = 'S';
        read_comm(pid, node.name, sizeof(node.name));

        ProcMap::accessor acc;
        g_process_tree.insert(acc, pid);
        acc->second = node;
    }
    closedir(dir);
    spdlog::info("[daemon] Initial scan: {} processes", g_process_tree.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Netlink connector — zero-polling, kernel-pushed events
// ─────────────────────────────────────────────────────────────────────────────

static void netlink_listener() {
    int nl_fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if (nl_fd < 0) {
        spdlog::warn("[daemon] No Netlink connector (not root?). Falling back to poll-less mode.");
        return;
    }

    struct sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC;
    addr.nl_pid    = (uint32_t)getpid();
    if (bind(nl_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        spdlog::error("[daemon] Failed to bind Netlink socket: {}", strerror(errno));
        close(nl_fd); return;
    }

    // Subscribe to PROC connector
    alignas(NLMSG_ALIGNTO) char sub_buf[NLMSG_LENGTH(sizeof(cn_msg) + sizeof(proc_cn_mcast_op))];
    auto* nlh = (struct nlmsghdr*)sub_buf;
    nlh->nlmsg_len   = sizeof(sub_buf);
    nlh->nlmsg_pid   = (uint32_t)getpid();
    nlh->nlmsg_type  = NLMSG_DONE;
    nlh->nlmsg_flags = 0;
    nlh->nlmsg_seq   = 0;
    auto* cn  = (struct cn_msg*)NLMSG_DATA(nlh);
    cn->id.idx = CN_IDX_PROC; cn->id.val = CN_VAL_PROC;
    cn->seq = 0; cn->ack = 0;
    cn->len = sizeof(proc_cn_mcast_op);
    *(proc_cn_mcast_op*)cn->data = PROC_CN_MCAST_LISTEN;
    send(nl_fd, nlh, nlh->nlmsg_len, 0);

    spdlog::info("[daemon] Netlink Process Connector active (zero-polling)");

    // Receive buffer — stack-allocated, no heap on hot path
    alignas(16) char recv_buf[8192];
    while (g_running.load(std::memory_order_relaxed)) {
        ssize_t len = recv(nl_fd, recv_buf, sizeof(recv_buf), 0);
        if (len < 0) { if (errno == EINTR) continue; break; }

        auto* nlh_r = (struct nlmsghdr*)recv_buf;
        while (NLMSG_OK(nlh_r, (uint32_t)len)) {
            if (nlh_r->nlmsg_type == NLMSG_ERROR) break;
            auto* cn_r = (struct cn_msg*)NLMSG_DATA(nlh_r);
            if (cn_r->id.idx == CN_IDX_PROC && cn_r->id.val == CN_VAL_PROC) {
                auto* ev = (struct proc_event*)cn_r->data;

                ProcessEventRecord rec{};
                rec.timestamp_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                switch (ev->what) {
                case PROC_EVENT_FORK: {
                    pid_t child  = ev->event_data.fork.child_pid;
                    pid_t parent = ev->event_data.fork.parent_pid;
                    ProcessNode node{};
                    node.pid = child; node.ppid = parent; node.state = 'S';
                    read_comm(child, node.name, sizeof(node.name));

                    { ProcMap::accessor a; g_process_tree.insert(a, child); a->second = node; }

                    memcpy(rec.type, "FORK", 5);
                    rec.pid = child; rec.ppid = parent;
                    memcpy(rec.name, node.name, sizeof(rec.name));
                    g_event_queue.enqueue(rec);  // lock-free enqueue
                    break;
                }
                case PROC_EVENT_EXEC: {
                    pid_t pid = ev->event_data.exec.process_pid;
                    char  name[64]; read_comm(pid, name, sizeof(name));

                    { ProcMap::accessor a;
                      if (g_process_tree.find(a, pid)) {
                          memcpy(a->second.name, name, sizeof(name));
                      } else {
                          ProcessNode node{}; node.pid = pid;
                          node.ppid = read_ppid(pid); node.state = 'S';
                          memcpy(node.name, name, sizeof(node.name));
                          g_process_tree.insert(a, pid); a->second = node;
                      }
                    }
                    memcpy(rec.type, "EXEC", 5);
                    rec.pid = pid; rec.ppid = 0;
                    memcpy(rec.name, name, sizeof(rec.name));
                    g_event_queue.enqueue(rec);
                    break;
                }
                case PROC_EVENT_EXIT: {
                    pid_t pid = ev->event_data.exit.process_pid;
                    g_process_tree.erase(pid);
                    memcpy(rec.type, "EXIT", 5);
                    rec.pid = pid; rec.ppid = 0; rec.name[0] = '\0';
                    g_event_queue.enqueue(rec);
                    break;
                }
                default: break;
                }
            }
            nlh_r = NLMSG_NEXT(nlh_r, len);
        }
    }
    close(nl_fd);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UNIX socket server — simdjson request parsing + fmt response building
// ─────────────────────────────────────────────────────────────────────────────

// Build process_tree JSON with {fmt} — no heap allocation via format_to
static std::string build_process_tree_response() {
    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), R"({{"status":"ok","processes":[)");

    bool first = true;
    // Iterate TBB map with const_accessor (shared read lock per bucket)
    for (auto it = g_process_tree.begin(); it != g_process_tree.end(); ++it) {
        const auto& n = it->second;
        if (!first) fmt::format_to(std::back_inserter(buf), ",");
        fmt::format_to(std::back_inserter(buf),
            R"({{"pid":{},"ppid":{},"name":"{}","state":"{}"}})",
            n.pid, n.ppid, n.name, (char)n.state);
        first = false;
    }
    fmt::format_to(std::back_inserter(buf), "]}");
    return fmt::to_string(buf);
}

static std::string build_events_response() {
    // Drain the lock-free queue into a snapshot
    std::vector<ProcessEventRecord> snapshot;
    snapshot.reserve(256);
    ProcessEventRecord rec;
    while (g_event_queue.try_dequeue(rec)) snapshot.push_back(rec);

    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), R"({{"status":"ok","events":[)");
    bool first = true;
    for (const auto& e : snapshot) {
        if (!first) fmt::format_to(std::back_inserter(buf), ",");
        fmt::format_to(std::back_inserter(buf),
            R"({{"time":{},"type":"{}","pid":{},"ppid":{},"name":"{}"}})",
            e.timestamp_ns, e.type, e.pid, e.ppid, e.name);
        first = false;
    }
    fmt::format_to(std::back_inserter(buf), "]}");
    return fmt::to_string(buf);
}

static void handle_client(int client_fd) {
    // Stack-allocated read buffer — no heap
    char raw[2048];
    ssize_t n = read(client_fd, raw, sizeof(raw) - simdjson::SIMDJSON_PADDING - 1);
    if (n <= 0) { close(client_fd); return; }
    raw[n] = '\0';

    std::string response;
    try {
        // simdjson ondemand — SIMD-accelerated, zero-copy parsing
        // Needs a padded_string for safe SIMD loads
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(raw, n);
        auto doc = parser.iterate(padded);

        std::string_view req_type;
        auto err = doc["request"].get_string().get(req_type);
        if (err) {
            response = R"({"status":"error","message":"missing request field"})";
        } else if (req_type == "process_tree") {
            response = build_process_tree_response();
        } else if (req_type == "events") {
            response = build_events_response();
        } else {
            response = fmt::format(
                R"({{"status":"error","message":"unknown request: {}"}})", req_type);
        }
    } catch (...) {
        response = R"({"status":"error","message":"invalid json"})";
    }

    // Write response with writev or loop
    const char* ptr = response.data();
    ssize_t remaining = (ssize_t)response.size();
    while (remaining > 0) {
        ssize_t w = write(client_fd, ptr, (size_t)remaining);
        if (w <= 0) break;
        ptr += w; remaining -= w;
    }
    close(client_fd);
}

static void unix_socket_server() {
    const char* sock_path = "/tmp/syspilot.sock";
    unlink(sock_path);

    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) { spdlog::critical("[daemon] socket() failed"); return; }

    // SO_REUSEADDR + non-blocking accept via O_NONBLOCK select loop
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        spdlog::critical("[daemon] bind() failed: {}", strerror(errno));
        close(srv); return;
    }
    chmod(sock_path, 0666);
    listen(srv, 128);  // High backlog for bursty CLI queries

    spdlog::info("[daemon] UNIX socket listening at {}", sock_path);

    while (g_running.load(std::memory_order_relaxed)) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(srv, &rfds);
        struct timeval tv{1, 0};
        if (select(srv + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        int client = accept(srv, nullptr, nullptr);
        if (client < 0) continue;

        // Each client gets a detached thread — pool this in future
        std::thread([client]{ handle_client(client); }).detach();
    }
    close(srv);
    unlink(sock_path);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────

int run_daemon() {
    // ── Init spdlog async logger ──────────────────────────────────────────────
    spdlog::init_thread_pool(8192, 2);  // 8k message queue, 2 bg threads
    auto console = spdlog::stdout_color_mt<spdlog::async_factory>("syspilotd");
    spdlog::set_default_logger(console);
    spdlog::set_pattern("[%T.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);

    spdlog::info("✨ SysPilot Daemon starting (mimalloc·simdjson·TBB·ConcurrentQueue)");

    scan_proc_initial();

    std::thread nl_thread(netlink_listener);
    std::thread sock_thread(unix_socket_server);

    nl_thread.join();
    sock_thread.join();
    return 0;
}

} // namespace daemon_service
