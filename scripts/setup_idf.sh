#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export IDF_TOOLS_PATH="$TASK_ROOT/.local/espressif"
export PATH="/Users/rick/.platformio/packages/tool-cmake/bin:/Users/rick/.platformio/packages/tool-ninja:/opt/homebrew/bin:$PATH"
export IDF_PIP_WHEELS_URL=""
TASK_IDF="$TASK_ROOT/tools/esp-idf-v6.1"
TASK_EXPECTED_COMMIT=fff9895c82d744c7237be8847347bdd1b07c6643
if [[ ! -d "$TASK_IDF" ]]; then
  git clone --depth 1 --branch v6.1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git "$TASK_IDF"
fi
[[ "$(git -C "$TASK_IDF" rev-parse HEAD)" == "$TASK_EXPECTED_COMMIT" ]] || {
  echo 'ESP-IDF commit differs from the recorded baseline' >&2
  exit 1
}
git -C "$TASK_IDF" submodule update --init --recursive --depth 1
bash "$TASK_IDF/install.sh" esp32s31
