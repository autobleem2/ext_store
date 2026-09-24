# ext_store - developer context

The AutoBleem Store: an extension (a plugin) of the AutoBleem launcher. The design is the launcher's
`docs/store-plan.md` and `docs/extensions-plan.md`; the project-wide rules are autobleem-main's `docs/decisions.md`
(TSV sources are unlimited, the Store only pulls, no Project Eris code, extensions are installed by hand and are
separate downloads, repository names `ext_<name>`).

## Layout

- `src/store_service.*` - `StoreService`, the model:
  - the sources: our catalog, `sources/*.tsv`, and the URLs in `sources.txt` (a line is `URL[<TAB>name]` -
    the name the user gave it, shown as `StoreSourceInfo::displayName`; the items keep the list's own name
    inside, so a rename or a new address does not lose what was installed or queued from it:
    `renameSource`/`changeSourceUrl`, `validSourceUrl` refuses a blank or no server), remote ones cached;
  - the entries and their states;
  - `installed.tsv`, and the persisted `queue.txt`;
  - one worker thread at the lowest priority that downloads (resumable, stoppable) and installs through
    core's `AppInstaller`/`GameInstaller` - it starts only after one full refresh of the sources;
  - a **sources thread** of its own, so reading a source never waits behind a download. The first read
    shows the cached copies at once (`cache/catalog.json`, `cache/source-<md5 of the URL>.tsv`) and then
    fetches each source afresh, replacing its copy as it arrives (`StoreSourceInfo::loading` while it is
    read). `addSourceUrl` fetches only the new URL; `removeSourceUrl` fetches nothing, and its items go at
    once. Opening the screen refreshes only when the last refresh is over five minutes old
    (`refreshIfOlderThan`); Square forces one.
  
  It has no UI. It is tested in `tests/test_store_service.cpp` against a fake site.
- `src/store_pictures.*` - `StorePictures`: an item's picture on a thread of its own (never behind a download).
  A game's cover comes from **our** sources only (the owner's rule): the installed folder, then the covers
  databases by serial (the source's, or the one the PlayStation rdb gives for the title), then RetroArch's
  local box art by the rdb's record name; the source's `image` URL only for a game none of them knows, and
  never libretro's servers. An App's icon is the installed `app.ini`'s `Image=`, else the catalog's `image`.
  Fetched pictures go to `cache/pictures/`. Tested in `tests/test_store_pictures.cpp`.
- `src/gui_store.*` - `GuiStore`, the screen: the tabs Apps / Games / Downloads / Sources, the list (each item
  with its picture, the one downloading with a progress bar) and the detail pane (the picture on top), in the
  launcher's `PanelStyle`. It re-reads the service twice a second and never calls `poll()`, whose events are
  the extension's (the launcher's reloads). A reload keeps the cursor on its row - by key, or by place for the
  keyless "Add a source URL" row and for a row that went (a removed source). The Apps and Games lists page with
  L2/R2 and Left/Right, show one source at a time with Select (round to all of them), and filter by a search
  with Start (any case, part of the title); Circle widens a narrowed list before it closes the Store. A spinner
  turns in the line under the header while sources are read, and at the end of a source's row. Removing a
  source asks first.
- `src/store_extension.cpp` - `StoreExtension`: `AB_EXTENSION`, the service's config from `Env` (the catalog
  URL, `store_download_command`, the platform keys; `AB_STORE_CATALOG` overrides the catalog), `poll()` →
  the launcher's bubble, `requestRescan`/`reloadApps`, and `suspend`/`resume`/`shutdown` → the service's
  pause/stop.
- `lang/` - its strings in the 16 languages (English is the source). It also carries the strings the launcher
  translates already, copied from the launcher's files so the wording is the same. Every string change goes
  into all 16 files in the same commit.

## Building and testing

- Built with the launcher: `-DAB_EXTENSION_DIRS=<this checkout>`. `ab_add_extension` builds `store` and
  stages it in `<build>/extensions/store/`; `test_store_service` and `test_store_pictures` are added when the build has tests.
- Format with the launcher's `.clang-format` (copied here), lint with its `.clang-tidy`
  (`tools/lint.sh <files>` from the launcher).
- To try it on the Windows dev build:
  1. Serve a catalog with `python -m http.server`.
  2. Set `AB_STORE_CATALOG=http://127.0.0.1:<port>/catalog.json`.
  3. Stage the stick with `tools/make_usb.py`.
  4. Drive it with `tools/ab_drive.py`: L2+R2 → menu item 6 → the Extensions list → Cross.

## Rules learnt while building it

- No function-local static in an inline function of a header the Store uses. On Windows it is one per DLL:
  an inline `Gui::getInstance()` made the Store open a second window (fixed in core; `getInstance` is out of
  line now).
- The launcher exports the whole SDK (`--whole-archive`). Anything in core the Store calls is there even when
  the launcher never calls it.
