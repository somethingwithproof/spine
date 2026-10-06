#!/usr/bin/env bash
# Copyright (C) 2026 The Cacti Group
# Licensed under the GNU Lesser General Public License, version 2.1 or later.
# Run only against isolated regression services; database tests mutate fixtures.
set -euo pipefail
: "${SPINE_TEST_DB_HOST:?Set the isolated regression database host}"
: "${SPINE_TEST_SNMP_HOST:?Set the isolated regression SNMP host}"
coverage_base="${SPINE_COVERAGE_DIRECTORY:-coverage}"
mkdir -p "$coverage_base"
coverage_base="$(cd "$coverage_base" && pwd -P)"
source_dir="$(pwd -P)"
coverage_dir="$(mktemp -d "$coverage_base/run.XXXXXX")"
printf '%s\n' incomplete > "$coverage_dir/status.txt"
printf '%s\n' "$coverage_dir" > "$coverage_base/latest-run.txt"
coverage_complete=0
capability_added=0
finish() {
    status=$?
    if [ "$capability_added" -eq 1 ]; then
        if [ "$(id -u)" -eq 0 ]; then setcap -r ./test_spine_regressions; else sudo setcap -r ./test_spine_regressions; fi
    fi
    if [ "$coverage_complete" -eq 1 ] && [ "$status" -eq 0 ]; then
        printf '%s\n' success > "$coverage_dir/status.txt"
    else
        printf '%s\n' incomplete > "$coverage_dir/status.txt"
    fi
}
trap finish EXIT
for tool in gcc gcov lcov sha256sum tar; do command -v "$tool" >/dev/null; done
# Exclude only the owned evidence directory, never an untested production unit.
coverage_find_path='/not-in-source-tree'
test "$coverage_base" != "$source_dir"
case "$coverage_base" in "$source_dir"/*) coverage_find_path="./${coverage_base#"$source_dir"/}";; esac
find . -path "$coverage_find_path" -prune -o -type f \( -name '*.c' -o -name '*.h' -o -name 'Makefile.am' -o -name 'configure.ac' -o -path './tests/fixtures/*' -o -path './tests/tools/*' -o -path './.github/workflows/regressions.yml' \) -not -path './.git/*' -not -path './config/config.h' -print0 | sort -z > "$coverage_dir/source-inputs.nul"
xargs -0 sha256sum < "$coverage_dir/source-inputs.nul" > "$coverage_dir/source.sha256"
tar -czf "$coverage_dir/source-inputs.tar.gz" --null -T "$coverage_dir/source-inputs.nul"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git rev-parse HEAD > "$coverage_dir/revision.txt"
    git status --porcelain > "$coverage_dir/worktree-status.txt"
else
    : "${SPINE_SOURCE_REVISION:?Set the exact source revision for a copied build tree}"
    printf '%s\n' "$SPINE_SOURCE_REVISION" > "$coverage_dir/revision.txt"
    printf '%s\n' 'Copied build tree: source.sha256 is authoritative; repository status unavailable.' > "$coverage_dir/worktree-status.txt"
fi
gcc --version > "$coverage_dir/compiler.txt"
gcc -dumpmachine >> "$coverage_dir/compiler.txt"
if command -v dpkg-query >/dev/null; then
    dpkg-query -W gcc libc6-dev libmariadb-dev libsnmp-dev libssl-dev lcov autoconf automake libtool > "$coverage_dir/dependencies.txt"
fi
gcov --version > "$coverage_dir/gcov.txt"
lcov --version > "$coverage_dir/lcov.txt"
mkdir -p config m4
autoreconf -fi > "$coverage_dir/build.log" 2>&1
./configure CC=gcc CFLAGS="-std=gnu11 -g -O0 -UNDEBUG --coverage -fprofile-update=atomic -include $source_dir/tests/tools/coverage_process_exit.h" LDFLAGS='--coverage' >> "$coverage_dir/build.log" 2>&1
make --no-print-directory coverage-source-list > "$coverage_dir/production-sources.txt"
make --no-print-directory coverage-test-source-list > "$coverage_dir/test-sources.txt"
mapfile -t production_sources < "$coverage_dir/production-sources.txt"
mapfile -t test_sources < "$coverage_dir/test-sources.txt"
test "${#production_sources[@]}" -gt 0
test "${#test_sources[@]}" -gt "${#production_sources[@]}"
producer_units=()
for source in "${production_sources[@]}"; do
    case "$source" in *.c) ;; *) printf 'Unsupported production source: %s\n' "$source" >&2; exit 1;; esac
    test -s "$source"
    producer_units+=("$(basename "${source%.c}")")
    grep -Fxq "$source" "$coverage_dir/test-sources.txt"
done
for source in "${test_sources[@]}"; do
    case "$source" in *.c) ;; *) printf 'Unsupported test source: %s\n' "$source" >&2; exit 1;; esac
    test -s "$source"
    producer_units+=("test_spine_regressions-$(basename "${source%.c}")")
done
printf '%s\n' "${producer_units[@]}" > "$coverage_dir/producers.txt"
test "$(sort -u "$coverage_dir/producers.txt" | wc -l)" -eq "${#producer_units[@]}"
make clean >> "$coverage_dir/build.log" 2>&1
find . -maxdepth 1 -name '*.gcda' -delete
make -j2 spine test_spine_regressions >> "$coverage_dir/build.log" 2>&1
sha256sum config/config.h > "$coverage_dir/generated-config.sha256"
cp config/config.h "$coverage_dir/generated-config.h"
sha256sum spine test_spine_regressions > "$coverage_dir/binaries.sha256"
cp config.log "$coverage_dir/config.log"
notes=()
for producer in "${producer_units[@]}"; do test -s "$producer.gcno"; notes+=("$producer.gcno"); done
tests/tools/verify_coverage.sh --producers "$source_dir" "$coverage_dir/producers.txt" gcno
sha256sum "${notes[@]}" > "$coverage_dir/notes.sha256"
./test_spine_regressions > "$coverage_dir/default.log" 2>&1
grep -Fq 'production regression tests passed' "$coverage_dir/default.log"
grep -Fq 'production additional contracts passed' "$coverage_dir/default.log"
./test_spine_regressions --database > "$coverage_dir/database.log" 2>&1
grep -Fq 'production database configuration regressions passed' "$coverage_dir/database.log"
grep -Fq 'production live SNMP and PHP reindex contracts passed' "$coverage_dir/database.log"
./test_spine_regressions --snmp-agent > "$coverage_dir/snmp.log" 2>&1
grep -Fq 'production SNMP agent regressions passed' "$coverage_dir/snmp.log"
profiles=()
for producer in "${producer_units[@]}"; do test -s "$producer.gcda"; profiles+=("$producer.gcda"); done
tests/tools/verify_coverage.sh --producers "$source_dir" "$coverage_dir/producers.txt" gcda
if [ "$(id -u)" -eq 0 ]; then
    chmod a+rw "${profiles[@]}"
    setpriv --reuid=65534 --regid=65534 --clear-groups ./test_spine_regressions --icmp-no-capability > "$coverage_dir/icmp-denied.log" 2>&1
    setcap cap_net_raw+ep ./test_spine_regressions
    capability_added=1
    setpriv --reuid=65534 --regid=65534 --clear-groups ./test_spine_regressions --raw-icmp > "$coverage_dir/icmp-capability.log" 2>&1
else
    ./test_spine_regressions --icmp-no-capability > "$coverage_dir/icmp-denied.log" 2>&1
    sudo setcap cap_net_raw+ep ./test_spine_regressions
    capability_added=1
    ./test_spine_regressions --raw-icmp > "$coverage_dir/icmp-capability.log" 2>&1
fi
grep -Fq 'production ICMP socket-failure regression passed' "$coverage_dir/icmp-denied.log"
grep -Fq 'production ICMP loopback regression passed' "$coverage_dir/icmp-capability.log"
lcov_args=()
for source in "${production_sources[@]}"; do lcov_args+=(--include "$source_dir/$source"); done
lcov --capture --directory . --output-file "$coverage_dir/production.info" --rc branch_coverage=1 "${lcov_args[@]}" > "$coverage_dir/capture.log" 2>&1
tests/tools/test_coverage_verifier.sh > "$coverage_dir/verifier-self-test.log"
tests/tools/verify_coverage.sh "$coverage_dir/production.info" "$source_dir" "$coverage_dir/production-sources.txt"
lcov --summary "$coverage_dir/production.info" --rc branch_coverage=1 > "$coverage_dir/summary.txt" 2>&1
# Bind every producer and scenario to this immutable tested source snapshot.
sha256sum "${profiles[@]}" > "$coverage_dir/profiles.sha256"
mkdir -p "$coverage_dir/profiles" "$coverage_dir/bin"
cp -- "${notes[@]}" "${profiles[@]}" "$coverage_dir/profiles/"
cp -- spine test_spine_regressions "$coverage_dir/bin/"
(cd "$coverage_dir" && sha256sum profiles/* bin/*) > "$coverage_dir/saved-producers.sha256"
sha256sum --check "$coverage_dir/source.sha256" > "$coverage_dir/source-verification.log"
sha256sum --check "$coverage_dir/generated-config.sha256" >> "$coverage_dir/source-verification.log"
sha256sum --check "$coverage_dir/binaries.sha256" >> "$coverage_dir/source-verification.log"
sha256sum --check "$coverage_dir/notes.sha256" >> "$coverage_dir/source-verification.log"
printf '%s\n' default additional-contracts database live-reindex snmp icmp-denied icmp-capability > "$coverage_dir/scenarios.txt"
(cd "$coverage_dir" && sha256sum default.log database.log snmp.log icmp-denied.log icmp-capability.log scenarios.txt production.info summary.txt source.sha256 generated-config.sha256 binaries.sha256 notes.sha256 profiles.sha256 production-sources.txt test-sources.txt producers.txt revision.txt compiler.txt generated-config.h source-inputs.nul source-inputs.tar.gz saved-producers.sha256) > "$coverage_dir/evidence.sha256"
cat "$coverage_dir/summary.txt"
coverage_complete=1
