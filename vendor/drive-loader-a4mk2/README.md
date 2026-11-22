# The A4 MKII's Drive Loader (vendored)

Every file here is elekloader's `mods/drive-loader-a4mk2/` file of the same name, unchanged, from
[elekloader/elekloader](https://github.com/elekloader/elekloader) commit `092c90f` (drive-loader-a4mk2
0.1 with the `*` mark for saved data and APPLY & RESTART, #408 and #409, GPL-2.0-or-later; the Analog
Four MKII's +Drive mods loader, by Iced-Cream). Its `page.s`,
`mod.json` and docs (`README.md`, `PAGE.md`) are not copied: TELOS for the A4 builds these sources with
its own `a4mk2/row.s` and `a4mk2/mod.json`. The loader's design (the store, the `/mods` route, the boot
link and the MODS page) is in its README and PAGE.md there.

`page.c`'s comments still describe the loader's own row (MODS, a lightning bolt); in TELOS the row is
`a4mk2/row.s`'s.

To update them, copy the files from a newer elekloader and note its commit here. A change TELOS needs in
them belongs in elekloader first.
