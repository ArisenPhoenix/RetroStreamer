ArchStreamer visual identity (source of truth for desktop + Android + Windows).

Files
  archstreamer-icon.svg                 Canonical full mark (ink tile + stream + pad)
  archstreamer-icon-foreground.svg      Android adaptive-icon foreground (no tile)
  archstreamer-icon-{16..512}.png       Raster exports
  archstreamer-icon.ico                 Windows multi-size icon (EXE / shell)
  archstreamer.qrc                      Qt resource for window icon (native runs)
  archstreamer_gui.rc.in                Windows .rc template (embeds the .ico)

Consumers
  Flatpak:  deploy/flatpak/*.yml installs SVG/PNG as io.github.ArisenPhoenix.ArchStreamer
  Linux:    cmake --install copies .desktop + hicolor icons (same name)
  Windows:  archstreamer_gui.exe embeds .ico; finish-install shortcuts use TargetPath,0
  Android:  mobile/.../res/mipmap-* + adaptive foreground
  All GUI:  QApplication::setWindowIcon from :/branding/archstreamer-icon-256.png

Regenerate rasters (from repo root):
  inkscape branding/archstreamer-icon.svg -o branding/archstreamer-icon-512.png -w 512 -h 512
  # …same for 128/256 and small sizes 16/24/32/48/64…
  convert branding/archstreamer-icon-{16,24,32,48,64,256}.png branding/archstreamer-icon.ico
