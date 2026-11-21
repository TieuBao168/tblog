// Crash handling — src/crash/crash_handler.cpp (what is reported) on top of
// pf::crash_install (src/platform/<os>/crash.cpp: signal handlers / exception
// filter, alternate stacks, backtrace, re-raise / chaining).
// POSIX: SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL handlers on an alternate
// stack. Windows: an unhandled-exception filter (access violation, illegal
// instruction, divide by zero, stack overflow, ...) and a SIGABRT handler.
// On a crash: "*** tblog: caught signal N (NAME) ***\n" (Windows exceptions:
// "*** tblog: caught exception 0xC0000005 (EXCEPTION_ACCESS_VIOLATION) ***\n")
// to stderr (unless a crash-safe sink of the pipeline writes to stderr), then
// pipeline->crash(same line): the queue's buffered lines are drained into
// every crash-safe sink (FileSink, ConsoleSink), in sink order, followed by
// the marker; lines that other threads log from then on are written by those
// threads themselves, synchronously, after the marker (Queue::append). Then
// backtrace to stderr and the previous handler / default action, so a core
// dump / WER report is still produced. The pipeline may be replaced by
// calling install again; nullptr disables the pipeline part.
#pragma once

namespace tblog {

class Pipeline;

void install_crash_handler(Pipeline *pipeline);
void uninstall_crash_handler();
// Prepares the calling thread so that a stack overflow on it is still
// reported: its own alternate signal stack (POSIX, mmap'd, freed at thread
// exit) / SetThreadStackGuarantee (Windows).
bool crash_altstack_for_this_thread();

}  // namespace tblog
