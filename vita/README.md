# Snapcast Vita - PS Vita Snapcast Client

A full-featured Snapcast client for the PlayStation Vita with a native GUI.

## Features

- **Audio streaming** via Snapcast binary protocol (port 1704)
- **JSON-RPC control** via TCP (port 1705)
- **5-screen tabbed GUI**:
  - **Connect** - Server IP/port configuration
  - **Player** - Cover art, play/pause/skip, volume, track info
  - **Devices** - Overview of all clients with volume/mute control
  - **Groups** - Group management, stream assignment
  - **Settings** - Client name, latency adjustment, audio info
- **On-screen keyboard** for text input (native Vita IME)
- **Config persistence** saved to `ux0:data/snapcast/`
- **PCM audio** playback at 48kHz/16bit/stereo via SceAudioOut

## Controls

| Button | Action |
|--------|--------|
| L/R Triggers | Switch tabs |
| D-pad Up/Down | Navigate lists |
| D-pad Left/Right | Adjust volume / latency |
| Cross (X) | Select / Play-Pause |
| Square | Stop / Change stream |
| Triangle | Toggle mute |
| Start | Connect / Disconnect |
| Select + Start | Exit application |

## Prerequisites

You need the [Vita SDK](https://vitasdk.org/) installed. If CMake reports that it could not find the toolchain file, the SDK is either not installed or `VITASDK` points to the wrong path.

**Install the SDK** (e.g. into `/usr/local/vitasdk`):

```bash
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH

git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh
./install-all.sh
```

**If you installed the SDK elsewhere**, set `VITASDK` to that path before building, e.g.:

```bash
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH
```

## Building

From the `vita` directory, configure with an explicit build directory so the Makefile is created in `build/`:

```bash
cd vita
cmake -S . -B build
cd build && make
```

Alternatively, if you already ran `cmake ..` from `build/`, the Vita SDK scripts place the Makefile in the parent directory—run **`make` from `vita/`** (one level up from `build/`):

```bash
cd vita
make
```

This produces `snapcast_vita.vpk` in the build tree, which can be installed on a hacked PS Vita.

### Release build and storage writes

From the repository root:

```bash
cmake -S vita -B vita/build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG' \
  -DSNAPVITA_FILE_LOGGING=OFF -DSNAPVITA_AUDIO_DUMP=OFF
cmake --build vita/build/release -j2
```

Both diagnostic options default to `OFF`. This build does not create, append,
truncate or delete debug logs or PCM dumps. Existing diagnostic files are left
untouched. Continuous playback performs no application file writes. Errors
and audio counters remain visible in the UI.

`config.json` is written only for the first save or when persisted settings
actually change, including a new network/server profile. Unchanged reconnects
and app exits skip all save-related filesystem calls. Short writes are completed,
and a failed save is not treated as successfully persisted.

For troubleshooting, use a separate build directory and explicitly enable
`-DSNAPVITA_FILE_LOGGING=ON`. Audio dumping separately requires
`-DSNAPVITA_AUDIO_DUMP=ON`. Neither option adds file I/O to the playback thread.

### Sync regression test

With `VITASDK` set and a host C compiler installed, run from the repository root:

```bash
sh vita/tests/run.sh
```

The tests exercise the real wire-chunk handler, ring buffer and playback-grain
renderer with a simulated clock and Vita OS calls. They cover:

- Fresh connections, the original -700 ms YouTube gap, small buffers and repeated underruns
- Five-second buffering, live delay changes and server-provided latency
- Timestamp scheduling and trimming of late audio, including delayed packet bursts
- Drift correction with 20 ms and 100 ms chunks
- Sustained output pacing drift and periodic stalls without unnecessary restarts
- Playback-thread output cadence with simulated slow storage and no filesystem calls
- Oversized and unaligned writes, full buffers and unsigned counter wraparound
- Concurrent producer/consumer ordering across one million frames
- RTC adjustments and elapsed-time values beyond 32-bit microseconds
- Audio/stream thread creation and start failures, output failure, cleanup and retry
- PCM/Opus header validation and failure propagation through the stream worker
- Release startup/playback/shutdown without diagnostic filesystem calls
- Unchanged configuration saves without filesystem calls, plus changed settings and failed-save retries

The host build enables undefined-behavior traps. Speaker timing and actual
Vita output latency still require an on-device check.

### Playback synchronization

Playback starts at each stream's timestamp plus the effective buffer delay.
Underruns and timestamp discontinuities over 50 ms restart this schedule.
Ordinary timing errors allow soft correction up to half the effective buffer
delay, capped at 500 ms, with an allowance of at least two output grains at low
sample rates. Late samples are discarded; early audio waits at normal pitch.
Clock drift and output scheduling overhead use a two-second time window and
proportional speed correction, capped at approximately 6%. Source gaps clear
the correction history so an old slowdown does not persist into the next song.

The ring uses 2 MiB, allowing up to five seconds of audio at 48 kHz stereo plus
one maximum-size incoming chunk. Delay settings are bounded to supported limits
and the Settings screen shows both the server buffer and effective playback
delay. Server latency settings are applied on connection and delay changes
reschedule playback. In diagnostic builds, messages beginning with `[sync]`
identify restarts, underruns and delay changes.

Playback and protocol timestamps use elapsed process time, so changing the
system clock does not move playback deadlines. Codec headers are validated
before changing the active format. Thread startup and audio output failures
report a connection error and enter the existing reconnect path; terminated
audio threads are joined and released before another attempt.

The audio playback loop performs no filesystem I/O. Periodic file logging here
can interrupt hardware output even when the PCM ring is full, so diagnostics
use on-screen audio counters and, when explicitly enabled, stream-thread sync logs.

## Installing

The VPK includes the Snapcast launcher icon at `sce_sys/icon0.png`. It is a
128×128 indexed PNG exported from `doc/snapcast_icon.svg`. To regenerate it
with ImageMagick from the repository root:

```bash
magick -background '#ffc107' doc/snapcast_icon.svg -resize 128x128 -alpha off -strip -colors 128 PNG8:vita/sce_sys/icon0.png
```

1. Copy `snapcast_vita.vpk` to your Vita via USB or FTP
2. Install using VitaShell
3. Launch "Snapcast" from the LiveArea

## Architecture

```
vita/
├── CMakeLists.txt          # Vita SDK build system
├── src/
│   ├── main.c              # Entry point, main loop, streaming thread
│   ├── types.h             # Shared data structures
│   ├── json.h / json.c     # Minimal JSON parser/builder
│   ├── config.h / config.c # Configuration persistence
│   ├── network.h / network.c # TCP + binary protocol + JSON-RPC
│   ├── audio.h / audio.c   # SceAudioOut ring buffer playback
│   └── gui.h / gui.c       # vita2d tabbed GUI (960x544)
└── sce_sys/
    └── livearea/contents/
        └── template.xml     # LiveArea definition
```

## Supported Codecs

- **PCM** – fully supported (48 kHz / 16-bit / stereo)
- **Opus** – fully supported via libopus; configure the server with `codec = opus` in
  `snapserver.conf` for ~10–20× bandwidth reduction vs PCM
- **FLAC** – codec header is recognised but decoding is not yet implemented (requires libFLAC)

## Network Protocol

- **Binary streaming** (port 1704): Hello -> ServerSettings -> CodecHeader -> WireChunks
- **JSON-RPC control** (port 1705): Server.GetStatus, Client.SetVolume, Stream.Control, etc.

## License

GPL-3.0 (same as Snapcast)
