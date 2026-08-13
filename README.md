# MenuMusicNX

HOME Menu background music for the Nintendo Switch.

MenuMusicNX plays music **only while the HOME Menu is in the foreground**. Launch a game and playback pauses automatically. Press HOME over a running game and your music resumes from the same position — no manual toggles required.

> **Note:** This project was developed with assistance from AI coding tools (Cursor / Claude), based on ideas and testing by the maintainer. The focus-detection approach builds on community research around `pdm:qry` play events ([sys-tune #55](https://github.com/HookedBehemoth/sys-tune/issues/55)).

## Fork lineage

MenuMusicNX is a **fork of [sys-tune](https://github.com/HookedBehemoth/sys-tune)** by HookedBehemoth. It reuses sys-tune's audio pipeline (dr_libs decoding, `audout` playback, Tesla overlay shell, and sysmodule IPC) and strips away per-title play/pause configuration in favour of automatic HOME Menu detection.

### What changed from upstream sys-tune

| Upstream sys-tune | MenuMusicNX |
|---|---|
| Per-game play/pause toggles | Automatic — HOME only |
| Manual play/pause in overlay | Removed |
| Title-based config (`[title]` in ini) | Ignored for playback |
| `pmdmntGetApplicationProcessId` only | `pdm:qry` focus events + qlaunch detection |

## Requirements

- Nintendo Switch on **Atmosphère** (tested on firmware **20.1.5**)
- [devkitA64](https://devkitpro.org/) / libnx (for building)

## Installation

1. Download [`releases/MenuMusicNX-1.0.1.zip`](releases/MenuMusicNX-1.0.1.zip), or build your own (see [Building](#building)).
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

## Usage

| Situation | Behaviour |
|---|---|
| HOME Menu (no game) | Music plays |
| Game running | Music pauses |
| HOME pressed over a game | Music resumes (same position) |
| Return to game | Music pauses |
| Close game on HOME | Music continues |
| Console power on | Music fades in (~1 s) |
| Console power off | Stops immediately (no fade out) |

Open the overlay with **L + D-Pad Down + Right Stick click** (default Tesla binding), then select **MenuMusicNX**.

## Building

```bash
git clone --recursive https://github.com/Fimochi/MenuMusicNX.git
cd MenuMusicNX
make
make dist   # optional: produces dist/ zip ready for SD card
```

Requires the `overlay/lib` submodule ([libtesla](https://github.com/WerWolv/libtesla)).

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
