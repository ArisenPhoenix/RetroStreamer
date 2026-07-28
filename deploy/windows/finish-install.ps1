Copy-Item .\build\SDL2.dll "C:\Program Files\ArchStreamer\bin\" -ErrorAction SilentlyContinue
# if it isn't in build\, grab it from vcpkg:
# Copy-Item "$env:VCPKG_ROOT\installed\x64-windows\bin\SDL2.dll" "C:\Program Files\ArchStreamer\bin\"
$exe = "C:\Program Files\ArchStreamer\bin\archstreamer_gui.exe"
# vcpkg Qt usually ships windeployqt here:
& "$env:VCPKG_ROOT\installed\x64-windows\tools\Qt6\bin\windeployqt.exe" $exe

& "C:\Program Files\ArchStreamer\bin\archstreamer_gui.exe"