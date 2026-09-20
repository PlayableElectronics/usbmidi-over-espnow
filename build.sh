#!/bin/sh
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR="$PROJECT_DIR"
export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$REPO_DIR/.local/platformio}"
exec pio run --project-dir "$PROJECT_DIR" "$@"
