#!/bin/sh
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$PROJECT_DIR/../.." && pwd)
PORT=${1:?usage: ./flash.sh /dev/cu.usbmodemXXXX}
export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$REPO_DIR/.local/platformio}"
exec pio run --project-dir "$PROJECT_DIR" -t upload --upload-port "$PORT"
