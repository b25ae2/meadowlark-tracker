#include "CrashHandler.h"

#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

namespace
{
void writeString(const char* text)
{
    ssize_t ignored = write(STDERR_FILENO, text, strlen(text));
    (void)ignored;
}

// Only async-signal-safe calls, apart from backtrace(), which glibc and macOS allow here
// once it has been called outside the handler (installCrashHandler does that).
void onCrash(int signal)
{
    writeString("\nmeadowlark: crashed with ");
    writeString(strsignal(signal));
    writeString(", backtrace:\n");

    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    writeString("meadowlark: please report this with the lines above\n");

    // Default action, so the exit status (and any core dump) is the usual one.
    ::signal(signal, SIG_DFL);
    raise(signal);
}
} // namespace

void installCrashHandler()
{
    // The first backtrace() call may load libgcc and allocate; do it now, not in the handler.
    void* warmUp[1];
    backtrace(warmUp, 1);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = onCrash;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (int signal : signals)
        sigaction(signal, &action, nullptr);
}
