#pragma once

// Prints a backtrace to stderr on SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT, then lets the
// signal kill the process as usual (exit status and core dumps are unchanged).
//
// The firmware runs on a chip where address 0 is readable memory, so core code that reads
// through a null pointer works there and crashes here; the trace shows where.
void installCrashHandler();
