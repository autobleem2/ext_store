# abstored on Linux: building it and running it as a service

This guide is for someone who wants to share their own PS1 games with the AutoBleem Store over their home network.
It sets up `abstored` on a Linux machine that is always on: a home server, a NAS that runs Debian, or a
Raspberry Pi. It covers building the program, trying it by hand, and then making it start by itself as a
systemd service.

What `abstored` does and what it serves is in [README.md](README.md). In short, it reads a folder of games,
never writes to it, and offers the games to the Store at `http://<this machine>:8124/store.tsv`.

- [1. What you need](#1-what-you-need)
- [2. Build it](#2-build-it)
- [3. Try it by hand](#3-try-it-by-hand)
- [4. Covers and titles (optional)](#4-covers-and-titles-optional)
- [5. Run it as a service](#5-run-it-as-a-service)
- [6. Let the network reach it](#6-let-the-network-reach-it)
- [7. Add it to the Store](#7-add-it-to-the-store)
- [8. Updating and removing](#8-updating-and-removing)
- [9. Without root: a user service](#9-without-root-a-user-service)
- [10. When something is wrong](#10-when-something-is-wrong)

## 1. What you need

- A Linux machine on the same network as the console, Raspberry Pi or PC that runs AutoBleem. Any
  architecture works: x86-64, 32-bit x86, 32-bit and 64-bit ARM.
- A folder of PS1 games, with one folder per game (see [README.md](README.md), "The games folder"). An
  AutoBleem stick's `Games` folder can be used as it is.
- About 200 MB of free space for the build, which you can delete afterwards.
- These packages for building:

  | Distribution | Command |
  |---|---|
  | Debian, Ubuntu, Raspberry Pi OS, Linux Mint | `sudo apt install build-essential cmake git` |
  | Fedora | `sudo dnf install gcc-c++ make cmake git` |
  | Arch, Manjaro | `sudo pacman -S --needed base-devel cmake git` |
  | openSUSE | `sudo zypper install gcc-c++ make cmake git` |

  CMake must be version 3.14 or newer; `cmake --version` tells you. Every current distribution has one.
  Nothing else is needed: no SDL and no libraries. The server uses only AutoBleem's own engine, which is
  built from source along with it.

> A ready-made program is also published for each architecture: `abstored-linux-<arch>-<version>.tar.gz`
> on the [ext_store releases page](https://github.com/autobleem2/ext_store/releases). If you use it,
> unpack it, put `abstored/abstored` in `/usr/local/bin`, and continue at [3. Try it by hand](#3-try-it-by-hand).

## 2. Build it

```sh
git clone https://github.com/autobleem2/ext_store.git
cd ext_store
cmake -S server -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

- The first `cmake` step downloads AutoBleem's engine (`autobleem-core`, its `develop` branch) from GitHub.
  If you already have a checkout of it, add `-DAB_CORE_DIR=/path/to/autobleem-core` to that line and nothing
  is downloaded.
- The build takes a minute or two on a PC and about ten minutes on a Raspberry Pi 4.
- `cmake --install` copies the program to `/usr/local/bin/abstored`. To put it somewhere else, add
  `--prefix /some/where` to the install command.

Check that it runs:

```sh
abstored --version
```

It should print `abstored 1.0`. After this, you can delete the `build` folder.

## 3. Try it by hand

Start it on your games folder:

```sh
abstored /srv/games --name "Living room"
```

- `--name` is what the Store shows as the source's name.
- It prints one line for each scan: how many games it found, how many errors and how many warnings.
- Open `http://<this machine's address>:8124/` in a browser, from any device on the network. The page lists
  every game it serves and every problem the scan found. Common problems are a `.cue` that names a missing
  file, an empty image, or a game with no serial.
- Stop it with Ctrl+C.

Every option is described in [README.md](README.md), "Running". The ones you may want:

| Option | What it does |
|---|---|
| `--port 8124` | Listen on another port. |
| `--covers DIR` | A folder with `coversU.db`, `coversP.db` and `coversJ.db` - see below. |
| `--rdb FILE` | RetroArch's `Sony - PlayStation.rdb`, for better titles - see below. |
| `--state DIR` | Where the checksum cache is kept. The default is `~/.cache/abstored`. |
| `--no-checksums` | Skip working out each file's SHA-256. The Store can then not check the downloads. |

The checksums are worked out once, in the background, and then cached. On a large library this takes a
while the first time, and until a file's sum is ready it is served without one.

## 4. Covers and titles (optional)

Without these, a game is listed under its `Game.ini` title or its folder's name. It still gets a cover in
the Store if the Store's own databases know its serial.

**The cover databases** are AutoBleem's. They come with every AutoBleem stick (`Autobleem/bin/db/`), and
you can also download them from the download site:

```sh
sudo mkdir -p /usr/local/share/abstored
cd /usr/local/share/abstored
for r in U P J; do sudo curl -fLO "https://autobleem.retromenele.pl/db/covers$r.db"; done
```

Then add `--covers /usr/local/share/abstored` when you start the server.

**RetroArch's PlayStation database** gives the full, region-tagged names. If RetroArch is installed on this
machine, it is in RetroArch's `database/rdb/` folder. Otherwise, copy `Sony - PlayStation.rdb` from an
AutoBleem stick (`RetroArch/bin/database/rdb/`) to `/usr/local/share/abstored/`, then add
`--rdb "/usr/local/share/abstored/Sony - PlayStation.rdb"`.

## 5. Run it as a service

A service starts at boot, comes back if it stops, and runs under an account of its own that can only read
the games.

### 5.1 An account for it

```sh
sudo useradd --system --no-create-home --shell /usr/sbin/nologin abstored
```

(On Fedora and Arch the shell is `/sbin/nologin` or `/usr/bin/nologin`; any of them works.)

The account must be able to **read** the games folder and **enter** every folder on the way to it. Usually
it already can: game files are readable by everyone unless you made them private. To check:

```sh
sudo -u abstored ls /srv/games
```

If you see `Permission denied`, give it read access. One way is to share a group:

```sh
sudo chgrp -R abstored /srv/games
sudo chmod -R g+rX /srv/games
```

It never needs write access. The server does not write to the games folder.

### 5.2 The unit file

Create `/etc/systemd/system/abstored.service` (for example with `sudo nano /etc/systemd/system/abstored.service`):

```ini
[Unit]
Description=AutoBleem Store LAN server
After=network-online.target
Wants=network-online.target
# the games are on a drive that is mounted at boot: wait for it
RequiresMountsFor=/srv/games

[Service]
User=abstored
Group=abstored
ExecStart=/usr/local/bin/abstored /srv/games --name "Living room" \
    --covers /usr/local/share/abstored --state /var/cache/abstored
# systemd creates /var/cache/abstored for it, owned by the account
CacheDirectory=abstored
Restart=on-failure
RestartSec=5

# it only reads the games and writes its cache: nothing else of the system is open to it
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=read-only
PrivateTmp=yes
PrivateDevices=yes

[Install]
WantedBy=multi-user.target
```

Change these lines for your setup:
- **`/srv/games`**: your games folder. It appears in `RequiresMountsFor` and in `ExecStart`.
- **`--name`**: what the Store shows.
- **`--covers`**: remove it if you skipped section 4. Add `--rdb ...` if you have the rdb.

If your games are under `/home`, they still work: `ProtectHome=read-only` lets the service read them.

Start it, and make it start at every boot:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now abstored
systemctl status abstored
```

`status` should say `active (running)`. Its log is the systemd journal:

```sh
journalctl -u abstored -f          # follow it; Ctrl+C to stop following
journalctl -u abstored -b          # everything since this boot
```

- The server scans the folder again by itself whenever something in it changes. It checks every 10 seconds,
  so adding a game needs no restart. To scan at once, open `http://<this machine>:8124/rescan`.
- After you change the unit file, run `sudo systemctl daemon-reload && sudo systemctl restart abstored`.

## 6. Let the network reach it

If the machine has a firewall, open port 8124 (or the port you chose) to your home network only:

| Firewall | Command |
|---|---|
| ufw (Ubuntu, Raspberry Pi OS if installed) | `sudo ufw allow from 192.168.0.0/16 to any port 8124 proto tcp` |
| firewalld (Fedora, openSUSE) | `sudo firewall-cmd --permanent --zone=home --add-port=8124/tcp && sudo firewall-cmd --reload` |

Change `192.168.0.0/16` if your network uses another range, such as `10.0.0.0/8`.

**Do not forward this port on your router.** The server speaks plain HTTP and has no passwords. Anyone who
can reach it can download every game it serves. It is meant for your own network. Serve only games you may
share.

## 7. Add it to the Store

1. Find the machine's address with `hostname -I`. The first address it prints is usually the right one,
   for example `192.168.1.20`.
2. Check the list from another device: `http://192.168.1.20:8124/store.tsv` should show a line for each disc.
3. In AutoBleem, open L2+R2 → **Extensions** → **Store** → **Sources** → **Add a source URL** and enter
   `http://192.168.1.20:8124/store.tsv`.

The address must stay the same, so give the machine a fixed address in your router's DHCP settings (often
called an "address reservation" or "static lease"). If the address does change later, open **Sources** in
the Store, pick the source and choose **Change the address**. What you installed from the source is kept.

## 8. Updating and removing

**Updating** a server you built from source:

```sh
cd ext_store
git pull
cmake -S server -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo systemctl restart abstored
```

The first `cmake` fetches the newest engine again. The checksum cache is kept, so files already hashed are
not hashed again.

**Removing** it:

```sh
sudo systemctl disable --now abstored
sudo rm /etc/systemd/system/abstored.service /usr/local/bin/abstored
sudo systemctl daemon-reload
sudo rm -rf /var/cache/abstored /usr/local/share/abstored
sudo userdel abstored
```

Your games folder is not touched by any of this.

## 9. Without root: a user service

If you cannot or do not want to use `sudo`, the server can run as your own user with a systemd **user**
service.

1. Build it as in section 2, but install it to your home folder:
   `cmake --install build --prefix ~/.local`.
2. Create `~/.config/systemd/user/abstored.service`:

   ```ini
   [Unit]
   Description=AutoBleem Store LAN server

   [Service]
   ExecStart=%h/.local/bin/abstored %h/games --name "My games"
   Restart=on-failure

   [Install]
   WantedBy=default.target
   ```

   `%h` is your home folder. Change `%h/games` to where your games are.
3. Start it:

   ```sh
   systemctl --user daemon-reload
   systemctl --user enable --now abstored
   ```

4. A user service normally stops when you log out. To keep it running with nobody logged in, and to start
   it at boot, run this once (it needs an administrator):
   `sudo loginctl enable-linger $USER`.

The log is `journalctl --user -u abstored -f`. The checksum cache is in `~/.cache/abstored`.

If the machine has no systemd, a line in your crontab (`crontab -e`) starts it at boot instead:

```
@reboot /home/you/.local/bin/abstored /home/you/games --name "My games" >> /home/you/abstored.log 2>&1
```

## 10. When something is wrong

| What you see | What to do |
|---|---|
| `abstored: port 8124 is in use or not allowed` | Another program uses the port, or a port below 1024 was chosen without root. Choose another with `--port`, and use that port in the Store's URL. |
| `... is not a folder` | The games folder path is wrong, or the drive is not mounted yet. For a service, check the `RequiresMountsFor` line. |
| The web page opens on the server but not from other devices | The firewall (section 6), or the server has another address than the one you typed. |
| A game is missing from the list | Open the web page. The game is probably listed under problems, with the reason. A game has to be in a folder of its own, not loose in the games folder. |
| The Store says a download failed its check | The file changed after its checksum was worked out. Open `/rescan`, wait for the hashing to finish (the web page says when), and try again. |
| `Permission denied` in the journal | The `abstored` account cannot read a folder or a file - see section 5.1. |
| The first `cmake` fails to download `autobleem-core` | This machine has no internet access, or no `git`. Clone `https://github.com/autobleem2/autobleem-core` on another machine, copy it over, and pass `-DAB_CORE_DIR=`. |
