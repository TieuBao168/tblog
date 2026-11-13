#!/bin/sh
# Compile-time stripping check: build tests/strip_probe.c at several -O levels
# and configurations, then inspect the object with strings(1) and nm(1).
# Usage: check_strip.sh <cc> <outdir> [extra cflags...]
set -u
CC=$1
OUT=$2
shift 2
CFLAGS="-std=c99 -Wall -Wextra -pedantic -Werror -Iinclude $*"
mkdir -p "$OUT"
fails=0

# Only what would land in the flash/RAM image: read-only and data sections
# (the object's STT_FILE symbol always names the source file; ignore it).
has() {
    objcopy -O binary --only-section='.rodata*' --only-section='.data*' "$1" "$1.bin" || exit 1
    strings -a "$1.bin" | grep -q "$2"
}
sym() { nm "$1" | grep -q "$2"; }
expect() { # expect <yes|no> <what> <cmd...>
    want=$1; what=$2; shift 2
    if "$@"; then got=yes; else got=no; fi
    if [ "$got" != "$want" ]; then
        echo "FAIL [$cfg]: expected $what present=$want"
        fails=$((fails + 1))
    fi
}

for opt in -O0 -Os -O2; do
    cfg="$opt COMPILE_LEVEL=WARN"
    o="$OUT/strip$opt.o"
    $CC $CFLAGS $opt -DTBLOG_COMPILE_LEVEL=TBLOG_LV_WARN -c tests/strip_probe.c -o "$o" || exit 1
    expect no  "STRIPPED_* strings"     has "$o" STRIPPED_
    expect no  "stripped_helper_fn sym" sym "$o" stripped_helper_fn
    expect yes "KEPT_WARN_MARKER"       has "$o" KEPT_WARN_MARKER
    expect yes "KEPT_MODULE_MARKER"     has "$o" KEPT_MODULE_MARKER
    expect yes "kept_helper_fn sym"     sym "$o" kept_helper_fn
    expect yes "file name"              has "$o" strip_probe.c

    cfg="$opt FILELINE=0 MODULE=0"
    o="$OUT/strip$opt-nofile.o"
    $CC $CFLAGS $opt -DTBLOG_COMPILE_LEVEL=TBLOG_LV_WARN -DTBLOG_LITE_FILELINE=0 -DTBLOG_LITE_MODULE=0 \
        -c tests/strip_probe.c -o "$o" || exit 1
    expect no  "file name"          has "$o" strip_probe.c
    expect no  "KEPT_MODULE_MARKER" has "$o" KEPT_MODULE_MARKER
    expect yes "KEPT_ERROR_MARKER"  has "$o" KEPT_ERROR_MARKER

    cfg="$opt COMPILE_LEVEL=OFF"
    o="$OUT/strip$opt-off.o"
    $CC $CFLAGS $opt -DTBLOG_COMPILE_LEVEL=TBLOG_LV_OFF -c tests/strip_probe.c -o "$o" || exit 1
    expect no "any marker"           has "$o" _MARKER
    expect no "tblog_lite_write sym" sym "$o" tblog_lite_write
done

if [ $fails -ne 0 ]; then
    echo "check_strip: FAILED ($fails)"
    exit 1
fi
echo "check_strip: ok (-O0/-Os/-O2: strings and symbols below TBLOG_COMPILE_LEVEL absent)"
