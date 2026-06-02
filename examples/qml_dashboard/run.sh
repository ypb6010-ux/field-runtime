#!/usr/bin/env bash
# Launches example_qml_dashboard with the right DLL / Qt plugin / QML paths.
# For the Claude `!` prompt (bash) or Git Bash:
#   ! /d/developer/Qt6/JMJ/core/examples/qml_dashboard/run.sh
# Override the Qt location with QT6_BIN (Windows-style path) if it differs.
set -e

QTDIR_WIN="${QT6_BIN:-D:/developer/3rdparty/Qt/6.8.3/msvc2022_64}"
QXORM_WIN="D:/developer/3rdparty/QxOrm/6.8.3/bin"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$(cd "$DIR/../../build" && pwd)"        # core/build
REPOBIN="$(cd "$DIR/../../../bin" && pwd)"     # JMJ/bin (libpq + ssl)
EXE="$BUILD/examples/example_qml_dashboard.exe"

if [ ! -f "$EXE" ]; then
    echo "Not built yet: $EXE" >&2
    echo "Build it with: cmake --build core/build --target example_qml_dashboard" >&2
    exit 1
fi

# PATH entries must be unix-style — a literal "D:/..." would split on the ':'
# bash uses as the PATH separator and corrupt the list. Qt's plugin/qml vars
# are single values, so they keep Windows-style paths (Qt accepts '/').
QTDIR_U="$(cygpath -u "$QTDIR_WIN")"
QXORM_U="$(cygpath -u "$QXORM_WIN")"
export QT_PLUGIN_PATH="$QTDIR_WIN/plugins"
export QML2_IMPORT_PATH="$QTDIR_WIN/qml"
export PATH="$BUILD:$BUILD/persistence:$QTDIR_U/bin:$QXORM_U:$REPOBIN:$PATH"

exec "$EXE" "$@"
