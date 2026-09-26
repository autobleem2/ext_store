# AutoBleem Store

An extension of the [AutoBleem](https://github.com/autobleem2/autobleem) launcher: it downloads and installs
Apps and games, on every system AutoBleem runs on (the PlayStation Classic with the AutoBleem kernel's WiFi, the
Raspberry Pi, the PC USB stick, Windows).

- **What it offers** comes from sources:
  - AutoBleem's own catalog on the download site (Apps, and homebrew and freeware games whose licences allow
    it);
  - any number of **TSV sources** you add: a file dropped in `System/Extensions/store/sources/`, or a URL
    added in the Store's Sources tab (kept in `System/Extensions/store/sources.txt`). Cross on a source you
    added renames it, changes its address, switches it between http:// and https://, or removes it.
- **Apps** are AutoBleem's multi-platform App folders. The Store downloads the package for your system and
  merges it into `Apps/<name>/`, keeping other systems' binaries and your own `pad.ini`.
- **Games** go into `Games/<title>/`, and the launcher's scan takes it from there.
- **Pictures**: a game shows its cover from AutoBleem's own covers databases and PlayStation rdb (by the
  `serial` a source gives, else by the title the rdb knows), an App its icon (the catalog's `image`, then the
  installed App's own). A source's `image` is used only for a game those databases do not know. Nothing is
  fetched from libretro's servers. A source shows its server's icon in the Sources tab: a `<link>` from the
  site's root page, or `favicon.ico` as a fallback.
- Downloads run in the background, also after you leave the Store. A game launch or a power-off only pauses
  them, and a stopped download resumes where it stopped.

## Installing

Unpack `ext_store-<system>-<version>.zip` onto the stick (or the Pi's data partition, or the Windows data
folder), so that there is an `Extensions/store/` folder. Then open it from the launcher: L2+R2 -> Extensions
-> AutoBleem Store. It needs a network connection.

## The TSV format

UTF-8 text, one file per line, tab-separated:

```
# autobleem-store 1
# name: My Homebrew
kind	title	url	size	sha256	disc	serial	image	version	description
ps1	Some Game	https://example.org/some-game-d1.chd	412334080		1
ps1	Some Game	https://example.org/some-game-d2.chd	398442496		2
app	Some App	https://example.org/someapp-psc-1.2.zip						1.2
```

- **The header line** names the columns; they come in any order, and unknown ones are ignored. Without a
  header a line is `title<TAB>url[<TAB>size]` of a PS1 game.
- **Lines with the same kind and title are one item**, their files ordered by `disc`.
- **`size` and `sha256` are optional**: when given, a download that does not match them is refused.
- **Formats**: zip, 7z and tar.gz archives, and plain disc images (chd, pbp, cue/bin, img). RAR is not
  supported.
- **The NoPayStation list layout is read as well**: `Name`, `PKG direct link`, `File Size`, `Title ID`,
  `SHA256`. Each regional release (Title ID) is an item of its own. Its links are PSN packages (`.pkg`),
  which are not disc images: such items are listed as "Not installable" and never downloaded.

**You are responsible for what your sources contain.**

## Building

The Store is built with the launcher, which builds it as a plugin against its own copy of the AutoBleem SDK:

```
cmake -S <launcher checkout> -B build -DAB_EXTENSION_DIRS=<this checkout>
```

The result is staged in `build/extensions/store/`. `AB_STORE_CATALOG=<url>` makes the Store read another
catalog (a test site).

## Licence

GPL-3.0-or-later, as AutoBleem. Nothing in this repository comes from Project Eris or its PSC Store.
