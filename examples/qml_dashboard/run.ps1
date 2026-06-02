# Launches example_qml_dashboard with the right DLL / Qt plugin / QML paths.
# Run it from PowerShell, or from the Claude `!` prompt via:
#   ! powershell -NoProfile -ExecutionPolicy Bypass -File "D:\developer\Qt6\JMJ\core\examples\qml_dashboard\run.ps1"
#
# Override the Qt location with  $env:QT6_BIN  if it differs from the default.

$ErrorActionPreference = "Stop"

$qtDir   = if ($env:QT6_BIN) { $env:QT6_BIN }
          else { "D:\developer\3rdparty\Qt\6.8.3\msvc2022_64" }
$repoBin = Resolve-Path "$PSScriptRoot\..\..\..\bin"            # JMJ\bin (libpq + ssl)
$build   = Resolve-Path "$PSScriptRoot\..\..\build"            # core\build
$qxorm   = "D:\developer\3rdparty\QxOrm\6.8.3\bin"
$exe     = Join-Path $build "examples\example_qml_dashboard.exe"

if (-not (Test-Path $exe)) {
    Write-Error "Not built yet: $exe`nBuild it with: cmake --build core\build --target example_qml_dashboard"
}

# Qt install bin must precede repo bin so the matching Qt DLLs win; repo bin
# supplies libpq.dll for the QPSQL driver.
$env:PATH = "$build;$build\persistence;$qtDir\bin;$qxorm;$repoBin;$env:PATH"
$env:QT_PLUGIN_PATH   = "$qtDir\plugins"
$env:QML2_IMPORT_PATH = "$qtDir\qml"

& $exe @args
