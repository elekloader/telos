# TELOS (Analog Rytm MKII)

Mods kept on the Analog Rytm MKII itself, for OS 1.74 with elekloader's core-ar2 0.18 or newer. Version
0.1, built and linted against stock 1.74 only: not yet run in an emulator or on a unit under this name
(the Drive Loader it vendors passed elekloader's emulator gate).

TELOS on the Rytm is elekloader's Drive Loader (`mods/drive-loader-ar2`, in
[`vendor/drive-loader-ar2`](../vendor/drive-loader-ar2/README.md)) under TELOS's name. Only the
SETTINGS row differs (`row.s`); the `/mods` route, the boot link and the MODS page are the loader's,
unchanged.

## Install

Build your OS with TELOS in elekloader, as any mod: this folder (with `vendor/` beside it) and
core-ar2 0.18 with your stock OS file, then flash it as any OS update. Back up your projects first;
the stock OS file puts the unit back. **It is `drive-loader` to the linker.** The module's id is `drive-loader`
(its title stays TELOS), so +Drive mods that require `drive-loader` load under TELOS with no change,
and a build has TELOS or the Drive Loader, never both: same id, same claimed name (`data-route:mods`).

## Using it

- **Sending mods.** The unit's Data root lists a `/mods` folder, the +Drive's own `/mods`. Upload
  `.elemod` files there with Elektron Transfer, Elektroid or `elekloader mods`. No TELOS SysEx tool is
  needed, and `tools/telos.py` does not drive the Rytm.
- **On the unit.** SETTINGS > GLOBAL, then MODS (TELOS), the last row, with TELOS's gear. It opens the
  Drive Loader's MODS page: a +Drive file browser and the build list (a `*` marks a mod with saved
  data, and APPLY & RESTART reboots into a changed build, keeping the working project), as in
  elekloader's `mods/drive-loader-ar2/PAGE.md`.
- **When.** At core-ar2's `ev_boot_drive`, with the +Drive mounted and the working project loaded,
  before audio and the UI start. The mods chosen in `/mods/active` are linked into the +Drive mods' area.
- **Skipping.** Hold [NO] at power-on to skip the mods once. A start that didn't get 10 s in skips them
  once on the next start.

## How it differs from TELOS on the Digitone mk1

| | Digitone mk1 | Analog Rytm MKII |
|---|---|---|
| Transfer | TELOS's SysEx (`tools/telos.py`) | the unit's Data router, `/mods` |
| Store | 2 MB from sector 0x380000, 63 files | the +Drive's file system, `/mods` |
| Page | one row per mod, APPLY & RESTART | the Drive Loader's browser and build list, APPLY & RESTART |
| SETTINGS row | MODS (TELOS), gear | MODS (TELOS), the same gear |
| Projects keep their mods | yes | not yet |
