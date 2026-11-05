#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLUS_IDF_ROOT="${RICKYOS_IDF_PATH:-$TASK_ROOT/tools/esp-idf-v6.1}"
# Stop before preparing dependencies or producing a misleading binary.
python3 "$TASK_ROOT/scripts/verify_plus_release_idf.py" --idf "$PLUS_IDF_ROOT"
python3 "$TASK_ROOT/scripts/check_rickyos_plus_sources.py"
python3 "$TASK_ROOT/scripts/check_plus_font_resources.py"
python3 "$TASK_ROOT/scripts/prepare_plus_jpeg.py"
"$TASK_ROOT/.local/espressif/python_env/idf6.1_py3.14_env/bin/python" "$TASK_ROOT/scripts/prepare_plus_usb.py"
export IDF_TOOLS_PATH="$TASK_ROOT/.local/espressif"
export IDF_COMPONENT_MANAGER=0
PIO_TOOL_ROOT="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
export PATH="$PIO_TOOL_ROOT/tool-cmake/bin:$PIO_TOOL_ROOT/tool-ninja:/opt/homebrew/bin:$PATH"
# RICKYOS_IDF_PATH / RICKYOS_BUILD_DIR select another IDF checkout and build directory.
source "$PLUS_IDF_ROOT/export.sh"
# Restored source strings are authoritative. Do not reuse a stale generated
# StrId table after adding an option to the shared settings catalog.
"${RICKYOS_RESOURCE_PYTHON:-python3}" "$TASK_ROOT/firmware/rickyos-plus-1.2/upstream/scripts/gen_i18n.py" \
  "$TASK_ROOT/firmware/rickyos-plus-1.2/upstream/lib/I18n/translations" \
  "$TASK_ROOT/firmware/rickyos-plus-1.2/generated" \
  --src-dirs "$TASK_ROOT/firmware/rickyos-plus-1.2/upstream/src" "$TASK_ROOT/firmware/rickyos-plus-1.2/upstream/lib" \
  --strip-unused --builtin-langs en,zh_cn
BUILD_DIR="${RICKYOS_BUILD_DIR:-$TASK_ROOT/.local/build-rickyos-plus-1.2}"
cd "$TASK_ROOT/firmware/rickyos-plus-1.2"
# Diagnostic compile definitions come from environment variables in CMake.
# Reconfigure so reusing a build directory cannot silently keep the prior mode.
PLUS_IDF_COMMAND=(idf.py -B "$BUILD_DIR")
if [[ -n "${RICKYOS_SDKCONFIG:-}" ]]; then
  PLUS_IDF_COMMAND+=(-D "SDKCONFIG=$RICKYOS_SDKCONFIG")
fi
if [[ -n "${RICKYOS_SDKCONFIG_DEFAULTS:-}" ]]; then
  PLUS_IDF_COMMAND+=(-D "SDKCONFIG_DEFAULTS=$RICKYOS_SDKCONFIG_DEFAULTS")
fi
IDF_TARGET=esp32s31 "${PLUS_IDF_COMMAND[@]}" reconfigure
python3 "$TASK_ROOT/scripts/check_plus_wifi_security.py" "$BUILD_DIR/config/sdkconfig.h"
python3 "$TASK_ROOT/scripts/check_plus_private_network_profile.py" "$BUILD_DIR/compile_commands.json"
IDF_TARGET=esp32s31 "${PLUS_IDF_COMMAND[@]}" build
python3 "$TASK_ROOT/scripts/check_plus_web_memory.py" "$BUILD_DIR/config/sdkconfig.h"
# The device bootloader is IDF release/v6.1 9a97f6c5. An app built with another
# IDF (e.g. the plain v6.1 tag) locks up in early startup, and Plus has no OTA
# rollback, so such an image must never leave the build.
python3 "$TASK_ROOT/scripts/verify_plus_release_idf.py" --idf "$PLUS_IDF_ROOT" --project "$BUILD_DIR/project_description.json"
python3 "$TASK_ROOT/scripts/check_rickyos_plus_sources.py"
python3 - "$BUILD_DIR/RickyOS-Plus-1.2.elf" <<'PY'
from pathlib import Path
import sys
if b'pollMetalioPlusTouch' not in Path(sys.argv[1]).read_bytes():
    raise SystemExit('Linked application is missing the Plus touch backend')
print('Linked Plus touch backend verified')
PY
python3 "$TASK_ROOT/tests/check_plus_clock_objects.py"
