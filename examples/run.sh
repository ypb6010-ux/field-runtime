#!/usr/bin/env bash
# Generic launcher for a built Core example — sets the Qt / DLL / QML / libpq
# paths so the exe finds everything, then runs it.
#
# Usage (from the Claude `!` prompt or Git Bash):
#   ! /d/developer/Qt6/JMJ/core/examples/run.sh <exe-name> [args...]
#
# Self-contained examples (no external server / hardware needed):
#   run.sh example_qml_dashboard
#       QML 面板:日志 + 数据库 + 多来源(内置模拟 PLC)。History 页需本地 Postgres。
#   run.sh diag_modbus_latency --inproc --port 5599
#       诊断:主线程 vs worker 线程 modbus 读延迟(同进程内嵌 server)。
#   run.sh diag_modbus_latency --serve --port 5599        # 仅当独立 modbus server
#
# Override the Qt location with QT6_BIN (Windows-style path) if it differs.
set -e

QTDIR_WIN="${QT6_BIN:-D:/developer/3rdparty/Qt/6.8.3/msvc2022_64}"
QXORM_WIN="D:/developer/3rdparty/QxOrm/6.8.3/bin"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$(cd "$DIR/../build" && pwd)"        # core/build
REPOBIN="$(cd "$DIR/../../bin" && pwd)"     # JMJ/bin (libpq + ssl)

name="${1:?usage: run.sh <exe-name> [args...]   (e.g. example_qml_dashboard)}"
shift || true
EXE="$BUILD/examples/$name.exe"
if [ ! -f "$EXE" ]; then
    echo "Not built: $EXE" >&2
    echo "Build it with: cmake --build core/build --target $name" >&2
    exit 1
fi

# PATH entries must be unix-style ('D:/...' would split on the ':' PATH uses).
QTDIR_U="$(cygpath -u "$QTDIR_WIN")"
QXORM_U="$(cygpath -u "$QXORM_WIN")"
export QT_PLUGIN_PATH="$QTDIR_WIN/plugins"
export QML2_IMPORT_PATH="$QTDIR_WIN/qml"
export PATH="$BUILD:$BUILD/persistence:$QTDIR_U/bin:$QXORM_U:$REPOBIN:$PATH"

exec "$EXE" "$@"
