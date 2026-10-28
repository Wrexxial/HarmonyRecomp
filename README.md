# HarmonyRecomp

A static recompilation of **Castlevania: Harmony of Despair** (Xbox Live Arcade, title ID `58410A7A`) for Windows, built on the [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk) and modelled on [NocturneRecomp](https://github.com/birabittoh/NocturneRecomp).

The PowerPC code of `default.xex` and the character modules is translated to C++ at build time and compiled natively for x86-64. Graphics, audio and the Xbox system services come from the ReXGlue runtime (derived from [Xenia](https://github.com/xenia-project/xenia)).

> **This repository contains no game code or data.** You need your own copy of the game, dumped from your own console or purchase. Never share `game/`, `assets/`, `dlc/` or `generated/`: they contain the game or code derived from it.

## Screenshots

| Title screen | Online lobby |
| --- | --- |
| ![Title screen](1-titel.jpg) | ![Online lobby](2-lobby.jpg) |

| In-game with graphics menu | Gameplay with FPS counter |
| --- | --- |
| ![In-game with graphics menu](3-settings.jpg) | ![Gameplay with FPS counter](4-gameplay.jpg) |

## Features

- Runs the full game natively on Windows: all stages, all six base characters
- Graphics menu (F6): resolution scale, anti-aliasing, anisotropic filtering, VSync, fullscreen
- FPS counter (F2)
- **Online co-op without Xbox Live**: up to six players over LAN or a virtual LAN (Radmin VPN, ZeroTier, Tailscale)
- **DLC support**: put your own DLC packages into `dlc/` (see below)
- Crash reports in `logs/` to make bug reports useful

## Installation (easy)

1. Download **`HarmonyRecomp-Setup.exe`** from the [latest release](../../releases/latest).
2. Run it. Windows may warn that the program is not signed ("Windows protected your PC"): click **More info → Run anyway**.
3. Select **your game file**: the file without extension from `58410A7A\000D0000\` of your console dump (it starts with `LIVE`). Optionally select a folder with your DLC packages.
4. Click **Install** and wait. The setup downloads the build tools (about 1 GB), and if Visual Studio is not installed, Microsoft's C++ components (about 400 MB, you are asked to accept Microsoft's license). Then your PC translates and compiles the game: **10 to 30 minutes** once, depending on your CPU. About 10 GB of free disk space are needed while it runs, about 1 GB afterwards.
5. Click **Play**, or use the desktop shortcut.

Nothing of the game is downloaded: everything is built from your own copy on your own PC. To update, run the setup of a newer release; your settings and saves are kept. If something fails, the setup shows the log (`setup.log` in the install folder); please attach it to an issue.

The rest of this page is for building by hand and for developers.

## Requirements

- Windows 10/11, x86-64
- Your own XBLA package of the game (the file without extension in `58410A7A/000D0000/`, it starts with `LIVE`)
- Build tools, e.g. with [Scoop](https://scoop.sh) in PowerShell:

  ```powershell
  scoop install llvm cmake ninja git python
  ```

  The SDK needs **Clang 20 or newer** (`clang++ --version`).

## Building

1. Clone this repository.
2. Copy your game package into `game\` (exactly one file).
3. Optional: copy your DLC packages into `dlc\` (see [DLC](#dlc)).
4. Build:

   ```powershell
   python scripts\build.py
   ```

`build.py` runs every step in order and skips what is already done:

1. Extract the game package into `assets\` (and DLC packages into `assets\dlc\`)
2. Clone the ReXGlue SDK at the commit in `sdk-pin.txt` into `..\rexglue-sdk\`, **next to** this folder, and apply the local SDK patches (`SDK_PATCHES` in the script)
3. Build and install the SDK, including the `rexglue` codegen tool (takes the longest)
4. Run codegen (about 2 minutes), output in `generated\`
5. Build the game into `out\build\win-amd64-release\`

Options: `--debug`, `--codegen-only`, `--force-codegen`. Codegen reruns automatically when the manifest, a `harmonyrecomp_*.toml`, the DLC set or the SDK patches change.

## Running

```powershell
cd out\build\win-amd64-release
.\harmonyrecomp.exe
```

The executable finds the game data by itself: `--game_data_root`, then `game_data_root` in `harmonyrecomp.toml`, otherwise `assets` next to the exe or in the project folder.

Settings live in `harmonyrecomp.toml` next to the exe (copied there on the first build, your edits are kept). `license_mask = 1` reports the full game; without it the game runs as the trial version. The license of your purchase stays on your Xbox account and cannot be read by the recompiled game.

| Key | Function |
| --- | --- |
| F2 | FPS counter |
| F3 | SDK debug overlay (frame times) |
| F4 | All SDK runtime settings |
| F6 | Graphics menu ("Save" writes `harmonyrecomp.toml`) |
| `^` | Log console |

Logs are written to `logs\` next to the exe. If the game crashes, a `crash_*.txt` appears there as well; please attach it (and the matching log) to bug reports. The build also writes `harmonyrecomp.map`, which turns the addresses in a crash report into function names.

## Online co-op

On the Xbox 360 the game's co-op (up to six players) only works over Xbox Live. HarmonyRecomp replaces the required services in `src/netplay/`:

- **Sign-in:** the game sees a player signed in to Live with their own gamertag and online XUID. The offline profile, and with it your save data, stays unchanged.
- **Sessions and matchmaking:** create, search, join, details, host migration. Instead of a server, the search asks all PCs in the network (broadcast) plus the addresses in `netplay_peers`. Every player runs a small lobby service on UDP port `47658`.
- **XNet and QoS:** players are identified by their IPv4 address. The game's own traffic is plain UDP on port `1000` directly between the PCs.

### Playing with friends over the internet

1. **Set up a virtual LAN.** Everybody joins the same network:
   - **Radmin VPN** or **ZeroTier**: lobbies are found automatically.
   - **Tailscale**: does not forward broadcasts; put each other's Tailscale IP into `netplay_peers`.
2. **Everybody builds the project** with their own copy of the game. All players should use the same version of this repository.
3. **Settings** in `harmonyrecomp.toml` next to the exe:

   ```toml
   netplay_enabled = true
   netplay_name = "YourName"     # different for every player, max. 15 characters
   # netplay_peers = "100.x.y.z" # only needed if the search finds nothing
   ```

4. **Firewall:** on the first start Windows asks for network access. Allow `harmonyrecomp.exe` on **private and public** networks; VPN adapters are often classified as public. The ports are UDP `47658` (lobby) and UDP `1000` (game).
5. One player creates a room in the online menu, the others search and join.

The log shows every step with `[netplay]`: the chosen address, lobbies found, QoS results, opened ports.

| Setting | Meaning |
| --- | --- |
| `netplay_enabled` | Online co-op on/off (default: off) |
| `netplay_name` | Gamertag shown to other players |
| `netplay_ip` | Force your own address, e.g. the VPN IP (default: automatic, VPN adapters preferred) |
| `netplay_peers` | Additional players for the search: `ip` or `ip:port`, comma separated |
| `netplay_port` | UDP port of your lobby service (default 47658) |
| `netplay_remote_port` | Lobby port of the others (only for two instances on one PC) |
| `netplay_search_ms` | How long the lobby search waits (default 1000 ms) |

## DLC

The game's downloadable characters and chapters are content packages (the files in `58410A7A/00000002/` on your console). Use your own:

1. Copy the package files into `dlc\` in the project folder.
2. Run `python scripts\build.py`. It unpacks them into `assets\dlc\`, finds the character modules (`dll*.dll`) in them and recompiles them like the base characters.
3. Start the game. On the first start the packages are installed into the runtime's content folder (this takes a moment); the log shows `[dlc] installed …`.

DLC support has been tested with the build and installation pipeline, but not yet with real DLC packages. If a DLC character crashes with "unregistered function", the module needs a small config file like `harmonyrecomp_dllsoma.toml`; please open an issue with the log and crash report.

## Project layout

| Path | Content |
| --- | --- |
| `harmonyrecomp_manifest.toml` | Codegen entry: `default.xex` and the six character modules |
| `harmonyrecomp_config.toml` | Game-specific codegen settings (functions, CRT addresses, jump tables) |
| `harmonyrecomp_dll*.toml` | Codegen settings per character module |
| `harmonyrecomp.toml` | Template for the runtime settings |
| `src/` | Host code: app, graphics menu, crash reports, runtime fixes, DLC installation |
| `src/netplay/` | Online co-op: Live replacement, sessions, lobby service, game sockets |
| `scripts/build.py` | Complete build, including the SDK patches |
| `scripts/extract_game.py` | STFS package extractor (game and DLC) |
| `sdk-pin.txt` | SDK commit the project is tested with |

## Technical notes

- `default.xex`: original name `dcHD_Master.exe`, version `0x00000104`, base `0x82000000`, size `0x960000`, entry `0x8263BA90`, XDK 2.0.9328.0.
- Character modules are XEX DLLs in `data\player_dll\` with fixed load addresses (Alucard `0x83000000`, Soma `0x83090000`, Jonathan `0x83120000`, Charlotte `0x831B0000`, Shanoa `0x83240000`, Axe Armor `0x83510000`).
- Every recompiled module has an indirect-call table in guest memory, normally right behind its image. The main table (~13 MB) would cover the character load addresses, so it lives at `0x8C000000`; character modules get 1 MB slots from `0x8D000000` (DLC characters follow the base ones). This needs the relocatable-table patches in `build.py`.
- `sub_8277ECF0` uses a byte jump table without bounds check; its 16 real entries are listed in `harmonyrecomp_config.toml`.
- SDK patches in `build.py` (applied automatically): switch tables in the function scanner, relocatable function tables, readable message box text, settings file escaping, and stale handles cached in guest dispatcher headers (a crash source in online sessions).
- The netplay layer keeps the game's sockets in its own handle table and implements overlapped `WSARecvFrom`/`WSASendTo`/`WSAGetOverlappedResult`, which the SDK only stubs.

## Known issues

- Closing the game sometimes ends with a crash report (heap corruption during runtime shutdown). The game has saved by then; it does not affect playing.
- DLC support is untested with real packages (see above).

## Credits

- [ReXGlue SDK](https://github.com/birabittoh/rexglue-sdk) and [Xenia](https://github.com/xenia-project/xenia): recompiler and runtime
- [NocturneRecomp](https://github.com/birabittoh/NocturneRecomp): project structure and the original STFS extractor
- Xbox 360 structure layouts as documented by the Xenia project

## License

The code in this repository is licensed under the BSD 3-Clause License, see [LICENSE](LICENSE). `scripts/extract_game.py` is based on MIT-licensed code, see `scripts/LICENSE-NocturneRecomp.txt`.

Castlevania and Harmony of Despair are trademarks of Konami Digital Entertainment. This project is not affiliated with or endorsed by Konami or Microsoft.
