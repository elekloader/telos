# The Rytm MKII's Drive Loader (vendored)

Every file here is elekloader's `mods/drive-loader-ar2/` file of the same name, unchanged, from
[elekloader/elekloader](https://github.com/elekloader/elekloader) commit `d208dbd` (drive-loader-ar2
0.11 with the `*` mark for saved data and APPLY & RESTART, GPL-2.0-or-later; the Analog Rytm MKII's
+Drive mods loader, by Iced-Cream). Its `page.s`, `mod.json`, `tools/` and docs (`README.md`,
`PAGE.md`, `STAGE.md`) are not copied: TELOS for the Rytm builds these sources with its own
`ar2/row.s` and `ar2/mod.json`. The loader's design (the `/mods` route, the boot link and the MODS
page) is in its README and PAGE.md there.

`page.c`'s comments still describe the loader's own row (MODS, a lightning bolt); in TELOS the row is
`ar2/row.s`'s.

To update them, copy the files from a newer elekloader and note its commit here. A change TELOS needs in
them belongs in elekloader first.
