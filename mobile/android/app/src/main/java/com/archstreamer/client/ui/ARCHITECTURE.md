# Android ui Package Architecture

`ui/` owns Android presentation and input-mode behavior.

This package composes screens, focus state, controller/keyboard/remote handling,
overlays, video views, and UI models. Gameplay input rules should stay explicit:
text input behaves like text input; gameplay reserves keyboard/controller events
for play controls and menu behavior.
