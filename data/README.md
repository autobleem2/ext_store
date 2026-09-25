# The Store's data files

Shipped with the extension, in `Extensions/store/data/`.

## psn_serials.tsv

A PlayStation Store **Title ID** (`NPUF30001` - what NoPayStation's `PSX_GAMES.tsv` gives a game) and the
**disc serial** of the same game (`SLPS-00624`), which is what our covers databases and RetroArch's
PlayStation rdb know a game by. `StorePictures` looks a PSN item's cover and facts (publisher, year, players)
up by the serial this list names; without it only the title could be tried.

Tab-separated, with a header; the Store reads the `Title ID` and `Serial` columns by name and ignores the rest:

| column | |
|---|---|
| `Title ID` | the PSN Title ID |
| `PSN Name` | the title as the PlayStation Store lists it |
| `Region` | the PSN release's region |
| `Match Type` | how the serial was found: `exact`, `fuzzy:<score>` (with `(diff region)` when the serial is another region's release of the game), or `none` |
| `Redump Title` | the disc's title as Redump lists it |
| `Serial` | the disc serial; a multi-disc game lists each disc's, `SLUS-00453 / SLUS-00561 / ...` - the first is the cover's |

Given by the owner on 2026-09-25 (1494 Title IDs, 1475 with a serial; 1289 of those have a cover in our
covers databases). Replace the file to update it - nothing else refers to its contents.
