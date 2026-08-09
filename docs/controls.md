# Controls

This document defines ArchStreamer control meanings across Android, Android TV,
desktop clients, and host input routing.

## Button Names

Use these names when discussing or implementing controls:

| ArchStreamer name | PlayStation position | Common labels |
| --- | --- | --- |
| `SOUTH` | Cross | PS X, Xbox A, Android `BUTTON_A` |
| `EAST` | Circle | PS O, Xbox B, Android `BUTTON_B` |
| `NORTH` | Triangle | PS Triangle, Xbox Y, Android `BUTTON_Y` |
| `WEST` | Square | PS Square, Xbox X, Android `BUTTON_X` |
| `MENU` | Controller center/system button | PS button, Xbox Guide, Android Mode/Menu/Home key |
| `START` | Start / Options | Start, Options |
| `SELECT` | Select / Share | Select, Share, Back |
| `L1` / `R1` | Shoulders | Left/right shoulder |
| `L2` / `R2` | Triggers | Left/right trigger |
| `L3` / `R3` | Stick clicks | Left/right stick press |
| `UP` / `DOWN` / `LEFT` / `RIGHT` | D-pad | D-pad directions |

`MENU` is the canonical name for the center/system button. Avoid `Guide` in
user-facing text, and avoid `Home` as the main name because it can be confused
with Android's OS-level Home action.

## Gameplay Controls

When gameplay owns input, controls should be interpreted as follows:

| Physical input | Gameplay meaning |
| --- | --- |
| Controller `SOUTH` | Send `SOUTH` |
| Controller `EAST` | Send `EAST` |
| Controller `NORTH` | Send `NORTH` |
| Controller `WEST` | Send `WEST` |
| Keyboard `Enter` | Send `SOUTH` |
| Keyboard `Shift` | Send `EAST` |
| Keyboard / remote arrows | Send D-pad `UP` / `DOWN` / `LEFT` / `RIGHT` |
| Keyboard `Space` | Hold fast-forward while pressed |
| Keyboard `F` | Toggle fast-forward latch |
| Keyboard `P` | Toggle pause |
| Controller `MENU` | Toggle the ArchStreamer menu |

`Space` is a hold control during gameplay. Pressing it enables fast-forward;
releasing it disables the hold. The independent `F` latch may still keep
fast-forward enabled after Space is released.

`P` is only a pause control when gameplay owns input. It must not be sent as a
normal typed character while the menu or a text field owns input.

Opening the ArchStreamer menu during gameplay pauses in the background. Closing
the menu resumes by sending pause off. This is separate from the `P` pause
toggle.

## Menu Controls

When the ArchStreamer menu owns input:

| Physical input | Menu meaning |
| --- | --- |
| D-pad / arrows | Move menu focus |
| `SOUTH` / `Enter` / D-pad center | Activate focused item |
| `EAST` / Escape / Backspace | Leave field, step back, or close the menu |
| `MENU` | Toggle menu visibility |

While the menu owns input, gameplay hotkeys are disabled. In particular, `P`
does not pause-toggle, `F` does not fast-forward-toggle, and `Space` is not a
fast-forward hold.

## Text Entry

Text entry has priority over gameplay mappings.

When an OSK or real keyboard text field owns input:

| Physical input | Text meaning |
| --- | --- |
| `Enter` | Normal text-field Enter / confirm behavior |
| `Shift` | Normal keyboard Shift behavior |
| `Space` | Insert a space |
| `P` | Type `p` / `P` normally |
| `F` | Type `f` / `F` normally |
| Backspace | Delete previous character |
| Delete | Delete next character |

The special gameplay mapping `Enter -> SOUTH` applies only during normal
gameplay. It does not apply while typing in the OSK, a real text field, or any
other text-entry path.

## Device Notes

Android and Android TV report the controller center/system button under several
key codes depending on hardware: Mode, Menu, Guide, or Home. ArchStreamer should
treat those as `MENU` when the app receives them.

Desktop controller input comes through SDL and should use the same logical
button meanings above. Desktop keyboard input may be disabled by the user with
the client keyboard-input setting; when disabled, keyboard gameplay mappings
should not be sent to the host.

Touch overlay buttons use the same logical names and remapping rules as physical
controller buttons.
