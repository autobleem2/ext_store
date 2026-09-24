# abstored - the AutoBleem Store's LAN server

`abstored` serves a folder of PS1 games to the AutoBleem Store on the same network. The Store lists them with
their covers and installs them onto the stick, the Raspberry Pi or the PC.

It uses AutoBleem's own engine to find the games: the disc images, the serial read from each image, and the
title and cover from the covers databases and the PlayStation rdb. **It only reads the games folder**: nothing
is renamed, repaired or written there.

**Setting it up on a Linux machine, step by step - the build, a systemd service, the firewall - is
[INSTALL-linux.md](INSTALL-linux.md).**

## Building (any Linux)

You need a C++14 compiler, CMake 3.14 or newer, git and pthreads. SDL and other libraries are not needed.

```
cmake -S server -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This fetches autobleem-core (develop) from GitHub. To use a checkout you already have, pass
`-DAB_CORE_DIR=<path to autobleem-core>`. The program is `build/abstored`. Optionally, `cmake --install build`
puts it in `/usr/local/bin`.

## Running

```
abstored <games folder> [--port 8124] [--name "My games"] [--covers <folder with coversU/P/J.db>]
         [--rdb "<Sony - PlayStation.rdb>"] [--state <folder>] [--no-checksums]
         [--allow-uploads [--upload-token <token>]]
```

- Open `http://<this machine>:8124/` in a browser. The page shows the source's URL, every game served, and
  every problem the scan found: a cue naming a missing file, an empty image, a game without a serial.
- In the Store: **Sources** → **Add a source URL** → `http://<this machine>:8124/store.tsv`.
- **The games folder** holds one folder per game. Games may sit in sub-folders at any depth, and `!` folders
  (`!SaveStates`, `!MemCards`) are skipped. An AutoBleem stick's `Games` folder can be served as it is.
- **The folder is scanned again** whenever something in it changes (checked every 10 s), or when you open
  `/rescan`.
- **Checksums**: every file's SHA-256 is worked out in the background, once, and cached in `--state` (by
  default `~/.cache/abstored`). The Store checks each download against it. Until a file's sum is ready it is
  served without one. `--no-checksums` skips this, for example on a slow machine with a large library.
- **Covers**: a picture in the game's folder, or the covers database's picture by serial (`--covers`). The
  Store also finds covers in its own databases by the serial the server reports.
- **Uploads** (off by default: the folder is only read). `--allow-uploads` lets **AutoBleem LAN Share** (the
  Windows app, autobleem-pc-tools) put games into the folder over the network - a disc it read, or games it
  found on the PC. Every upload needs the token: `--upload-token` sets it, otherwise one is made once, kept in
  `<state>/upload-token` and printed at start. A game arrives in a `.uploading` folder first and moves into
  the games folder only when it is complete, under a free name (" (2)" when the name is taken). The folder
  must then be writable by the user abstored runs as.
- **Stopping**: Ctrl+C, or SIGTERM.

### As a service (systemd)

```
[Unit]
Description=AutoBleem Store LAN server
After=network-online.target

[Service]
ExecStart=/usr/local/bin/abstored /srv/games --name "Living room"
User=games
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## What it serves

| URL | |
|---|---|
| `/` | the status page |
| `/store.tsv` | the Store's source (the TSV format in the Store's README), every URL built from the address the client used |
| `/files/<path>` | the games' files, only those the scan listed (`Range` supported: a stopped download resumes) |
| `/cover/<game folder>` | a game's cover |
| `/rescan` | scan now |
| `/status.json` | what the status page shows, for a program: the games with their files, the problems, the folder's free space, whether uploads are on |
| `/upload/<game folder>/<file>` | with `--allow-uploads` and the token (`X-AB-Token`): `PUT ?offset=N` appends to the staged file (a stopped upload goes on), `GET` says how much is staged |
| `/upload/<game folder>` | `POST ?commit` moves the staged game into the folder and rescans; `DELETE` drops it |

**Plain HTTP, for a home network.** Anyone who can reach the port can read the games it serves, and with
uploads on, anyone who has the token can add to them. Do not expose it to the internet, and serve only games
you may share.

## Code

- `src/main.cpp`: the arguments, the routes, the watcher and the hasher. That is all that is here.
- The server itself is **autobleem-core's `ableem_lanserver`** (`lib_ableem/include/ableem/lanserver/`), shared
  with pc-tools' LAN Share for Windows:
  - `lan_library.*`: the scan (read-only), the checksum cache and the TSV.
  - `http_server.*`: a small HTTP/1.1 server (GET and HEAD, one connection per request, Range).
  - `index_page.*`: the status page.

Tests: `tests/core/test_lan_server.cpp` in autobleem-core.
