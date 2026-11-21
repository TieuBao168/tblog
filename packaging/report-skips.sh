#!/bin/sh
# Lists every test case that was skipped in the last `ctest` run of a build
# directory ("[ SKIP ] case (reason)" lines of tests/tiny_test.h), with the
# CTest test it belongs to. Informational: always exits 0 (on Linux CI,
# TBLOG_TEST_NO_SKIP=1 already turns skips into failures).
#
#   sh packaging/report-skips.sh [BUILD_DIR]     (default: build)
#
# Also appends the list to $GITHUB_STEP_SUMMARY when that is set.
build=${1:-build}
log=$build/Testing/Temporary/LastTest.log
if [ ! -f "$log" ]; then
    echo "report-skips: $log not found (ctest did not run?)"
    exit 0
fi
skips=$(tr -d '\r' < "$log" | awk '
    /^[0-9]+\/[0-9]+ Testing: / { t = $3 }
    /\[ SKIP \]/ { sub(/^.*\[ SKIP \] /, ""); print t ": " $0 }')
if [ -z "$skips" ]; then
    echo "report-skips: no skipped test cases"
    summary="No skipped test cases."
else
    n=$(printf '%s\n' "$skips" | wc -l | tr -d ' ')
    echo "report-skips: $n skipped test case(s):"
    printf '%s\n' "$skips" | sed 's/^/  /'
    summary=$(printf 'Skipped test cases (%s):\n\n%s\n' "$n" "$(printf '%s\n' "$skips" | sed 's/^/- /')")
fi
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    printf '### Skipped tests\n\n%s\n' "$summary" >> "$GITHUB_STEP_SUMMARY"
fi
exit 0
