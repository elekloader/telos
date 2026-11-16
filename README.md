<p align="center"><img src="brand/telos-header.png" alt="telos, the elekloader os" width="480"></p>

# telos

**The elekloader OS.** Flash once, then keep your mods on the unit itself. A build flashed once with
telos links more mods at every boot from a store on the unit, with no new firmware: you send
`.elemod` files over USB, and a page in the unit's SETTINGS switches them on and off.

telos is part of [elekloader](https://github.com/elekloader/elekloader), the mod loader for Elektron
firmware, and is meant for every device elekloader supports.

| Device | telos | Status |
|---|---|---|
| Digitone mk1 and Digitone Keys (OS 1.43, 1.44) | 0.2 | runs on a unit (OS 1.44); OS 1.43 in digiemu's emulator only |
| Octatrack MKI and MKII (OS 1.40C) | 0.2 | runs in the emulator; not in this repo yet |
| Digitakt II (OS 1.17) | 0.3 | runs in ElekLabs digikit's emulator; [digitakt2/README.md](digitakt2/README.md) |
| Digitakt, Digitone II, Analog Rytm, Analog Four, Model:Cycles, Model:Samples | | planned |

The rest of this page is the Digitone mk1's: OS 1.43 and 1.44, on elekloader's core-dn1 5.0 or newer.
Version 0.2 has run on a Digitone mk1 with OS 1.44: three mods sent over USB (19 KB in 0.6 s),
restarted into, run, and switched off and on again from the unit.

## Install

1. **Build your OS with TELOS** in elekloader, as any mod: TELOS and core-dn1 with your stock OS file.
   It needs an elekloader whose Digitone mk1 profile gives +Drive mods their area (`drive_align`);
   with an older one, the build has no room for them and every mod shows `E17`.
2. **Flash it**, as any OS update. Back up your projects first; the stock OS file puts the unit back.
3. **Send mods** with the unit on USB (close Elektron Transfer and Overbridge first: they hold the
   port), with elekloader importable (`PYTHONPATH=<elekloader checkout>`):

   ```bash
   python tools/telos.py unit add MOD.elemod ...
   ```

   ```bash
   python tools/telos.py unit restart
   ```

   They load after the restart. `add` formats the store the first time and replaces a file of the
   same name; `--off` sends a mod switched off. `list`, `on NAME`, `off NAME`, `remove NAME`,
   `clear`, `info` and `format` do the rest, and `--port` picks a MIDI port by part of its name
   (default `Digitone`). The files go as published: the unit links them against its own OS. The
   same commands with a card image in place of `unit` (an emulator's `plusdrive.img`) write the
   store into the image.

## On the unit

SETTINGS (the GLOBAL key), then MODS (TELOS), the last row, with a gear: a page with one row per mod
in the store, its state on the right, the store's use in the title bar, and APPLY & RESTART last
(RESTART when nothing changed). UP, DOWN or any knob move; YES switches a mod on or off for the next
boot (RIGHT on, LEFT off), written to the store at once; YES on APPLY & RESTART reboots, and the boot
links the new set; NO goes back to SETTINGS. PLAY, STOP and TEMPO stay the Digitone's.

| state | means |
|---|---|
| `ON` | on, running |
| `+ON` | on from the next boot |
| `OFF` | off |
| `-OFF` | off from the next boot, running until then |
| `E<n>` | on, but the boot refused it: n is the refusal code (elekloader's docs/FORMAT.md, "+Drive refusal codes") |

**A mod that crashes.** A boot that links mods sets a crash guard in the store, and 10 s of ticks later
clears it. A crash before then restarts the unit at once, and a boot that finds the guard links nothing
and clears it: one safe boot, then the set again.

**Projects and their mods.** Each project saved under TELOS keeps the list of mods that were running (a
hash of each mod's id and version, in the project's mod data). Loading one (the Digitone asks to save
the current project first, as stock does):

- After the load, when the project's mods are not the ones running, **LOAD PROJECT MODS?** lists what YES
  would do: `+` turn on, `-` turn off, `~` switch to the project's version (both in the store), and what it
  cannot: `?` a mod not on the unit, `!` another version than the project's, or one the boot refused
  (`E<n>`). YES writes the store and restarts; NO keeps the mods. With only `?` and `!` it says so (YES OK).
- After that restart the unit says only what is still not as the project had it.

The boot's own load of the last project says nothing, nor does a project saved without the list (before
this, or with the project's mod data full: the list goes last, after the pedals' settings). Saving a
project records what runs then.

## What a TELOS mod may be

A +Drive mod (elekloader's docs/FORMAT.md, "+Drive mods"): `.run` and `.bss` only, no sites, entries
only in tables that take more. core-dn1's tables get 16 spare entries each in a TELOS build. A table
core reads once at boot (`core_params`) takes the entry but core never reads it, so a mod's parameter
slots do not show yet. [examples/hello](examples/hello) is the smallest one.

## The store

2 MB of the +Drive's raw sectors, from sector 0x380000 (`telos.h`, and `STORE` in `tools/telos.py`). No
stock code reads or writes there: every stock card access is below sector 0x180000, FORMAT +DRIVE
erases only below that, and the smallest eMMC part the firmware accepts ends at 0x3A4000. It is not a
project slot, so it never shows in Transfer. It holds up to 63 files.

The directory has two copies, each with a sequence number and a CRC-32. The newer valid copy counts, and
a write goes to the other one, so a write cut short by a power loss leaves the older copy standing.
Sectors +17 to +31 hold TELOS's index of the mods it has met (id, version, title), which names a mod a
project needs after the mod is gone. The layout is in `telos.h` and `projset.c`.

## How it works

- **When.** At core-dn1's `ev_card_ready`, in the init task: the card is up, and the project, the UI
  task, the views and the MIDI task do not exist yet.
- **The link** is elekloader's +Drive boot linker (`vendor/drive-loader`, see its README) against the
  build descriptor `core_desc`. TELOS writes the chosen files as an `/mods/active` text (name and CRC-32
  per line) and reads each file from the store by name. A file whose CRC is not the directory's is
  dropped (code 2).
- **Memory.** The mods run from what the flashed build leaves of core's 1.25 MB (about 1.1 MB with core
  and TELOS alone). The link's staging buffer (512 KB, one file at a time) and its heap (1 MB) are
  scratch at the start of the bulk area, used only during the link, before any mod runs. The bulk
  area's contract is that a mod clears what it reads before writing it, so a pedal that owns that piece
  finds what it would at power-on. The status the page shows is copied out of the scratch heap first.
- **The page.** `row.s` adds the SETTINGS row (core's `ev_settings`, `core_additem`; its label needs the
  firmware's `std::string` constructor, and its gear icon, a 9 x 9 bitmap object its draw callback blits
  as the stock rows' do, the bitmap class's vtable: `defsym`s in `mod.json`, 1.44's in its port).
  `page.c` draws over SETTINGS on `ev_draw` and takes the keys on `ev_key` and `ev_enc` while it is
  open, NO's release too, so SETTINGS stays open under it.
- **The transfer** (`upload.c`): SysEx `F0 7D 54 4C <cmd> <seq> <7-bit body> F7` (0x7D is MIDI's
  non-commercial id), at core-dn1's `ev_sysex`. The MIDI task carries each request out and answers it,
  which the host waits for: a file goes as 512-byte DATA requests into the first free run of sectors
  that holds it, and END reads it back and checks its CRC-32 before the directory gets its entry. The
  MIDI task is above the UI task, so the page never runs in the middle of a request; the page's own
  writes set `telos_dir_busy`, and a request meanwhile is answered BUSY. Other MIDI input waits while a
  sector is written. The commands are at the top of `upload.c`; `tools/telos.py`'s `Unit` is the host's
  side.
- **The cache.** After the link, `flush.s` pushes the data cache and invalidates the instruction cache,
  as the Rytm's loader does.

## Build and test

```bash
python -m elekloader.sdk.build . --stock Digitone_and_Digitone_Keys_OS1.44.syx --out out
```

```bash
python tests/test_telos.py
```

The tests need no firmware: they check the store's layout against `telos.h` and run `tools/telos.py`
against a fake unit. In digiemu's emulator (over its DIN MIDI) a fresh unit took three mods in 1.9
emulated seconds on 1.43 and 1.44, and the next boot loaded them; digiemu's UI task stops after any
MIDI input, so the emulator can't show the page after a transfer, nor RESTART.

## Not yet

- The largest SysEx the unit takes: 600-byte DATA requests work on a unit, larger ones are untried.
- On a unit: the crash guard, a refused mod's `E<n>`, a transfer while a pattern plays, OS 1.43.
- Sending mods from elekloader's web page.
- A skip key at power-on.
- `core_params` at run time (a core-dn1 `core_param_install`).
- A check before switching on: the page shows a refusal only after the restart.

## Licence

GPL-2.0-or-later (LICENSE), as elekloader. `vendor/drive-loader` is elekloader's, under the same licence.
