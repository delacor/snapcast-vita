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

## Installing

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
