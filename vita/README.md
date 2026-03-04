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

Install the [Vita SDK](https://vitasdk.org/):

```bash
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH

git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh
./install-all.sh
```

## Building

```bash
cd vita
mkdir build && cd build
cmake ..
make
```

This produces `snapcast_vita.vpk` which can be installed on a hacked PS Vita.

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

Currently PCM is fully supported. The codec header parsing also recognizes FLAC and Opus
headers, but decoding for those codecs requires linking additional libraries (libFLAC, libopus)
which can be added as future enhancements.

## Network Protocol

- **Binary streaming** (port 1704): Hello -> ServerSettings -> CodecHeader -> WireChunks
- **JSON-RPC control** (port 1705): Server.GetStatus, Client.SetVolume, Stream.Control, etc.

## License

GPL-3.0 (same as Snapcast)
