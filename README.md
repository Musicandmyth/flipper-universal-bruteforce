# Universal Bruteforce (Flipper Zero)

Plays **every signal file in a folder**, one after another — a multi-format
"dictionary" bruteforcer. Point it at a folder of captured/known signals and it
replays or emulates them all in sequence, across Sub-GHz, Infrared, RFID,
iButton and NFC.

Built for **Momentum firmware** (SDK `mntm-012`) using `ufbt`.

## Supported formats

| Ext | Type | Action |
|------|------|--------|
| `.sub` | Sub-GHz | Transmits the signal (protocol-encoded or RAW, incl. custom CC1101 presets). |
| `.ir` | Infrared | Fires **every** signal contained in the file (parsed + raw). |
| `.rfid` | LF RFID (125 kHz) | Emulates the card for a dwell time. |
| `.ibtn` | iButton / 1-Wire | Emulates the key for a dwell time. |
| `.nfc` | NFC | Emulates the tag for a dwell time. |

A single folder can mix all of these — the app detects each file's type by its
extension and dispatches to the right subsystem.

## Features

- Pick any folder on the SD card (browse to it, press **Right** to select the folder).
- Sequentially plays all supported signal files found in that folder.
- **One-shot** formats (Sub-GHz, IR) transmit `repeats` times each; **emulation**
  formats (RFID, iButton, NFC) hold each for the delay time (minimum 1.5 s).
- Configurable **delay between files**, **repeats per file**, and **loop forever**.
- **Loading screen** that scans the folder first and shows how many signals were
  found before playback.
- Live progress screen: file X/Y, progress bar, current filename, signal type +
  info, OK/error counts.
- Controls: **Left/Right** = skip to previous/next signal (hold to fast-skip),
  **OK** = pause/resume, **Back** = stop and return.

## Files

| File | Purpose |
|------|---------|
| `application.fam` | App manifest (id, entry point, category, icon). |
| `subghz_bruteforce.c` | UI (menu / settings / run screen), folder picker, worker thread. |
| `signal_tx.c` / `signal_tx.h` | Multi-format dispatch: type detection + IR/RFID/iButton/NFC backends. |
| `subghz_tx.c` / `subghz_tx.h` | Sub-GHz transmit engine — parses a `.sub` file and keys the radio. |
| `icon.png` | 10×10 app icon. |

## Build online (recommended — no toolchain needed)

You can compile this straight from the GitHub repo in your browser; nothing to
install locally.

1. Make sure this repo is **public** and your latest changes are **pushed**.
2. Go to **https://flipc.org** (or the mirror **https://fzoc.kanjian.fr**).
3. Paste your repository URL, e.g. `https://github.com/<you>/subghz-bruteforce`.
4. Pick the firmware target: choose **Momentum** if it's listed. If it isn't,
   pick **Unleashed** — Momentum is a fork of Unleashed and runs Unleashed apps.
5. Build, then **download `subghz_bruteforce.fap`**.
6. Copy it to `SD/apps/Tools/` on the Flipper (or drag it in via qFlipper).

## Build with GitHub Actions

This repo also includes `.github/workflows/build.yml`, which builds the FAP on
every push and uploads it as a run artifact (**Actions** tab → latest run →
**Artifacts → subghz_bruteforce-fap**). Pushing a `v*` tag additionally publishes
a **Release** with the `.fap` attached.

## Build locally

The source was written against the real Momentum `mntm-012` SDK headers. On a
machine with internet access to `update.flipperzero.one` (needed for the ARM
toolchain):

```powershell
# One-time: install the build tool and point it at Momentum's SDK
python -m pip install --user ufbt
python -m ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel=release

# From this project folder (the one with application.fam):
python -m ufbt            # builds dist\subghz_bruteforce.fap

# With a Flipper connected over USB, build + install + launch:
python -m ufbt launch
```

The resulting `subghz_bruteforce.fap` goes in `SD Card/apps/Tools/` on the
Flipper (it appears under **Apps → Tools → Universal Bruteforce**).

## Usage

1. Put your signal files in a folder on the SD card. They can be any mix of
   `.sub`, `.ir`, `.rfid`, `.ibtn` and `.nfc`.
2. Open **Apps → Tools → Universal Bruteforce**.
3. **Select folder** → browse to the folder → press **Right** to choose it.
4. (Optional) **Settings** → set delay / repeats / loop. The delay also sets the
   emulation dwell time for RFID/iButton/NFC (clamped to a 1.5 s minimum).
5. **Start**. A loading screen scans the folder and shows the signal count, then
   playback begins. **Left/Right** skip between signals, **OK** pauses,
   **Back** stops.

## Legal / safety

Only transmit or emulate on devices you are **legally authorized** to operate.
Replaying access-control signals (gates, garages, cars, badges, fobs, etc.) that
you do not own or have permission to test may be illegal in your jurisdiction.
Fixed-code signals are also replayable in ways rolling-code systems are not —
know what you are transmitting.
