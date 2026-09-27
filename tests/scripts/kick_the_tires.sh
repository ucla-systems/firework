#!/usr/bin/env bash
# Firework kick-the-tires check (~5 minutes).
#
# Verifies that the environment is correctly set up and that the full
# Firework stack (custom kernel + custom glibc + runtime) can execute a
# small 2-process distributed benchmark end to end.
#
# Usage: scripts/kick_the_tires.sh

TESTS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GLIBC_SO="${FW_GLIBC_SO:-$TESTS_ROOT/../glibc/build/libc.so.6}"

PASS=0
FAIL=0
check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        echo "  [PASS] $desc"; PASS=$((PASS+1))
    else
        echo "  [FAIL] $desc"; FAIL=$((FAIL+1))
    fi
}

echo "== 1. Environment checks =="
check "running the Firework kernel (uname -r = $(uname -r))" \
      sh -c 'uname -r | grep -q firework'
check "firework debugfs interface present" \
      sudo test -d /sys/kernel/debug/firework
check "dax namespaces present (ndctl)" \
      sh -c 'ndctl list -N | grep -q namespace0.0'
check "custom glibc built at $GLIBC_SO" \
      test -f "$GLIBC_SO"
check "firework runtime built (libfirework.so)" \
      test -f "$TESTS_ROOT/runtime/build/lib/libfirework.so"

if [ "$FAIL" -gt 0 ]; then
    echo "Environment checks failed ($FAIL). Fix these before running benchmarks."
    exit 1
fi

smoke() {
    local desc="$1"; shift
    echo
    echo "== $desc =="
    if ! "$@"; then
        echo
        echo "== KICK-THE-TIRES: SMOKE TEST FAILED =="
        echo "   Inspect $TESTS_ROOT/results/latest/ for logs."
        exit 1
    fi
}

smoke "2. Smoke test: remote threads + CXL-resident locks (locktest, 2 processes)" \
      "$TESTS_ROOT/scripts/run_one.sh" locktest 2 --timeout 600
smoke "3. Smoke test: demand page sharing/migration (unshare, 2 processes)" \
      "$TESTS_ROOT/scripts/run_one.sh" unshare 2 --timeout 600 --no-setup

# The unshare run must actually have exercised the page-sharing path:
# non-zero page-fault and demand-migration counters, and the allocator must
# have read back the correct data ("Correct!").
echo
pf=$(grep -hoE "Page Faults: [0-9]+" "$TESTS_ROOT"/results/latest/rank*.log | grep -oE "[0-9]+" | sort -n | tail -1)
mig=$(grep -hoE "Migrations \(demand\): [0-9]+" "$TESTS_ROOT"/results/latest/rank*.log | grep -oE "[0-9]+" | sort -n | tail -1)
if [ "${pf:-0}" -gt 0 ] && [ "${mig:-0}" -gt 0 ] && grep -q "Correct!" "$TESTS_ROOT"/results/latest/rank*.log; then
    echo "  [PASS] page sharing exercised: ${pf} page faults, ${mig} demand migrations, data verified"
else
    echo "  [FAIL] expected non-zero page-fault/migration counters and 'Correct!' (got pf=${pf:-0}, mig=${mig:-0})"
    exit 1
fi
echo
echo "== KICK-THE-TIRES: ALL CHECKS PASSED =="
exit 0
