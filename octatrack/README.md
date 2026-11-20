# TELOS (Octatrack)

Mods from the CF card for the Octatrack (MKI and MKII, OS 1.40C), with elekloader's core-ot 0.41
or newer.
A build flashed once with this mod links more mods at every boot from the card, with no new
firmware. Version 0.4, run in octabam's emulator only. The design
and what was measured are in [design/](design/README.md).

Build it as any mod, with an elekloader that has the Octatrack's TELOS engine (a loader's own area
and the stock shadow in the build descriptor): this folder beside core-ot and settings-ot.

## Using it

- **Files.** Copy the `.elemod` files to `/ELEKLOADER/MODS/` on the card (USB DISK MODE or a
  card reader), and list the ones to load in `/ELEKLOADER/MODS/active`, one line each: the file's
  name, a space, its CRC-32 (zlib's, 8 hex digits). A line without a CRC is skipped. A file
  changed since `active` was written is dropped, never loaded. `tools/telos.py` does this with the
  card's folder in place of `unit` (`add`, `on`, `off`, `remove`, `clear`, and `list` with the last
  start's `status.txt`), or tick files on the unit in MODS.
- **When.** At the card's first mount at boot (core-ot's `ev_card`, in the sys task), before the
  project load and the first audio frame. Changes apply at the next start: APPLY offers to restart.
- **What it wrote.** `/ELEKLOADER/MODS/status.txt`: how many loaded, then one line per line of
  `active`: the mod's id, `loaded`, `refused` or `dropped`, and the code and detail of a refusal
  (elekloader's docs/FORMAT.md, "+Drive refusal codes": 21 `site`, 22 `flashed`, 23 `early`,
  24 `stock` and 26 `busy` are below). A start that skipped says why.
- **Nothing to load.** No `active` (delete or rename it on a computer) loads nothing.
- **Skipping the mods (0.4).** Hold [NO] when the unit starts, until the card is read: that start
  loads nothing. A start that crashed or hung in its first 10 s skips them on the next start too
  (`/ELEKLOADER/MODS/armed`, written before the link and emptied 10 s after). Both skip once: MODS
  shows the mods `SKIP`, a dialog says why, and the start after loads them again. A card without
  `active`, or the card out, loads nothing at all.

## How it works

- **The link** is the Rytm's boot linker (`vendor/drive-loader/dlink.c` and `json.c` in this
  repo, elekloader's `mods/drive-loader-ar2`), built
  with `DL_SITES`, against the build descriptor (`core_desc`). A mod may write sites, as the shop's
  octabam conversions all do:
  - each site's bytes on the unit must still be stock's (its `stock_sha256`), so a site over
    anything the flashed build wrote is refused (24 `stock`), and so is a site inside the DSP
    payloads, which the OS has reused by the load point;
  - two mods' sites may not overlap: the later in id order goes (21 `site`);
  - a site in what the OS ran or read before the mount, and doesn't again before it matters, is
    refused (23 `early`): the USB init (0x4001dfbc..0x4001e042) and its descriptor tables
    (0x400e2000..0x400e21e0), elekloader's profile `drive_early`. Such a mod must be flashed;
  - a mod's stock parts are copied from the running OS, with the descriptor's stock shadow (the
    stock bytes of every range the flashed build wrote, `elekloader/desc.py`) laid over them.
- **The commit.** The sites and the tables' merge are written with interrupts masked, once no
  task's live stack holds an address strictly inside a site (the ten tasks' TCBs and stacks are
  fixed; a TCB keeps its SP at +72). Then the data cache is pushed and the instruction cache
  invalidated. After 50 tries without a quiet moment nothing is written (26 `busy`).
- **Memory.** `drive_area` (2 MB) holds the mods; `drive_staging` (256 KB) one file at a time;
  `telos_heap` (768 KB) the link's working memory. All three are `.bss` in core's reserve, so the
  build takes about 3 MB of sample memory more.
- **`memcpy`.** `telos.c` exports one: GCC calls it for `dlink.c`'s struct copies, and core-ot
  has none.

## On the unit: MAIN MENU > MODS (0.2, restart 0.4)

With settings-ot 0.2 (required), MODS starts with CARD MODS: one row per `.elemod` file in
`/ELEKLOADER/MODS` (the FAT driver's opendir and readdir, 0x46c8240e and 0x46c8244e), by title,
then APPLY.
- **Each row** reads `<state> <title>`. The state is `ON`, `OFF`, `+ON` (ticked, loads at the
  next start), `-OFF` (unticked, still running), `ERR` (refused at this start) or `SKIP` (on,
  but this start skipped the mods).
- **YES on a mod** unticks it, or ticks it after the boot's own checks (`dl_check`) on the set
  as it would be:
  - a clash with a ticked mod opens "CLASHES WITH <mod>", and YES swaps them;
  - any other refusal opens "CAN'T LOAD <mod>" with the reason.
- **APPLY** writes `active` (on the engine task) and asks RESTART NOW TO APPLY?: YES restarts the
  unit, NO leaves it for the next start.
- **The restart** is the ColdFire's soft reset (RCR at 0xFC0A0000 = 0x80, as core-dn1's on the
  Digitone), on the engine task after `active` is written. The stock OS never restarts itself (an
  OS upgrade asks for a power cycle), so this restart has run in the emulator only up to the
  reset; whether the unit comes back up from it is to see on a unit.
- **The checks see the OS as it was before the link.** The menu's checks run with mods already
  running, so TELOS keeps what each site it wrote replaced (`dl_site_saving`), and the stock test
  reads that, the boot-time copy of the data and the shadow (`dl_stock_read`). A site over a
  range the flashed build wrote is refused outright (22 `flashed`).

## Projects and their mods (0.3)

As on the Digitone mk1 (this repo's root). Each project keeps the list of mods that ran: a hash of
each one's id and version, flashed and card mods both, in core-ot's project data (`TLMS`, order 95).
- **When it is written.** After a project load, when what runs differs from the project's list,
  TELOS marks the block changed; core writes the project's `.work` about 2 s later, and SAVE
  PROJECT keeps it in `.strd`, as for any mod's project data.
- **LOAD PROJECT** (and RELOAD) of a project whose mods differ opens LOAD PROJECT MODS? with the
  counts: `+` to turn on, `-` to turn off, `~` the project's version of a mod (both files on the
  card), and what YES can't do: `?` not on this card or in this build, `!` another version, or one
  this start refused. YES ticks the card's mods as the project had them, writes `active` and
  restarts (0.4); NO keeps them. A project that only has `?` or `!` says PROJECT MODS NOT ALL
  HERE. A start that skipped the mods neither asks nor changes the project's list.
- **The next start's own load** says only what is still not as the project had it (the list YES
  left in `/ELEKLOADER/MODS/project`, cleared after). Otherwise the start's load says nothing,
  nor does a project without the list.

## DSP code

Through elekloader's `mods/dspload-ot` 0.2, flashed in the build: a TELOS mod lists its DSP source in
mod.json's `dsp_load`, and dspload sends it once the card's link has added it to `ot_dsp_blobs`.
A guard word at each end, checked every frame, stops it for good if a reverb's buffer reaches it
(dspload-ot's README). `design/dsptone` is the example.

## Not yet

Seen on a unit: none of it. The skip key holds only if the OS hears [NO] before the card mounts,
and the restart only if the unit comes back from a soft reset; both are steps in the hardware test.
