Aurora Bricks - FreeChainXenon XEX bring-up
===========================================

FILE: default.xex   (a genuine Xbox 360 XEX2, built with FreeChainXenon)

WHAT THIS IS
  This is a *bring-up* title, not the playable game. FreeChainXenon - the only
  free/libre toolchain that produces a real .xex - is very early and has no
  graphics, input or D3D9 yet, so the Breakout game cannot be drawn with it.
  This XEX exists to prove the Aurora/.xex deployment path end to end.

WHAT TO EXPECT ON CONSOLE
  If it runs, it will resolve a kernel function and display a code (0x1244A).
  That on-screen code is the proof it booted and executed. The console will
  then need a power cycle.

HOW TO TRY (RGH/JTAG console with Aurora):
  1. Copy the folder AuroraBricks-XEX-Bringup (containing default.xex) to any
     storage the console can read.
  2. Launch Aurora, open the File Manager, browse to the folder and run
     default.xex.

FOR THE PLAYABLE GAME
  Use the "AuroraBricks-libxenon" artifact instead (AuroraBricks.elf). That
  build uses libXenon and actually renders the game.
