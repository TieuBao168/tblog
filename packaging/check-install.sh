#!/bin/sh
# Driven by `make check-install`. Installs tblog into $BUILD/check-install,
# then builds examples/basic.c against it through pkg-config:
#   (a) static:  pkg-config --static --cflags --libs, -ltblog -> -l:libtblog.a
#   (b) shared:  pkg-config --cflags --libs, run with LD_LIBRARY_PATH
# and runs both. Also checks a DESTDIR staged install + uninstall.
# Falls back to a tiny built-in .pc reader if pkg-config is not installed.
set -eu
: "${BUILD:?}" "${MAKE:=make}" "${CC:=cc}" "${CFLAGS:=}" "${LDFLAGS:=}" "${RUNNER:=}"
: "${PKG_CONFIG:=pkg-config}"

top=$(pwd)
root=$top/$BUILD/check-install
prefix=$root/usr
work=$root/work
rm -rf "$root"
mkdir -p "$work"

say() { printf '=== check-install: %s\n' "$*"; }
fail() { printf 'check-install: FAIL: %s\n' "$*" >&2; exit 1; }

# ---- pkg-config (or a minimal stand-in) -------------------------------------
pc_builtin() {
    static=0 cflags=0 libs=0 var= name=
    for a in "$@"; do
        case $a in
            --static) static=1 ;;
            --cflags) cflags=1 ;;
            --libs) libs=1 ;;
            --variable=*) var=${a#--variable=} ;;
            -*) ;;
            *) name=$a ;;
        esac
    done
    file=$PKG_CONFIG_PATH/$name.pc
    [ -f "$file" ] || { echo "no $file" >&2; return 1; }
    awk -v static=$static -v cflags=$cflags -v libs=$libs -v var="$var" '
        function expand(s,   k) {
            while (match(s, /\$\{[A-Za-z0-9_.]+\}/)) {
                k = substr(s, RSTART + 2, RLENGTH - 3)
                s = substr(s, 1, RSTART - 1) v[k] substr(s, RSTART + RLENGTH)
            }
            return s
        }
        /^[A-Za-z0-9_.]+=/ { i = index($0, "="); v[substr($0, 1, i - 1)] = expand(substr($0, i + 1)); next }
        /^[A-Za-z0-9_.]+:/ { i = index($0, ":"); val = substr($0, i + 1); sub(/^[ \t]+/, "", val)
                             f[substr($0, 1, i - 1)] = expand(val) }
        END {
            if (var != "") { print v[var]; exit }
            out = ""
            if (cflags) out = f["Cflags"]
            if (libs) { out = out " " f["Libs"]; if (static) out = out " " f["Libs.private"] }
            print out
        }' "$file"
}
PKG_CONFIG_PATH=$prefix/lib/pkgconfig
export PKG_CONFIG_PATH
if command -v "$PKG_CONFIG" >/dev/null 2>&1; then
    pc() { "$PKG_CONFIG" "$@"; }
    say "using $PKG_CONFIG"
else
    pc() { pc_builtin "$@"; }
    say "$PKG_CONFIG not found, using the built-in .pc reader"
fi

# ---- staged install (DESTDIR) + uninstall ------------------------------------
say "DESTDIR install/uninstall"
stage=$root/stage
$MAKE --no-print-directory install PREFIX=/opt/tblog DESTDIR="$stage" >/dev/null
for f in include/tblog/log.h lib/libtblog.a lib/libtblog.so.0.1.0 lib/libtblog.so.0 \
         lib/libtblog.so lib/pkgconfig/tblog.pc; do
    [ -e "$stage/opt/tblog/$f" ] || fail "DESTDIR install is missing $f"
done
grep -qx 'prefix=/opt/tblog' "$stage/opt/tblog/lib/pkgconfig/tblog.pc" || fail "bad prefix in staged tblog.pc"
$MAKE --no-print-directory uninstall PREFIX=/opt/tblog DESTDIR="$stage" >/dev/null
left=$(find "$stage" -type f -o -type l)
[ -z "$left" ] || fail "uninstall left files behind: $left"

# ---- real install into $prefix -----------------------------------------------
say "install PREFIX=$prefix"
$MAKE --no-print-directory install PREFIX="$prefix" >/dev/null
[ "$(readlink "$prefix/lib/libtblog.so")" = libtblog.so.0 ] || fail "libtblog.so symlink"
[ "$(readlink "$prefix/lib/libtblog.so.0")" = libtblog.so.0.1.0 ] || fail "libtblog.so.0 symlink"
say "pkg-config --cflags --libs tblog:          $(pc --cflags --libs tblog)"
say "pkg-config --static --cflags --libs tblog: $(pc --static --cflags --libs tblog)"

conf=$work/test.conf
run_example() {  # $1 = binary, $2 = log file; extra env is passed by caller
    printf 'backend = buffer\nfile.path = %s\nreload.watch = false\n' "$2" > "$conf"
    (cd "$work" && $RUNNER "$1" "$conf") || fail "$1 exited with $?"
    grep -q 'started, pid-independent message hello' "$2" || fail "$2 lacks the expected line"
    n=$(grep -c 'worker . step' "$2")
    [ "$n" -eq 4000 ] || fail "$2 has $n worker lines, expected 4000"
}

# ---- (a) static --------------------------------------------------------------
say "(a) static link"
# With both libtblog.a and libtblog.so in libdir, -ltblog picks the .so; ask
# the linker for the archive by name.
static_flags=$(pc --static --cflags --libs tblog | sed 's/-ltblog\b/-l:libtblog.a/')
# shellcheck disable=SC2086
$CC $CFLAGS "$top/examples/basic.c" $static_flags $LDFLAGS -o "$work/basic_static"
if readelf -d "$work/basic_static" | grep -q 'NEEDED.*libtblog'; then
    fail "basic_static depends on libtblog.so"
fi
run_example "$work/basic_static" "$work/static.log"

# ---- (b) shared --------------------------------------------------------------
say "(b) shared link"
# shellcheck disable=SC2086
$CC $CFLAGS "$top/examples/basic.c" $(pc --cflags --libs tblog) $LDFLAGS -o "$work/basic_shared"
readelf -d "$work/basic_shared" | grep -q 'NEEDED.*\[libtblog\.so\.0\]' ||
    fail "basic_shared does not depend on libtblog.so.0"
LD_LIBRARY_PATH=$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_LIBRARY_PATH
run_example "$work/basic_shared" "$work/shared.log"
ldd "$work/basic_shared" | grep -q "$prefix/lib/libtblog.so.0" || fail "loader did not pick the installed .so"

say "OK (static + shared examples built from the installed tree and ran)"
