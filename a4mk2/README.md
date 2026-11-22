# TELOS (Analog Four MKII)

Mods kept on the Analog Four MKII itself, for OS 1.56 with elekloader's core-a4mk2 0.10 or newer. Version
0.3, built and linted against stock 1.56 only: not yet run in an emulator or on a unit.

TELOS on the A4 is elekloader's A4 Drive Loader (`mods/drive-loader-a4mk2`, in
[`vendor/drive-loader-a4mk2`](../vendor/drive-loader-a4mk2/README.md)) under TELOS's name. Only the
SETTINGS row differs (`row.s`); the store, the transfer, the boot link and the MODS page are the
loader's, unchanged.

## Install

Build your OS with TELOS in elekloader, as any mod: this folder (with `vendor/` beside it) and
core-a4mk2 0.10 with your stock OS file, then flash it as any OS update. Back up your projects first;
the stock OS file puts the unit back. **It is `drive-loader` to the linker.** The module's id is `drive-loader`
(its title stays TELOS), so +Drive mods that require `drive-loader` load under TELOS with no change,
and a build has TELOS or the Drive Loader, never both: same id, same claimed names
(`data-route:mods`, `settings:MODS`).

## Using it

- **Sending mods.** The unit's Data root lists a `/mods` folder, as on the Analog Rytm MKII's Drive
  Loader. Upload `.elemod` files there with Elektron Transfer, Elektroid or `elekloader mods`; folders
  go three deep. No TELOS SysEx tool is needed, and `tools/telos.py` does not drive the A4.
- **On the unit.** SETTINGS > GLOBAL, then MODS (TELOS), the last row, with TELOS's gear. It opens the
  Drive Loader's MODS page: a +Drive file browser and the build list (KEEP, ADD, DROP; a `*` marks a mod with saved data,
  and APPLY & RESTART reboots into a changed build, keeping the working project), as in
  elekloader's `mods/drive-loader-a4mk2/PAGE.md`.
- **When.** At core-a4mk2's `ev_boot_drive`, once the +Drive is readable and before the project load.
  The mods chosen in `/mods/active` are linked into the +Drive mods' area.
- **Skipping.** Hold [NO] at power-on to skip the mods once. A start that didn't get 10 s in skips them
  once on the next start.
- **The store.** Sectors 0x3000..0x7a000 of the +Drive, which stock never addresses: a directory in two
  copies and each file one run of sectors. FORMAT +DRIVE empties it.

## How it differs from TELOS on the Digitone mk1

| | Digitone mk1 | Analog Four MKII |
|---|---|---|
| Transfer | TELOS's SysEx (`tools/telos.py`) | the unit's Data router, `/mods` |
| Store | 2 MB from sector 0x380000, 63 files | sectors 0x3000..0x7a000, folders, up to 1,024 entries |
| Page | one row per mod, APPLY & RESTART | the Drive Loader's browser and build list, APPLY & RESTART |
| SETTINGS row | MODS (TELOS), gear | MODS (TELOS), the same gear |
| Projects keep their mods | yes | not yet |
