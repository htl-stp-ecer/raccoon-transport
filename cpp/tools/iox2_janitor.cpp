// iox2_janitor — periodically reap dead iceoryx2 nodes.
//
// iceoryx2 is a *daemon-less* IPC stack: there is no central RouDi-style
// broker, every process is supposed to clean its own state on exit. In
// practice that means orphan node descriptors stay under /tmp/iceoryx2/
// whenever a process crashed, was killed, or systemd-restarted without
// running its destructors — and a future Node::create() then runs into
// "OpenIsMarkedForDestruction" / corrupted-service races.
//
// This binary fills the gap: it runs as a long-lived systemd service
// owned by the same uid as the other iox2 users on the host, and calls
// iox2's own `try_cleanup_dead_nodes()` on a tick. That API walks
// /tmp/iceoryx2/nodes/, identifies node-ids whose owner-pid no longer
// exists, and removes both the node entry and every service tag the
// dead node owned. It is the same primitive raccoon::Transport calls
// at startup — moving it into a continuous loop here means the *next*
// open in any raccoon process almost always finds a clean state.
//
// Design notes:
//   * No iox2 Node is constructed here. cleanup_dead_nodes_of_services
//     is a static method that only needs a ConfigView.
//   * Sleep dominates the wall clock; CPU is negligible (< 0.1 %).
//   * We log only when cleanup actually did something — quiet on a
//     healthy system, loud when a heal happened so the operator can
//     correlate it with a crash.
//   * SIGINT / SIGTERM exits cleanly so `systemctl stop` is fast.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include <sys/stat.h>

#include "iox2/iceoryx2.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int /*sig*/) {
    g_stop.store(true, std::memory_order_relaxed);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// Cheap stat-based change detector. Each iox2::Node::try_cleanup_dead_nodes
// internally walks every entry under /tmp/iceoryx2/services/ — measured at
// ~1k syscalls and ~0.5 s of CPU per sweep on a Pi 3B when a stuck node is
// present. Calling that on a fixed period regardless of state burns CPU
// even when nothing has changed since the previous sweep.
//
// The directory mtime advances whenever an iox2 process registers a node,
// dies, or another sweep removes an orphan. We snapshot it after every
// sweep and skip the next sweep when it has not advanced — turning the
// steady-state cost from "0.5 s CPU per period" into a single stat() call.
constexpr const char* kNodesDir = "/tmp/iceoryx2/nodes";

struct DirMtime {
    long sec{0};
    long nsec{0};
    bool valid{false};

    bool operator==(const DirMtime& rhs) const {
        return valid == rhs.valid && sec == rhs.sec && nsec == rhs.nsec;
    }
};

DirMtime readNodesMtime() {
    struct stat st{};
    if (::stat(kNodesDir, &st) != 0) return DirMtime{0, 0, false};
    return DirMtime{
        static_cast<long>(st.st_mtim.tv_sec),
        static_cast<long>(st.st_mtim.tv_nsec),
        true};
}

DirMtime g_last_mtime{};

// One sweep. Reports back true if it logged so the caller can flush.
bool sweep(bool force) {
    const auto mtime = readNodesMtime();
    if (!force && mtime == g_last_mtime) {
        // Nothing has changed since we last looked — no node was registered,
        // none died, nobody else swept. Skip the expensive cleanup call.
        return false;
    }

    try {
        auto state = iox2::Node<iox2::ServiceType::Ipc>::try_cleanup_dead_nodes(
            iox2::Config::global_config());
        // Re-read mtime after the sweep: try_cleanup_dead_nodes itself
        // bumps the dir mtime when it removes entries, so storing the
        // *post-sweep* value is what makes the change-detector idle on
        // a quiet system.
        g_last_mtime = readNodesMtime();

        if (state.cleanups > 0 || state.failed_cleanups > 0) {
            std::cerr << "iox2-janitor: reaped " << state.cleanups
                      << " dead node(s), skipped " << state.failed_cleanups
                      << " (locked / no permission)\n";
            std::cerr.flush();
            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "iox2-janitor: sweep failed: " << e.what() << "\n";
        std::cerr.flush();
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    // Quiet iox2's own debug log unless the operator turned it on via env.
    iox2::set_log_level_from_env_or(iox2::LogLevel::Warn);

    // Sweep cadence (env-override for tests / forensics). 2 s is the
    // sweet spot we measured on a Pi 3B: fast enough that a dead node
    // is gone before the next process's NodeBuilder runs, slow enough
    // that the binary's wakeups don't show up in `top`.
    std::chrono::milliseconds period{2000};
    if (argc >= 2) {
        try {
            const auto ms = std::stoi(argv[1]);
            if (ms > 0) period = std::chrono::milliseconds(ms);
        } catch (...) {
            std::cerr << "iox2-janitor: usage: " << argv[0]
                      << " [period_ms]\n";
            return 2;
        }
    }

    install_signal_handlers();

    std::cerr << "iox2-janitor: starting, sweep period "
              << std::chrono::duration_cast<std::chrono::milliseconds>(period).count()
              << " ms\n";

    // Do an initial sweep so a freshly-booted system gets cleaned up
    // before any other raccoon process even tries to open a service.
    // Force=true so the change-detector doesn't skip the very first run
    // (g_last_mtime is uninitialised at this point).
    (void)sweep(/*force=*/true);

    while (!g_stop.load(std::memory_order_relaxed)) {
        // Sleep in short ticks so SIGTERM exits within ~100 ms instead
        // of waiting for the full period.
        auto deadline = std::chrono::steady_clock::now() + period;
        while (std::chrono::steady_clock::now() < deadline) {
            if (g_stop.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_stop.load(std::memory_order_relaxed)) break;
        (void)sweep(/*force=*/false);
    }

    std::cerr << "iox2-janitor: stopping\n";
    return 0;
}
