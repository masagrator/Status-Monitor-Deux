# Status Monitor Deux
Monitor Your hardware in real time!<br>
This is an overlay homebrew dedicated to Nintendo Switch.<br>
You need to have installed Tesla environment to use it.

> [!NOTE]
> This README was written to convey only basics of tool.

This is the next iteration of Status Monitor Overlay that changes structurally how it works.<br>
Instead of hardcoding each mode now they are stored as script files with filetype `.smd`.<br>
Any additional service can be added by creating `.smse` file.

# Data shown in included SMD files
- CPU Usage for each core (Cores `#0`-`#2` are used by apps/games, Core `#3` is used by OS, background processes and also Tesla overlays)
- GPU Load
- CPU, GPU & RAM target frequencies (also real frequencies + RAM Load if [sys-clk 2.0.0_rc4+](https://github.com/retronx-team/sys-clk/releases) or [Horizon OC 2.2.0+](https://github.com/Horizon-OC/Horizon-OC/releases) is installed, use only official RetroNX release for reliable results)
- Used RAM categorized to: (not supported by FWs <5.0.0)
  - Total
  - Application
  - Applet
  - System
  - System Unsafe
- SoC, PCB & Skin temperatures (Skin temperature not supported by FWs <5.0.0)
- Fan Rotation Level
- PFPS, FPS, resolutions, game read speed (shows only if [my fork of SaltyNX](https://github.com/masagrator/SaltyNX/releases) is installed)
- Battery temperature, raw charge, age, average voltage, average current flow and average power flow
- Charger type, max voltage, and max current
- DSP usage (only for FW older than 17.0.0)
- NVDEC, NVENC and NVJPG clock rates
- Network type + Wi-fi password

# Functionality:
- If you can see Ⓨ next to mode name, it means you can configure it by pressing Y button which shows up list of possible things to tweak for current mode like which data should be shown, how they should be shown.
- Global settings are available by pressing Left D-Pad button in Main Menu.
- If you can see joycon symbol next to mode name, it means it's using custom exit key combo. To see how it looks like you can go to Global settings -> Exit key combo, you can change it too.
- You can move supported overlays on screen with touch screen or sixaxis (for which buttons enable sixaxis you can see in main menu settings)
- Mutlilanguage support that doesn't need recompiling to work, it supports all languages as long as Switch builtin fonts are supporting your alphabet
- If you want to implement support for additional services to make them available from status monitor, you can create your own .smse file. It supports only `Out` functions. Already included are extensions for `hoc:clk` and `sys:clk` if you want to see examples.
- You can create your own overlays, you can look in `modes` folder for seeing how all of them are implemented. Format is explained [here](docs/SMD_FORMAT.md)

> [!NOTE]
> Currently implemented languages:
> - American English
> - Polish
> - German
>
> If you want to add new language, send in issues those two translated files: [FILE 1](config/status-monitor-deux/locale.ini), [FILE 2](docs/toTranslate.ini)

All additional files are stored in `config/status-monitor-deux`, including:
- `modes` folder - stores SMD files used for rendering. If there is only one SMD file detected, overlay jumps automatically into detected file (this can be turned off in Global settings). Each SMD file stores its own texts and localization must be implemented inside them.
- `extensions` folder - stores SMSE files which store informations about additional services and how their variables should be named. For functions without buffers function name is used as reference to variable it outputs.
- `config.ini` - stores global settings and settings for each mode. It's automatically created on first boot if it doesn't exist.
- `locale.ini` - stores localization info unrelated to SMD files.

# How to setup everything: 

Atmosphere CFW is required. SX OS and other CFWs are not supported.
It's advised to use Atmosphere's USB Transfer Tool homebrew to transfer files. If you use Hekate's USB Mass Storage and you will be putting files using any other Operating System than Windows, you must run Hekate's Archive Bit Fixer after putting all files.
This tutorial is only for Atmosphere 1.8.0+ users 

1. Download newest `SaltyNX` release from [HERE](https://github.com/masagrator/SaltyNX/releases), unpack zip file, copy both folders (`SaltySD` and `atmosphere`) to the root of your sdcard, accept any popup about overwriting folders.
2. Download newest `Ultrahand` release (sdout.zip) from [HERE](https://github.com/ppkantorski/Ultrahand-Overlay/releases), unpack it, copy all unpacked folders to the root of sdcard, accept any popup about overwriting folders.
3. Download `Status Monitor Deux` release from [HERE](https://github.com/masagrator/Status-Monitor-Deux/releases), copy folders `switch` and `config` to root of sdcard. It may not be visible in USB Mass Storage, but it's there.
4. Restart Switch, now you can access overlays by pressing all 3 buttons at once: `L`, `D-pad down` and `R-stick` (aka pressing it).

# Extras

Syntax highlighting for VS Code<br>
https://github.com/masagrator/Status-Monitor-Deux/tree/main/.vscode/extensions/statusmonitor-smd

Repository storing other SMD files than stored in this repository:<br>
https://github.com/masagrator/SMD-Files

---

# Thanks to:
- KazushiMe for writing code to read registers from max17050 chip
- CTCaer for Hekate from which I took max17050.h and calculation formulas for reading battery stats from max17050 chip
- Lightos for providing German translation

Huge part of new code relative to Status Monitor Overlay was written with Claude. SMD and SMSE files design is 100% my own.

# Troubleshooting:

> [!IMPORTANT]
> Q: When opening Full or Mini mode, overlay is showing that Core #3 usage is at 100% while everything else is showing 0, eventually leading to crash. Why this happens?

A: There are few possible explanations: 
1. You're using nifm services connection test patches (in short `nifm ctest patches`) that are included in various packs. Those patches allow to connect to network that has no internet connection. But they cause nifm to randomly rampage when connected to network. Find any folder in `atmosphere/exefs_patches` that has in folder name `nifm`, `nfim` and/or `ctest`, delete this folder and restart Switch (if you are using `sys-patch`, turn off `nifm` patching). If you must use it, only solution is to use this overlay only in airplane mode.
2. You're using some untested custom sysmodule that has no proper thread sleeping implemented. Find out in atmosphere/contents any sysmodule that you don't need, delete it and restart Switch.
3. Your Switch is using sigpatches, is not a primary device, is using linked account, and is connected to network. Delete sigpatches, change your Switch to primary device, unlink account, or disable Wi-Fi.

> [!IMPORTANT]
> Q: When I open overlay, nothing is listed.

A: It means you have removed or didn't copy `status-monitor-deux` folder from/to `sdmc:/config/`. Download newest release and be sure to copy all files in release zip.
