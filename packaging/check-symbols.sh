#!/bin/sh
# Usage: check-symbols.sh libtblog.a libtblog.so.X.Y.Z
# Fails unless the shared library exports exactly the global tblog_* functions
# defined in the static archive (a public function missing TBLOG_API in the
# header is hidden by -fvisibility=hidden and shows up here as "missing").
set -eu
NM=${NM:-nm}
archive=$1
shlib=$2
tmp=${TMPDIR:-/tmp}/tblog-syms.$$
trap 'rm -f "$tmp".a "$tmp".so' EXIT

# Global (upper-case type) tblog_* definitions in the archive.
"$NM" --defined-only "$archive" 2>/dev/null |
    awk 'NF == 3 && $2 ~ /^[A-Z]$/ && $3 ~ /^tblog_/ { print $3 }' | sort -u > "$tmp".a
# Everything the .so exports.
"$NM" -D --defined-only "$shlib" | awk 'NF >= 3 { print $3 }' | sort -u > "$tmp".so

status=0
extra=$(grep -v '^tblog_' "$tmp".so || true)
if [ -n "$extra" ]; then
    echo "check-symbols: $shlib exports non-tblog symbols:" >&2
    echo "$extra" | sed 's/^/  /' >&2
    status=1
fi
missing=$(comm -23 "$tmp".a "$tmp".so)
if [ -n "$missing" ]; then
    echo "check-symbols: defined in $archive but not exported (add TBLOG_API?):" >&2
    echo "$missing" | sed 's/^/  /' >&2
    status=1
fi
if [ ! -s "$tmp".so ]; then
    echo "check-symbols: $shlib exports nothing" >&2
    status=1
fi
[ $status -eq 0 ] && echo "check-symbols: OK, $(wc -l < "$tmp".so) exported: $(tr '\n' ' ' < "$tmp".so)"
exit $status
