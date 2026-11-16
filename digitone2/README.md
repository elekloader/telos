# telos on the Digitone II

OS 1.12, on elekloader's core-dn2 0.5 or newer. The same sources as the Digitakt II's (built with
`TELOS_DT1` and `TELOS_DN2`): the main README holds, with the differences below.

## Install

Build your OS with core-dn2 0.5 and this folder's TELOS (elekloader needs the Digitone II profile's
`drive_align`), flash it, then send mods over USB:

```bash
python tools/telos.py unit --device digitone-mk2 --port "Digitone II" add MOD.elemod ...
```

```bash
python tools/telos.py unit --device digitone-mk2 --port "Digitone II" restart
```

On the unit: SETTINGS, then MODS (TELOS), the last row, after SYSTEM (no icon). UP and DOWN move,
YES switches, NO goes back.

## What differs

- **The store** is 2 MB of the card's raw sectors from 0x7FE000, the Digitone mk1's layout
  (`telos.h`), and each project slot's mods a sector from 0x7FF000 (`store.c`). The Digitone II has
  no file system and no project data for mods. Stock writes no sector from 0x458000 up, and ElekLabs
  digikit keeps its own store from 0x800000 to the card's end, so TELOS sits just below it. The
  Data API's unchecked bank ids reach sectors below 0x800000 (ElekLabs digikit's finding 59): only a
  crafted request from a computer could write there. FORMAT on the unit does not touch it.
- **Memory.** The mods run from what the flashed build leaves of core's 256 KB (0x47F00000-0x47F40000).
  The link's scratch is RAM no OS instruction names, above the DMA buffers: the staging buffer (512 KB)
  at 0x47000000, the heap (1 MB) at 0x47080000.
- **When.** At core-dn2's `ev_card_ready`, in the init task, before the project and the UI task.
- **No stock copy**, as on the Digitakt II: stock bytes are read from RAM as the OS has them.
- **The keys** are the panel's codes, as on the Digitakt II (YES 10, UP 11, NO 12, LEFT 13, DOWN 14,
  RIGHT 15).
- **SHARC mods** are not TELOS mods: their code goes into the DSP's boot stream, in the flashed build.

## Checked

In the emulator of [ElekLabs/digikit](https://github.com/ElekLabs/digikit) (`digi-emud`, ColdFire
only, a card image, USB SysEx through its `midi.usb.in`), build TN2B:

- format, two mods sent and listed; after `unit restart` both loaded, site-check's own code site ran
  (its counter rose), and hello-marker's corner was drawn;
- the page, YES to `-OFF`, APPLY & RESTART, and the next boot linked the one left on;
- a mod that crashes the UI task: the next boot skipped the store's mods (skip 2);
- SAVE PROJECT AS with site-check running, site-check switched off, LOAD PROJECT: LOAD PROJECT MODS?
  `+SITE CHECK`, and YES restarted with it loaded.

`site-check` and `crash-check` here are the test mods; `examples/hello-marker-dn2` is elekloader's.

## Not yet

- Anything on a unit (ElekLabs digikit's own runtime has not run on a Digitone II either).
- The tick rate on a unit: the crash guard counts 300 ticks as 10 s.
- Whether the Digitone II asks to save a modified project on LOAD PROJECT.
