EDEN - Xbox 360 game-library frontend
=====================================

Eden is a Nintendo Switch game-library FRONTEND for the Xbox 360 (RGH/JTAG).
It scans a USB stick and shows the Switch game files it finds in a menu.

THIS BUILD DOES NOT EMULATE THE NINTENDO SWITCH.
It cannot boot, run, or play any game. It only discovers and lists files.
There are no working CPU, GPU, memory or firmware components. Nothing here is
faked or simulated: if you press A on a game, you get an honest notice.

There is a Super Mario 64 PC port for N64 (NOT this project). This is not that.

WHAT WORKS
  - Boots on Xbox 360 (libXenon), draws a menu, reads the controller.
  - Scans the "roms" folder of a FAT32 USB stick for
    .nsp / .xci / .nro / .nso files.
  - Reads each file's header to show its REAL info: title ID, region,
    container type (NSP/PFS0, XCI, NRO, NSO) and content count.
  - Parses scene filenames ("Game [TITLEID][vX][REGION].ext").
  - Detects a prod.keys file under keys/ and reports how many entries it has.
  - Shows a game grid with titles, file sizes, region and a DETECTED badge.
  - Home / Library / Game Info / Settings / About / Menu screens.
  - Rescan, grid-columns, show-extensions and scan-on-start options.

FOLDER LAYOUT
  switch.elf   the executable (launch from Aurora File Manager or XeLL)
  roms/        put your OWN, legally obtained game files here
  keys/        put your OWN, legally obtained prod.keys here (detected and
               counted for information; not used by this frontend build)
  assets/      icons / backgrounds / fonts (placeholders)

USING IT
  1. Copy this whole "eden" folder onto a USB stick formatted FAT32 (MBR).
  2. Put your game files in roms/.
  3. On the console, launch switch.elf from Aurora's File Manager (or XeLL).
  4. The frontend finds roms/ automatically (also checks eden/roms).

CONTROLS
  D-pad / left stick : navigate
  A                  : select / open
  B                  : back
  Start              : menu (Library / Settings / About / Rescan / Close)
  LB / RB            : previous / next page (library, game info)

LEGAL
  This project ships NO games, NO firmware, NO encryption keys and NO
  copyrighted box art. You must supply your own files. The frontend draws
  neutral placeholders instead of inventing covers.
  Licensed GPL-3.0-or-later. Inspired by the Eden emulator project;
  not affiliated with or endorsed by Eden, Nintendo, or Microsoft.
  Not affiliated with the Super Mario 64 PC port or any other project.
