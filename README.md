# FArchive 1.7.0 — GEM archiver for TOS, MultiTOS, MiNT and PC GEM

`farchive.prg` extracts and makes archives in **ZIP, LHA (.LZH), ZOO, ARC,
LZX, TAR, TAR.GZ (.TGZ) and GZ**. It works like FConv: it starts as an
icon (next to FView's and FConv's, if you use them), takes drops the MultiTOS way (AP_DRAGDROP) and the
AV way (VA_DRAGACCWIND), VA_START and the command line, opens into the
same log window and shows the same non-modal progress dialog (all tasks,
the current task with the member being done, Cancel). Under MiNT each task
runs in its own `fawork.ttp` at nice 19; on TOS it runs inside FArchive.

- **Drop archives on the icon** to extract them into the destination
  folder (each into a folder of its own with *Folder per archive*), or —
  with *Options › Extract dropped archives* off — to open each in a window.
- **Drop other files or folders on the icon** to pack them into a new
  archive: FArchive asks for its format (with *Best compression*, set to
  the last ones used), then for its name. *File › New archive...* asks the
  same way. An archive that is opened or extracted is read as what its
  contents show, whatever its extension; its window's info line shows the
  format, and files added to it go in that format.
- **Archive windows** (*File › Open archive...*, or *New archive...* for an
  empty one) list the members with size, packed size, method and date.
  Click to select, Shift-click to add or take away one, Control-click for
  everything from the last click; the cursor keys move the selection and
  Shift with them selects from where it started (also Page Up/Down, Home,
  End); *Edit › Select all* (^S) and *Select none* (^Z).
  *Archive › Extract selected* (^T) extracts
  just the selected members and *Archive › Extract all* (^Y) the whole
  archive; double-click a member to extract just that one. *Archive ›
  Find...* (^F) selects every member whose path contains the name typed
  (or, with `*` and `?`, has a file or folder name that matches) and shows
  the first; *Find next* (^G) goes on to the next one. *Archive › Check
  8.3 names* (^K) tells whether the archive is safe to extract on TOS as it
  is: members whose names (or folders) aren't valid 8.3 names are selected
  and listed in a window of their own (and in the log).
- **Copy between archives**: drag selected members from one archive window
  to another; they are extracted to a temporary folder next to FArchive and
  added to the other archive in its own format (two tasks, with progress).
  *Edit › Copy* (^C) or *Cut* (^X) the selected members, then *Paste* (^V)
  in another archive window does the same; after a Cut they are deleted
  from the first archive once they are safely in the second.
- **Rename**: *Edit › Rename...* (^R) shows the selected member's path;
  change its file name, or the name of any one folder on the way (that
  folder and everything in it is renamed). Handy for
  making an archive TOS safe before extracting it (see *Check 8.3
  names*). The members are extracted to a temporary folder and added again
  under the new name, the old ones taken out in the same rewrite.
- **Fix 8.3 names**: *Archive › Fix 8.3 names...* (^J) gives every file
  and folder whose name isn't a valid 8.3 name (or would clash with
  another once TOS makes them capitals) one, `Long File Name.txt` →
  `LONGFI_1.TXT`, in the archive itself, so it extracts anywhere as it
  is. The new names are listed in the log and it always asks first. Like
  Rename, the archive is only rewritten if every file came out and went
  back in; empty folders keep their names. Not for ARC (8.3 already) or
  GZ.
- **Filter**: *Archive › Filter...* (^L) lists only the files whose names
  match, e.g. `*.jpg` or `*.jpg;*.png` (folders are hidden while a filter
  is on; the info line shows it). Select all, Find and the cursor keys
  work on what is shown; *Show all* takes the filter away.
- **Delete**: *Edit › Delete...* (Delete key) takes the selected members
  (and everything in a selected folder) out of the archive, which is
  written again without them. Not for GZ files (one file each), nor
  members merged into an LZX group.
- **Confirm changes** (*Options*, on by default): copying, moving and
  deleting members ask first, in an alert window that doesn't stop
  anything else; so does adding anything that would replace members
  already in the archive (*Replace*, *Keep old* or *Cancel*).
- **Skip Mac files** (*Options*, on by default): `.DS_Store`, the 4 KB
  `._name` files and the `__MACOSX` folder that macOS puts in archives are
  left out when extracting and when adding. Without a
  destination folder set, FArchive asks for one first. **Files and
  folders dropped on an archive window are added to it**, as is what
  *Archive › Add files...* picks (no name in the file selector = the folder).
  Members of the same name are replaced when *Replace existing files* is on.
- **8.3 names**: extracting to a FAT drive (TOS, or MiNT's FAT file
  systems — checked per drive with `Dpathconf`), member and folder names
  that are not valid 8.3 names are renamed (`Long File Name.txt` →
  `LONGFI_1.TXT`, clashes get `_2`, `_3`...; no `~`, which TOS doesn't take) and each rename is listed in
  the log. Drives with long names (ext2, MinixFS...) get the names as they
  are. *Options › 8.3 names on FAT drives* switches this off.
- Paths in archives are made safe: drive letters and `..` are dropped.
  *Keep folders* off extracts everything flat into one folder.
- **The log** lists every task with what was extracted, added, skipped
  (already there and *Replace* off, links, unknown methods), renamed or
  failed (with CRC errors), and a summary per batch. It is also appended to
  `FARCHIVE.TXT` in the destination folder — **only on a hard disk**: when
  the destination is on A: or B: it goes next to the program instead, and
  when that is a floppy too, no log file is written.
- Settings are kept in `FARCHIVE.INF` next to the program. Menus and
  dialogs are in the resource file `farchive.rsc`, which goes next to the
  program (`make rsc` builds it from `rsc/lang/en.txt`). About, the new
  archive's format and Find are windows of their own, so nothing waits for
  them.

| Format | Reads | Writes |
|---|---|---|
| ZIP | stored, deflate | deflate (stored for folders and empty files) |
| GZ, TAR.GZ | deflate, several members | deflate (*Best compression*: slower, a little smaller) |
| TAR | ustar, GNU long names, pax paths | ustar with GNU long names |
| LHA | -lh0- -lh1- -lh4- -lh5- -lh6- -lh7- -lzs- -lz4- -lz5- -lhd-, header levels 0–3 | -lh5- (level 1 headers, Atari LHarc style paths), -lh0- when it does not shrink |
| ZOO | stored, LZW, lh5 | LZW, stored when it does not shrink |
| ARC | stored, packed, squeezed, crunched (5–8), squashed | crunched (method 8), stored when it does not shrink; no folders (8.3 names) |
| LZX | stored, LZX (verbatim and aligned blocks, merged groups) | LZX, 64K window |

A GZ file holds a single file; use TAR.GZ for more. Adding to an archive
writes a new one next to it (`FARC$$$$.TMP`) and then replaces it, so
there must be room for both.

## PC GEM

`pcgem/FARCHIVE.APP` is FArchive for PC GEM (FreeGEM / OpenGEM on DOS): a
32-bit DJGPP program with its own copy of the PC GEM layer (`pcgem/`),
the same sources and the same resource file as on the Atari. Install it
in a folder of its own (e.g. `C:\FARCHIVE`) with `FARCHIVE.RSC` and
`CWSDPMI.EXE`. On PC GEM everything runs in FArchive itself (no
background worker), and long file names are used where DOS has them.

## Building

Needs the MiNT cross toolchain with mintlib and gemlib (`m68k-atari-mint-gcc`,
or the ELF toolchain with `make CROSS=m68k-atari-mintelf-`):

```
make                    # release and log builds for every CPU
make release            # release builds only
make VARIANTS=000       # only the 68000 builds (or 030, 030fpu, 040, cf)
make arctest            # host test tool for the archive layer (native gcc)
```

| Build | Folder | Runs on |
|---|---|---|
| 68000 | `release/`, `log/` | every Atari |
| 68030 | `release/030/`, `log/030/` | TT, Falcon, 68030 accelerators (no FPU needed) |
| 68030 + 68882 | `release/030fpu/`, `log/030fpu/` | the same with an FPU (FArchive does no floating point, so this is the 030 build again) |
| 68040 | `release/040/`, `log/040/` | 68040 machines and accelerators |
| ColdFire | `release/cf/`, `log/cf/` | FireBee |

Each folder holds `farchive.prg` and `fawork.ttp` — **copy both of one
build into the same folder** (`fawork.ttp` is the MiNT background worker;
without it FArchive still works, cooperatively). The log builds write
`FARCHIVE.LOG` next to the program (cleared at each start). No resource
file is needed — menus, dialogs and the icon are built at run time.

`test/arctest` lists (`l`), extracts (`x`, `-8` to act as if the
destination were a FAT drive, `-n` for no folders, `-s N` for member N
only) and adds (`a ARCHIVE FMT FILES...`) on the host.

## Source

| File | |
|---|---|
| `farchive.c` | the GUI: icon and log window, archive windows, menus, drag & drop (`AP_DRAGDROP`, `VA_START`, `VA_DRAGACCWIND`), task queue and progress dialog |
| `arcjob.c` | what a job does: list, extract, add |
| `archive.c` | detection, listing, extraction with 8.3 names, adding; TAR, TAR.GZ and GZ |
| `arcfmt.c` | ZIP, LHA, ZOO, ARC and LZX member headers |
| `arcfs.c` | file system calls; `Dpathconf` to tell FAT from long-name drives |
| `arcio.c`, `codecs.c` | byte and bit streams, CRCs, Huffman codes; which codec a method uses |
| `deflate.c`, `inflate.c`, `lzh.c`, `lzw.c`, `lzx.c` | the codecs: deflate, LHA's lh1–lh7/lzs/lz5, ZOO and ARC's LZW/RLE/squeeze, Amiga LZX |
| `job.c`, `jobcore.c`, `worker.c` | the job system from FView: `fawork.ttp` workers (Pexec, pipes, `U:\SHM`) and cooperative TOS jobs |
| `util.c`, `fview.h`, `gemui.h`, `doc.h` | shared definitions from FView |

## What has been tested, and what to check on the machine

- **The archive layer on a host** (`make arctest`): archives FArchive
  wrote were checked with Info-ZIP `unzip -t`, GNU tar and gzip, lhasa,
  ZOO 2.10, ARC 5.21 and an LZX extractor; archives from those tools — and
  lhasa's test archives from about 30 LHA programs, Atari LHarc 3.13
  included — extracted byte for byte; 8.3 renaming checked with a forced
  FAT mode. Run under AddressSanitizer, including 175 truncated and
  corrupted archives. ARC's old methods 5–7 have no test files.
- **Type-checked for the 68000, 68030 and 68040** against the real mintlib
  and gemlib headers.
- The binaries in `release/` and `log/` were built with the MiNT cross
  toolchain from this source.
- **Not yet run on TOS/MiNT**: the GUI, the worker and `Dpathconf` on real
  drives are worth watching on first runs.

Known limits: LHark's lh7 variant, `-lhx-`, MacBinary-wrapped MacLHA files
and level-2 headers over 4 GB are not read; empty folders are not stored in
LHA, ZOO or LZX archives.

## Licence

FArchive is released under the MIT License; see `LICENSE`.
Copyright (c) 2026 Francis C Bullen.
