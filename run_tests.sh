#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MOCK_PID=""
cleanup() {
  if [ -n "$MOCK_PID" ]; then
    kill "$MOCK_PID" 2>/dev/null && wait "$MOCK_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

FAILED=0

run_step() {
  local label="$1"
  shift
  echo ""
  echo "=== $label ==="
  if "$@"; then
    echo "=== $label: PASSED ==="
  else
    echo "=== $label: FAILED ==="
    FAILED=1
  fi
}

# 1. Python mock server tests
run_step "Mock server tests (pytest)" \
  mock_server/.venv/bin/python -m pytest mock_server/test_resolume_mock.py -v

# 2. Build C++
run_step "C++ build" \
  cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# 3. C++ unit tests (no mock server needed)
echo ""
echo "=== C++ unit tests ==="
build/test_protocol --reporter compact
build/test_composition --reporter compact
build/test_looper_core --reporter compact
echo "=== C++ unit tests: PASSED ==="

# 4. Start mock server for integration tests
echo ""
echo "=== Starting mock server for integration tests ==="
mock_server/.venv/bin/python mock_server/resolume_mock.py --port 8080 &
MOCK_PID=$!

# Wait for server to be ready
for i in $(seq 1 20); do
  if curl -s -o /dev/null http://127.0.0.1:8080/api/v1/product 2>/dev/null; then
    break
  fi
  sleep 0.1
done

# 5. C++ integration tests
run_step "C++ integration tests (ws_client)" \
  build/test_ws_client --reporter compact

# 6. Tear down mock server
kill "$MOCK_PID" 2>/dev/null && wait "$MOCK_PID" 2>/dev/null || true
MOCK_PID=""

# Summary
echo ""
if [ "$FAILED" -eq 0 ]; then
  echo "All tests passed."
else
  echo "Some tests failed."
  exit 1
fi
