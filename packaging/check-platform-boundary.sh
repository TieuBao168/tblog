#!/bin/sh
# Enforce the platform boundary: outside src/platform/, library code must not
# include OS headers or call OS APIs directly; it goes through
# src/platform/platform.h (namespace tblog::pf) instead.
#
#   sh packaging/check-platform-boundary.sh [SRC_DIR]   (default: src)
#
# Exit status 1 and the offending lines on any hit, 0 otherwise.
set -eu

src=${1:-src}

# OS headers (standard C/C++ headers such as <cstdio>, <ctime>, <thread> are fine).
headers='^[[:space:]]*#[[:space:]]*include[[:space:]]*<(unistd\.h|fcntl\.h|sys/[^>]*|signal\.h|dirent\.h|execinfo\.h|syslog\.h|pthread\.h|time\.h|windows\.h|io\.h|direct\.h)>'

# OS calls. `::name(` must not follow an identifier character (so
# Sink::write( and pf::rename_replace( do not match); the bare names below
# do not exist outside the OS APIs. A call is the name directly followed by
# '(' (prose in comments, "fdatasync (it can ...", does not match).
id='(^|[^A-Za-z0-9_])'
qualified="${id}::(open|write|read|close|dup|dup2|fsync|fdatasync|ftruncate|rename|unlink|stat|lstat|fstat|mkdir|opendir|readdir|closedir|isatty|syscall|localtime_r|clock_gettime)\\("
bare="${id}(syscall|sigaction|sigaltstack|gettid|localtime_r|gmtime_r|posix_memalign|clock_gettime|nanosleep|mmap|munmap|fsync|fdatasync|openlog|closelog|backtrace|backtrace_symbols_fd)\\(|SYS_gettid|STDERR_FILENO"

status=0
if grep -rnE --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.c' \
        --exclude-dir=platform "$headers|$qualified|$bare" "$src"; then
    echo "check-platform-boundary: OS-specific code outside $src/platform/ (see above);" >&2
    echo "  move it behind src/platform/platform.h (tblog::pf)." >&2
    status=1
else
    echo "check-platform-boundary: OK (no OS-specific code outside $src/platform/)"
fi
exit $status
