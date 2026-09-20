#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/bridge-policy.XXXXXX")
trap 'rm -f "$OUT"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror "$ROOT/tests/test_policy.cpp" -o "$OUT"
"$OUT"
