#ifndef DAEMON_H
#define DAEMON_H

namespace daemon_service {
// Starts the syspilotd daemon listening to netlink and UNIX sockets
int run_daemon();
}

#endif // DAEMON_H
