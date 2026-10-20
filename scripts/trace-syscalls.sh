#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Project HYPERION - reproduce the syscall usage evidence behind docs/syscalls/.
#
# Traces a fixed set of representative Linux programs with strace and prints a
# per-syscall call count, highest first. The counts feed the `evidence` column
# of docs/syscalls/inventory.tsv (as `trace:N`).
#
# This is a small, x86-64-host sample. It is a lower bound on real usage, not a
# definitive frequency ranking. Re-run it and update the inventory when adding
# workloads.
#
# Usage: scripts/trace-syscalls.sh [output-dir]
set -u

OUT="${1:-$(mktemp -d)}"
mkdir -p "${OUT}"
COUNTS="${OUT}/raw_counts.txt"
: > "${COUNTS}"

if ! command -v strace >/dev/null 2>&1; then
    echo "error: strace is required (apt-get install strace)." >&2
    exit 1
fi

# run <label> <command...>
run() {
    local label="$1"; shift
    local log="${OUT}/${label}.log"
    if ! command -v "${1}" >/dev/null 2>&1; then
        echo "  skip ${label}: ${1} not installed" >&2
        return 0
    fi
    strace -f -qq -o "${log}" -- "$@" >/dev/null 2>&1 || true
    # strace lines: "pid syscall(args...) = ret". Extract the syscall name.
    awk '/^[0-9]+ +[a-zA-Z_]/ {gsub(/\(.*/,"",$2); print $2}' "${log}" 2>/dev/null \
        | sort | uniq -c \
        | awk -v L="${label}" '{print L" "$2" "$1}' >> "${COUNTS}"
}

run ls      ls -la /usr/bin
run cat     cat /etc/services /etc/passwd
run grep    grep -r root /etc
run find    find /usr/share -name '*.h' -type f
run sort    sort /etc/services
run tar     tar cf /tmp/hyperion-trace.tar /etc/services
run sh      sh -c 'for i in $(seq 1 50); do echo "$i"; done'
run bash    bash -c 'echo hi; pwd; id; uname -a'
run sed     sed -n '1,100p' /etc/services
run awk     awk 'BEGIN{for(i=0;i<1000;i++)s+=i; print s}'
run gcc     gcc --version
run python3 python3 -c 'print(sum(range(1000)))'
run uname   uname -a
run ps      ps aux

echo "== per-syscall totals across workloads =="
awk '{c[$2]+=$3} END{for(k in c) printf "%8d %s\n", c[k], k}' "${COUNTS}" | sort -rn
echo
echo "raw per-workload counts: ${COUNTS}"
