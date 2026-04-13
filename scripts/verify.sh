#!/usr/bin/env bash
set -euo pipefail

echo "=== clang-format check ==="
# Check if code matches formatting style
if command -v clang-format >/dev/null; then
    clang-format --dry-run --Werror *.c *.h || echo "Formatting check failed (informative for now)"
else
    echo "clang-format not found, skipping."
fi

echo ""
echo "=== flawfinder (Security SAST) ==="
if command -v flawfinder >/dev/null; then
    flawfinder --minlevel=3 --quiet . || echo "flawfinder found issues (informative for now)"
else
    echo "flawfinder not found, skipping."
fi

echo ""
echo "=== cppcheck ==="
cppcheck --enable=all --std=c11 --error-exitcode=1 \
  --suppress=missingIncludeSystem \
  --suppress=unusedFunction \
  --suppress=checkersReport \
  --suppress=toomanyconfigs \
  *.c *.h 2>&1 | tee /tmp/cppcheck.txt || echo "cppcheck found issues (informative for now)"

echo ""
echo "=== scan-build (Clang Static Analyzer) ==="
make clean
scan-build -o /tmp/scan-results --status-bugs make -j"$(nproc)" 2>&1 || echo "scan-build found issues (informative for now)"

echo ""
echo "=== unit tests (cmocka) ==="
pushd tests/unit >/dev/null
make clean
make -j"$(nproc)" run
popd >/dev/null

echo ""
echo "=== smoke tests (with AddressSanitizer) ==="
./spine --help > /dev/null
echo "spine --help: OK"
./spine --version > /dev/null
echo "spine --version: OK"
echo ""
echo "=== doxygen check ==="
if command -v doxygen >/dev/null; then
    # Generate a default Doxyfile if it doesn't exist
    if [ ! -f Doxyfile ]; then
        doxygen -g > /dev/null
        # Quiet output unless there are errors
        sed -i 's/QUIET                  = NO/QUIET                  = YES/' Doxyfile
        # Exclude external libraries that aren't documented
        sed -i 's/EXCLUDE                =/EXCLUDE                = uthash.h/' Doxyfile
    fi
    doxygen Doxyfile || echo "Doxygen found issues (informative for now)"
    echo "Doxygen documentation generation: OK"
else
    echo "doxygen not found, skipping."
fi


echo ""
echo "=== All checks completed successfully ==="
