#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT

cd "${REPO_ROOT}"

readonly TRACKED_EXACT=(
  "configure.ac"
  "Makefile.am"
  "Makefile.in"
  "aclocal.m4"
  "libtool"
)

readonly TRACKED_PREFIX=(
  "autom4te.cache/"
  "m4/"
)

readonly WORKTREE_PATHS=(
  "configure"
  "config.log"
  "config.status"
  "autom4te.cache"
  "aclocal.m4"
  "libtool"
  "m4"
  "config/compile"
  "config/config.guess"
  "config/config.sub"
  "config/depcomp"
  "config/install-sh"
  "config/ltmain.sh"
  "config/missing"
  "config/stamp-h1"
  "config/config.h.in"
  "config/config.h.in~"
  "config/config.guess~"
  "config/config.sub~"
)

fail=0

for path in "${TRACKED_EXACT[@]}"; do
  if git ls-files --error-unmatch "${path}" >/dev/null 2>&1; then
    echo "ERROR: tracked autotools file: ${path}"
    fail=1
  fi
done

for prefix in "${TRACKED_PREFIX[@]}"; do
  if git ls-files "${prefix}*" | grep -q .; then
    echo "ERROR: tracked autotools directory content: ${prefix}"
    fail=1
  fi
done

for path in "${WORKTREE_PATHS[@]}"; do
  if [[ -e "${path}" ]]; then
    echo "ERROR: autotools artifact present: ${path}"
    fail=1
  fi
done

if [[ ${fail} -ne 0 ]]; then
  cat <<'EOF'
This repository is CMake-only.
Remove autotools files/artifacts and use:
  cmake -S . -B build -DSPINE_BUILD_MAIN=ON
  cmake --build build
EOF
  exit 1
fi

echo "OK: no autotools files/artifacts detected"
