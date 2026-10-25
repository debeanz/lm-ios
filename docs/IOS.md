# iPhone / iPad (iOS)

An unsigned IPA of Luigi's Mansion Recompiled for iPhone and iPad (arm64, iOS 16+), made from your
own copy of the game. Same game code, renderer and options as the PC version, plus a phone layer
(`src/mobile`): an on-screen GameCube controller, the iPhone settings, haptics and a detailed
session log.

## How it differs from the PC version

iOS does not allow compiling or loading code at run time, so the PC version's first-run setup
(recompile `main.dol`, build a game module with the bundled clang) cannot run on a phone. The iOS
app is the `lmrecomp` kind of build instead: the game is recompiled **at build time** from your
`main.dol` and linked into the app (`LM_STATIC_MODULE`). One IPA plays one release, USA (GLME01) or
Europe (GLMP01), chosen when it is built.

The app holds no game data. On the first start it asks for your disc image of the same release,
which is copied into the app (or copy it there yourself with the Files app). The European Hidden
Mansion overlay needs both discs and the PC setup; it is not part of the iOS build yet.

The IPA contains code generated from the game, and `main.dol` is the game's own program: both
stay in a **private** repository and are for your own devices.

## Building the IPA (GitHub Actions, no Mac needed)

Two repositories, so the build runs on GitHub's free Mac minutes for public repositories while
nothing from the game is ever public:

- **build repository** (may be public): this source only. It runs `.github/workflows/ios.yml`. The
  workflow publishes nothing in it (no artifacts, no releases), and its public log never prints
  generated game code (compiler messages about it are only counted).
- **data repository** (private, e.g. `<you>/lm-ios-data`): your `main.dol` (release `game-data`)
  goes in, the IPA and the full build log (release `ios-<run number>`) come out. It runs no Actions.

Setup once:

1. Your `main.dol`:
   - from an ISO/GCM: `py tools/ios/extract_dol.py "<your disc>.iso" gamedata`;
   - from RVZ/WIA: Dolphin > right-click the game > Properties > Filesystem > Extract System Data
     (`sys/main.dol`).
2. Upload it to the data repository (skipped when unchanged):
   `powershell -ExecutionPolicy Bypass -File tools/ios/publish-dol.ps1 -Dol gamedata/main.dol -Repo <you>/lm-ios-data`
3. A fine-grained personal access token (GitHub > Settings > Developer settings > Fine-grained
   tokens): repository access = only the data repository, permission Contents = Read and write.
   Store it in the build repository as the secret `GAME_DATA_TOKEN`
   (`gh secret set GAME_DATA_TOKEN --repo <you>/<build repo>`, then paste it), and set
   `gh variable set GAME_DATA_REPO --repo <you>/<build repo> --body <you>/lm-ios-data`.
   Secrets are never shown in logs, and the workflow only runs when you start it.

Build: `powershell -ExecutionPolicy Bypass -File tools/ios/build-ipa.ps1 -Repo <you>/<build repo> -DataRepo <you>/lm-ios-data`
(`-Version GLMP01` for a European `main.dol`). It pushes the current branch, runs the workflow on a
GitHub-hosted Mac (macos-15), waits and downloads the release into `build/ios-ipa/ios-<n>`. On the
phone, the IPA is in the data repository's releases. A failed run prints the public log tail and
downloads the full build log.

What the workflow does: builds the recompiler for the Mac (`tools/recomp/host`), configures the iOS
cross build (`-DCMAKE_SYSTEM_NAME=iOS`, Ninja, `-DLM_HOST_RECOMPILE=...`; Dawn's prebuilt ios-arm64
package, SDL3 and nod built from source, nod with Rust's `aarch64-apple-ios` target), builds the
`LuigisMansioniOS` target (`packaging/ios/ios_bundle.cmake`, `Info.plist.in`) and zips `Payload/`.
Third-party builds are cached between runs.

Locally on a Mac the same commands as in the workflow work.

## Installing

SideStore, AltStore, Sideloadly or LiveContainer sign the IPA with your Apple ID. For the larger
memory limit (helpful on 6 GB iPhones and at high resolutions) sign with
`packaging/ios/LuigisMansion.entitlements` where the tool allows it (Sideloadly advanced options,
a paid developer account); without it the game still runs. The app declares iOS Game Mode support.

## First start

The disc page lists the disc images in the app's folder and why one does not fit (another release,
not a GameCube image). "Choose disc image" opens the Files picker; the chosen file is copied into
the app. "Check again" looks for a file copied in with the Files app.

## Touch controls (GameCube layout)

- Left: Control Stick in an octagonal gate (fixed, or floating where the thumb lands: iPhone tab >
  Control Stick), D-pad above it.
- Right: big green A, small red B to its lower left, kidney-shaped X (right) and Y (top) around A,
  yellow C-Stick to the lower left.
- Top: analog L and R in the corners, purple Z under R, START and the menu button in the middle.
  The menu button opens the F1 menu on its iPhone tab (and closes it).

Behaviour, the same as in every phone port: smoked-glass controls drawn in code (no image files),
press animation and glow, light haptic tap per press, constant opacity, hidden while a controller is
connected (and shown again when it disconnects). A finger can slide from one face button to another.
A tap is always seen by the game, even if the finger lifts before the game reads the pad. The D-pad
sends one press per new direction and, after 0.4 s held, repeats every 0.15 s, so menus step once
per tap. The game's rumble plays on the Taptic Engine while the touch controller is used (Vibration).

Fingers that miss the controls go to the menus as a mouse: tap to click, drag up/down to scroll.

## iPhone settings (F1 > iPhone)

The same rows and values as every phone port; changes apply and are saved at once:

| Row | Values |
|---|---|
| Resolution | Native (panel size) / 2106x1084 / 1920x1080 / 1600x900 / 1280x720 (render height, never above the panel) |
| Aspect Ratio | iPhone Default / 1.943:1 / 16:9 / 4:3 |
| Screen Scaling | Fit (bars around the image, Aurora patch 0239) / Stretch |
| Frame Rate | 30 / 60 / 120 FPS (ProMotion); above 30 the frames in between are interpolated, the game logic stays at 30 |
| Show FPS | Off / On |
| UI Size | Small / Default / Large (menus at about 1.5x the PC size by default) |
| Touch Controls | Off / On |
| Touch Opacity | 20-100 % |
| Touch Size | 70-150 % |
| Control Stick | Fixed / Floating |
| Vibration | Off / On |

First start: 60 fps, and a render height by memory as a quality tier (8 GB: 1080, 6 GB: 900, less:
720). Desktop-only rows (window, VSync, menu size, PC frame-rate slider) are hidden on the phone;
everything else in the F1 menu works as on the PC. There is no Quit button (iOS apps do not quit).

## Logs

Every session writes `Logs/session_<date>_<time>.log` in the app's folder (Files app > On My iPhone >
Luigi's Mansion > Logs; the last 10 are kept). Everything the game prints goes there with the time
and the seconds since start on each line, written line by line, so nothing is lost when the app
crashes or iOS ends it in the background. It contains:

- a header: app and program version, device model, iOS version, RAM, GPU (Metal family, working
  set), screen size and refresh rate, safe area, memory, free storage, how the previous run ended,
  the phone settings;
- a `[perf]` line every 10 s: average and game fps against the target, dropped frames, worst frame,
  memory footprint and what is left before iOS's limit, thermal state, Low Power Mode, input in use;
- app lifecycle (background/foreground, memory warnings, thermal and power changes), controllers
  added/removed, every touch control press, every settings change, the disc search;
- the game's own log (`[boot]`, `[ws]`, `[precache]`, `[hang]` reports with every thread's stack);
- crash reports: the signal, the faulting address and the stack, written straight into the file.

The same lines go to the system log (Console.app, subsystem `com.lmrecomp.ios`).

## Where things are

| What | Where |
|---|---|
| settings.ini, install.ini, last_run_state.ini, memory card saves, logs, disc images | app's Documents (Files app) |
| shader and pipeline caches | Library/Caches/Luigi's Mansion Recompiled (iOS may clear it; it is rebuilt) |
| pipelines.bin, icons | inside the app |

## Testing the phone layer on a PC

`LM_MOBILE=1` turns the phone layer on in `lmrecomp` / `LuigisMansionRecomp`: the touch controller
(the left mouse button is one finger), the iPhone tab and the phone display model.
`LM_MOBILE_IGNORE_PADS=1` keeps the touch controller with a controller attached, `LM_SESSION_LOG=1`
writes the session log on a PC too.

## Status

Builds on GitHub Actions (first successful run 5, 2026-10-25: Xcode 26.3, a 14 MB IPA with a 50 MB
arm64 executable). Fixes the first runs needed: Objective-C++ enabled on Apple, libc++'s
experimental switch for `std::jthread`, a const path iterator in the updater, and library lookups
restricted to the iPhone SDK (the runner's Homebrew macOS libpng/zstd were picked up otherwise).

Not yet run on a device: start-up, Metal rendering with Aurora's local patches, touch input and
performance are unverified; the session log is the first place to look. Not yet: the European
Hidden Mansion overlay on iOS, both releases in one IPA.
