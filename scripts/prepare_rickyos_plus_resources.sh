#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT_ROOT="$TASK_ROOT/firmware/rickyos-plus-1.2"
RESOURCE_PYTHON="${RICKYOS_RESOURCE_PYTHON:-python3}"
cd "$PROJECT_ROOT/upstream"
"$RESOURCE_PYTHON" scripts/gen_i18n.py lib/I18n/translations ../generated --src-dirs src lib --strip-unused --builtin-langs en,zh_cn
"$RESOURCE_PYTHON" scripts/build_html.py
# RickyOS never bundles or auto-installs the upstream first-use guide.

"$RESOURCE_PYTHON" "$TASK_ROOT/scripts/compress_plus_builtin_fonts.py"
