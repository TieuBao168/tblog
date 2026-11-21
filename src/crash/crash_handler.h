// Crash handling — src/crash/crash_handler.cpp (what is reported) on top of
// pf::crash_install (src/platform/<os>/crash.cpp: signal handlers, alternate
// stacks, backtrace, re-raise).
// Installs SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL handlers on an alternate
// stack. On signal: "*** tblog: caught signal N (NAME) ***\n" to stderr
// (unless a crash-safe sink of the pipeline writes to stderr), then
// pipeline->crash(same line): the queue's buffered lines are drained into
// every crash-safe sink (FileSink, ConsoleSink), in sink order, followed by
// the marker. Then backtrace to stderr, restore the default handler and
// re-raise so a core dump is still produced. The pipeline may be replaced by
// calling install again; nullptr disables the pipeline part.
#pragma once

namespace tblog {

class Pipeline;

void install_crash_handler(Pipeline *pipeline);
void uninstall_crash_handler();
// Gives the calling thread its own alternate signal stack (mmap'd, freed at
// thread exit) so a stack overflow on that thread is still reported.
bool crash_altstack_for_this_thread();

}  // namespace tblog
