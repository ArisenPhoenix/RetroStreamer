# include/host/console

Console, emulator, and content headers live here.

This area owns game catalog handling (`GameConfig`), emulator process
configuration (`EmulatorConfig`), RetroArch helpers, save-profile logic, and
console-specific integrations.

System-specific code belongs in child directories such as `switch/` or `nds/`.
Generic session orchestration belongs in `include/host/session/`.
