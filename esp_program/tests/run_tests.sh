#!/usr/bin/env bash
# Builds and runs the host-side tests for the desk logic (no hardware needed).
set -euo pipefail
cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/test_desk_logic"
c++ -std=c++17 -Wall -Wextra -Werror -I ../main test_desk_logic.cpp -o "$out"
"$out"
