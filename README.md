# MenuMusicNX

HOME Menu background music for the Nintendo Switch.

<p align="center">
  <a href="https://github.com/Fimochi/MenuMusicNX/releases">
    <img src="assets/logo.png" alt="Logo" width="400">
  </a>
</p>

MenuMusicNX plays music **only while the HOME Menu is in the foreground**. Launch a game and playback pauses automatically with a **smooth fade-out effect**. Press HOME over a running game and your music resumes from the same position with a **smooth fade-in effect** — no manual toggles required.

> **Note:** This project was developed with assistance from AI coding tools (Cursor), based on ideas and testing by the maintainer. The focus-detection approach builds on community research around `pdm:qry` play events ([sys-tune #55](https://github.com/HookedBehemoth/sys-tune/issues/55)).

## Fork lineage

MenuMusicNX is a **fork of [sys-tune](https://github.com/HookedBehemoth/sys-tune)** by HookedBehemoth. It reuses sys-tune's audio pipeline (dr_libs decoding, `audout` playback, Tesla overlay shell, and sysmodule IPC) and strips away per-title play/pause configuration in favour of automatic HOME Menu detection.

## Manual notes from me, Fimochi
- These notes are one of the only things that are not ai in this project
- if there ARE any bugs (which i doubt), then pls remember that this is open source so u can make changes, and also i made this in less than like 2 hours with ai so dont judge me pls thx :)
- I'm too stupid to code this so i asked cursor to make this cuz i felt like it
- I mean i created the images myself i didnt use ai for that because ai images suck
- I also asked it to add a fade in and out effect for when you enter or exit a game so that it feels polished, and it did in the release 1.0.1, so this is a good fork with an advantage over others, and the fade is SO COOL
- I know its ai coded so if u dont like that for some reason then you dont have to use it but its really good i swear
- Also i just wanted to add that i now that some people are heavily AGAINST AI, and if you really hate this project for sthis or some other reason then feel free not to use it. but imo, i think that ai is REALLLY GOOD, as long as its used properly for things that people will find useful (like making projects like these that people cant or wont code themselves), and not for stupid stuff like ai videos or copilot slop. anyway, thats just me. ai shouldnt replace the human, it should be a tool to work alongside it, and for this project there ARE a few things i did myself, like setup this repo and make the assets in /assets. anyway, thats all from me on this topic.
- O yeah this will also overwrite your sys-tune install if you have it so id recommend uninstalling that first if u wanna use this
- anyway thats all from me i hope you enjoy FINALLY having proper support for home menu music like the wii and wii u and 3ds and MORE

### What changed from upstream sys-tune

| Upstream sys-tune | MenuMusicNX |
|---|---|
| Per-game play/pause toggles | Automatic — HOME only |
| Manual play/pause in overlay | Removed |
| Title-based config (`[title]` in ini) | Ignored for playback |
| `pmdmntGetApplicationProcessId` only | `pdm:qry` focus events + qlaunch detection |
| Games paused music, applets kept it playing | **Applets pause too** (Settings, Album, hbmenu, ...) with a `pause_on_applet` toggle |
| No fade transition | **Smooth fade-in/out when entering or leaving games** |

## Requirements

- Nintendo Switch on **Atmosphère** (tested on firmware **20.1.5**)
- [devkitA64](https://devkitpro.org/) / libnx (for building)

## Installation

1. Download a packaged release, or build your own (see [Building](#building)).
2. Extract to the **root of your SD card**.
3. Reboot the console (or restart title `4200000000000000` from the Toolbox).

Layout after install:

```
atmosphere/contents/4200000000000000/exefs.nsp   ← sysmodule
atmosphere/contents/4200000000000000/toolbox.json
atmosphere/contents/4200000000000000/flags/boot2.flag
switch/.overlays/sys-tune-overlay.ovl            ← Tesla overlay
```

4. Add music via Tesla → **MenuMusicNX** → **Music browser** (`.mp3`, `.flac`, `.wav`).
5. Optional: press **ZR** on a file or folder in the browser to set a **startup playlist** (auto-loaded on boot).

Config is stored at `sdmc:/config/sys-tune/config.ini` (path kept for compatibility with upstream sys-tune).

Keys read from that file:

| Key | Default | Meaning |
|---|---|---|
| `pause_on_applet` | `1` | Pause playback while a system/library applet is in the foreground |
| `focus_log` | `0` | Append pdm focus events and playback decisions to `sdmc:/config/sys-tune/focus.log` |
| `volume` | `1.0` | Playback volume (mirrored by the overlay slider) |
| `shuffle` / `repeat` | `0` / `1` | Shuffle and repeat mode, mirrored by the status bar buttons |
| `load_path` | *(empty)* | Startup file or folder, set with **ZR** in the browser or playlist |

The sysmodule re-reads these values whenever the focus event log changes, so a toggle flipped in the overlay applies the next time a game or applet takes the foreground.

## Usage

| Situation | Behaviour |
|---|---|
| HOME Menu (no game) | Music plays |
| Game running | Music pauses |
| System applet open (Settings, Album, eShop, hbmenu, ...) | Music pauses |
| Tesla overlay open (this menu, sys-clk, ...) | Music keeps playing |
| HOME pressed over a game | Music resumes (same position) |
| Return to game | Music pauses |
| Close game on HOME | Music continues |

Open the overlay with **L + D-Pad Down + Right Stick click** (default Tesla binding), then select **MenuMusicNX**.

Applet pausing can be switched off with **Pause in applets** in the overlay (or `pause_on_applet=0` in the config file): music then keeps playing while Settings, the Album or applet-mode homebrew are open.

## Focus detection

Playback follows the `pdm:qry` focus event log, polled every 50 ms:

- qlaunch (HOME Menu) has focus → music plays (fade-in);
- the running application (game, or homebrew in application mode) has focus → music pauses (fade-out);
- any other system/library applet has focus (Settings, Album, eShop, hbmenu, ...) → music pauses;
- the overlay applet used by Tesla/nx-ovlloader is ignored, so opening this menu never stops the music;
- if `pdm:qry` is unavailable the old rule applies: music plays while no application is running.

Tracks keep their position while paused, so playback resumes exactly where it left off. Set `focus_log=1` in the config to dump every raw focus event and the resulting decision to `sdmc:/config/sys-tune/focus.log`.

## Building

```bash
git clone https://github.com/Fimochi/MenuMusicNX.git
cd MenuMusicNX
make
make dist   # optional: produces dist/ zip ready for SD card
```

The required [libtesla](https://github.com/WerWolv/libtesla) headers are vendored in `overlay/lib` so builds do not depend on a moving submodule revision.

## Stability hardening

Version 1.1 focuses on sysmodule and overlay reliability:

- validates and terminates all paths received over IPC;
- fixes failed MP3/WAV decoder initialization and cleanup;
- synchronizes decoder lifetime, seeking, queue changes, and shutdown;
- prevents empty-playlist underflow and invalid queue moves;
- bounds-checks browser paths and short filenames;
- handles missing `pdm:qry` and headphone GPIO services without aborting;
- uses bounded audio waits so the sysmodule can always shut down;
- removes legacy foreground-game volume manipulation;
- avoids invalid progress calculations for empty or corrupt tracks.

## Project structure

```
MenuMusicNX/
├── sys-tune/          Atmosphère sysmodule (audio + HOME detection)
├── overlay/           Tesla overlay (playlist / browser / volume)
├── common/            Shared config, SDMC, process management
├── ipc/               Client library for overlay ↔ sysmodule IPC
└── Makefile           Top-level build
```

HOME Menu detection lives in `common/pm/pm.cpp` (`IsHomeMenuForeground()`).

## Credits

- **[sys-tune](https://github.com/HookedBehemoth/sys-tune)** — HookedBehemoth, TotalJustice, and contributors; original sysmodule and overlay
- **[dr_libs](https://github.com/mackron/dr_libs)** — mackron; audio decoding
- **[libtesla](https://github.com/WerWolv/libtesla)** — WerWolv; overlay UI
- **[sys-clk](https://github.com/retronx-team/sys-clk)** — process ID helpers
- **masagrator** — `pdm:qry` focus detection notes ([sys-tune #55](https://github.com/HookedBehemoth/sys-tune/issues/55))

## License

See [LICENSE](LICENSE) (inherited from upstream sys-tune).
