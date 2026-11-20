# telos on the Digitakt II

OS 1.17, on elekloader's core-dt2 1.2 or newer. The same sources as the Digitakt mk1's (built with
`TELOS_DT1` and `TELOS_DT2`), so everything in the main README holds, with the differences below.

## Install

Build your OS with core-dt2 1.2 and this folder's TELOS (elekloader needs the Digitakt II profile's
`drive_align`), flash it, then send mods over USB:

```bash
python tools/telos.py unit --device digitakt-mk2 --port "Digitakt II" add MOD.elemod ...
```

```bash
python tools/telos.py unit --device digitakt-mk2 --port "Digitakt II" restart
```

On the unit: SETTINGS, then MODS (TELOS), the last row, after SYSTEM (no icon). The keys are the
mk1's: UP and DOWN move, YES switches, NO goes back.

## What differs from the mk1

- **The store** is a file on the +Drive, `/telos/store`, and the projects' lists `/telos/projects`, as
  on the Digitakt mk1 (`../digitakt/store.c`). TELOS writes no raw sector, so it never meets
  ElekLabs digikit's store at the card's end (sectors 0x2DF1000-0x2E1A000).
- **Memory.** The mods run from what the flashed build leaves of core's 256 KB (0x47F00000-0x47F40000).
  The link's scratch is RAM no OS instruction names: the staging buffer (512 KB) at 0x47E80000, in the
  profile's `ddr-low` area, and the heap (640 KB) at 0x47F40000, between core's area and the boot
  stack's top 128 KB. A mod's `regions` may be in `ddr-low` or `ddr`.
- **No stock copy.** core-dt2 has no room for a copy of the 3.2 MB OS before it runs, so stock bytes
  are read from RAM as the OS has them (with the build descriptor's shadow over the flashed build's
  sites). A mod whose site covers data the OS has changed by then is refused (code 24).
- **The keys** are the panel's codes (YES 10, UP 11, NO 12, LEFT 13, DOWN 14, RIGHT 15), and the text
  call takes flags where the mk1's takes a length (`TEXTF` in `page.c`).
- **Projects.** Saves and loads run in a background worker task (core-dt2's events). The boot's own
  load of the working copy calls no event, so, as on the mk1, a restart for a project's mods is
  checked 60 ticks after the boot.

## Checked

In the emulator of [ElekLabs/digikit](https://github.com/ElekLabs/digikit) (`digi-emud`, ColdFire
only, a +Drive card image, USB SysEx through its `midi.usb.in`):

- format, two mods sent and listed; after `unit restart` both loaded, and site-check's own code site
  ran (its counter rose while the UI ran);
- the page: both mods ON, the store's use, YES to `-OFF`, APPLY & RESTART, and the next boot linked
  the one left on;
- a mod that crashes the UI task on its 30th tick: the next boot skipped the store's mods (skip 2);
- SAVE PROJECT AS with site-check running, site-check switched off and restarted, then LOAD PROJECT:
  LOAD PROJECT MODS? `+SITE CHECK`, and YES restarted with it loaded.

`site-check` and `crash-check` here are the test mods.

## Not yet

- Anything on a unit.
- The tick rate on a unit: the crash guard counts 300 ticks as 10 s, the mk1's rate.
- Whether the Digitakt II asks to save a modified project on LOAD PROJECT: the emulator showed no
  prompt, but the test's edit may not have marked the project modified.
