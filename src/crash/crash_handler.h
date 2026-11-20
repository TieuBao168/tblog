// Crash handling — src/crash/crash_handler.cpp (what is reported) on top of
// pf::crash_install (src/platform/<os>/crash.cpp: signal handlers, alternate
// stacks, backtrace, re-raise).
// Installs SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL handlers on an alternate
// stack. On signal: "*** tblog: caught signal N (NAME) ***\n" to stderr, then
// backend->on_crash() (drain buffered lines first), then
// backend->crash_write(same line), backtrace to stderr, then restore default
// handler and re-raise so a core dump is still produced. backend may be
// replaced by calling install again; nullptr disables the backend part.
#pragma once

namespace tblog {

class Backend;

void install_crash_handler(Backend *backend);
void uninstall_crash_handler();
// Gives the calling thread its own alternate signal stack (mmap'd, freed at
// thread exit) so a stack overflow on that thread is still reported.
bool crash_altstack_for_this_thread();

}  // namespace tblog
