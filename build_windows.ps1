Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Set-Location $PSScriptRoot

cmake -S . -B build `
  -DARCHSTREAMER_BUILD_HOST=OFF `
  -DCMAKE_TOOLCHAIN_FILE=C:\dev\vcpkg\scripts\buildsystems\vcpkg.cmake
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build build --config Release -j2
exit $LASTEXITCODE
