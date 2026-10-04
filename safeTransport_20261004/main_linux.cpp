// Linux/POSIX entry point example — pBMP280 + KR260I2cTransport
// (swap for WiringPiI2cTransport if that's the target instead).
//
// Usage:
//   ./sensor_app       runs in the foreground; Ctrl-C (SIGINT) or
//                       `kill <pid>` (SIGTERM) stops it cleanly.
//   ./sensor_app -b    forks into the background and returns control
//                       of the shell immediately; the sensor keeps
//                       running in a detached worker process. Stop
//                       it later with `kill <pid>` (the pid printed
//                       when it started — that's the WORKER's pid,
//                       not the short-lived supervisor's).
//
// -b uses a double fork with an explicit SIGCHLD-reaping supervisor,
// not a bare single fork(). This was NOT the original design — it
// changed after empirically finding a real problem: a single fork()
// leaves the eventual zombie-reaping entirely up to whatever the
// system's PID 1 happens to be. On a full init system (systemd, etc.)
// that's automatic and fine. Tested against this environment's PID 1,
// it was NOT: the worker correctly received SIGTERM and exited
// (confirmed via /proc's State: S -> Z transition), but nothing ever
// reaped it, leaving a permanent zombie. That's a known real-world
// container pitfall too, not just a sandbox artifact — running this
// as a container's own PID 1 (or under a shell script as PID 1) has
// the same failure mode. The fix: don't rely on inheriting a
// trustworthy init. Reap our own child, ourselves, explicitly.
//
// Structure:
//   original process -- fork() --> [exits immediately: returns the shell prompt]
//                             `-> supervisor -- fork() --> worker (the actual sensor)
//                                     |
//                                     `-- installs SIGCHLD handler BEFORE the second
//                                         fork(), so it can't miss the notification;
//                                         waits for it, then waitpid()s the worker
//                                         explicitly — reaping it itself, regardless
//                                         of what PID 1 does or doesn't do.
//
// Still not a FULL Unix daemon even with this fix:
//   - No setsid(): stays in the same session/controlling terminal as
//     the shell that launched it. Closing that terminal can still
//     deliver SIGHUP.
//   - No chdir("/"): keeps holding a reference to the launching
//     directory, which can block unmounting that filesystem.
// stdin/stdout/stderr redirection to /dev/null IS included below,
// though, in both the supervisor and the worker — confirmed
// empirically last time that omitting this hangs anything waiting to
// read EOF on those (a calling script's command substitution, a test
// harness capturing output) for as long as either process keeps
// running. The supervisor prints the worker's pid BEFORE redirecting
// its own stdio — reversing that order would make the pid message
// invisible to whoever's waiting to read it.
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <sys/wait.h>
#include <atomic>
#include <semaphore.h>
#include "pBMP280.h"
#include "KR260I2cTransport.h"

namespace {
    std::atomic<bool> g_childExited{false};

    // Signal-handler-safe: only a plain, lock-free atomic store,
    // nothing else — same convention as pBMP280's own signal handler.
    void handleSigchld(int /*signum*/) {
        g_childExited.store(true, std::memory_order_relaxed);
    }

    void redirectStdioToDevNull() {
        const int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) close(devNull);
        }
    }

    // The actual sensor code — runs in the worker process (background
    // mode) or directly in this process (foreground mode). Blocks
    // until SIGTERM/SIGINT, then returns.
    void runSensor() {
        // Install signal handling BEFORE constructing any pBMP280 —
        // see pBMP280::installDefaultSignalHandlers()'s own comment
        // for why this is opt-in rather than automatic.
        pBMP280::installDefaultSignalHandlers();

        sem_t busSem;
        sem_init(&busSem, /*pshared=*/0, /*value=*/1);

        KR260I2cTransport bmpBus("/dev/i2c-1", 0x76, &busSem);
        pBMP280 bmp(bmpBus);

        bmp.begin(); // starts bmp's own dedicated pthread

        // Keep this process alive without busy-polling: pause() blocks
        // until ANY signal is handled, then returns — so this loop
        // just re-checks signalReceived() and, if it's still false
        // (some other signal woke us), goes back to sleep. Once
        // SIGTERM/SIGINT actually arrives, the loop exits, this
        // function returns, and bmp's destructor runs (StopThread() +
        // pthread_join()) before the process actually exits.
        while (!pBMP280::signalReceived()) {
            pause();
        }

        sem_destroy(&busSem);
    }
}

int main(int argc, char** argv) {
    bool background = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-b") == 0) background = true;
    }

    if (!background) {
        runSensor();
        return 0;
    }

    // --- Background mode: double fork + supervisor ---

    const pid_t supervisorPid = fork();
    if (supervisorPid < 0) {
        std::perror("fork");
        return 1;
    }
    if (supervisorPid > 0) {
        // Original process: exit immediately. THIS is what actually
        // returns control of the shell to the user right away.
        return 0;
    }

    // We are the supervisor now. Install SIGCHLD handling BEFORE the
    // second fork() below, so a very-fast-exiting worker can't
    // terminate before we're listening for it.
    struct sigaction sa{};
    sa.sa_handler = &handleSigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP; // only notify on termination, not stop/continue
    sigaction(SIGCHLD, &sa, nullptr);

    const pid_t workerPid = fork();
    if (workerPid < 0) {
        std::perror("fork");
        return 1;
    }

    if (workerPid > 0) {
        // Supervisor: print the WORKER's pid — that's what the user
        // should `kill` later, not this supervisor's own pid — THEN
        // detach our own stdio, and finally settle in to wait for the
        // worker to exit so we can reap it ourselves.
        std::printf("started in background, pid=%d\n", workerPid);
        std::fflush(stdout);
        redirectStdioToDevNull();

        while (!g_childExited.load()) {
            pause();
        }
        int status = 0;
        waitpid(workerPid, &status, 0); // reaps the worker — no zombie left behind
        return 0;
    }

    // We are the worker now — this is where the actual sensor runs.
    redirectStdioToDevNull();
    runSensor();
    return 0;
}
