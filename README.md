# Relcast

A lightweight command-line **Icecast/Shoutcast player and transcoder**, built on top of FFmpeg's `libav*` libraries. It behaves similarly to the now-abandoned `sc_trans` program.
The code is in a very alpha stage, it may lack features, and may behave erraticaly sometimes,  but I am successfully using it to stream music to a Shoutcast server.
Playlists are simple text files with one full path of a song file per row, just the way sc_trans was using.

## Table of Contents

- [Features](#features)
- [Notes](#notes)
- [Warnings](#warnings)
- [Limitations](#limitations)
- [Requirements](#requirements)
- [Building](#building)
- [Quick Start](#quick-start)
- [Command-Line Reference](#command-line-reference)
- [Config File Format](#config-file-format)
- [Runtime Control Signals](#runtime-control-signals)

## Features

- Transcodes content from a playlist file source or a live source (broadcaster application) to Icecast/Shoutcast servers.
- On-the-fly transcoding between **MP3**, **Ogg Vorbis**, **Opus**, **AAC (ADTS)**, and **FLAC**.
- Per-stream sample rate and channel count override (resample/downmix via `libswresample`).
- **Playlist mode**: loops over a local list with smooth crossfades between tracks (natural end-of-track fades, forced skip, and hot playlist-file reload — all controllable at runtime via signals).
- **Live DJ takeover** (`live_enable = 1`): a SHOUTcast v1-style TCP listener that lets an authenticated DJ client take over the output live, automatically crossfading in and out of the playlist, with priority-based "kick" semantics between multiple credentials. IPv6 is supported.

## Notes

- Tracks must meet a minimum duration to be eligible for crossfading with the previous or next track. Tracks shorter than **10 seconds** will not be crossfaded.

## Warnings

- The code is in a very alpha stage! Use it at your own risk!
- The program is theoretically capable of supporting multiple stream definitions (see [Config File Format](#config-file-format) below); however, this functionality has not yet been tested.

## Limitations

- Linux-only. I have, yet, no intention to port it.
- The metadata in the media files is not used. Instead, the program uses the filename without the extension as metadata.
- Shoutcast sources and servers support only MP3 and AAC as output formats. Consequently, listeners will only be able to decode MP3 and AAC streams.

## Requirements

- A C11 compiler (`gcc` or `clang`)
- `pkg-config`
- FFmpeg development libraries:
  - `libavformat`
  - `libavcodec`
  - `libavutil`
  - `libswresample`

  Tested against FFmpeg 7.x (`libavformat` 63.x, `libavcodec` 63.x, `libavutil` 61.x, `libswresample` 7.x). Older FFmpeg builds may require minor API adjustments, as some `avcodec_get_supported_config`-style calls require a reasonably recent FFmpeg version.
- POSIX threads (`pthread`) and `libm`
- Linux (developed and tested on Linux; relies on POSIX signal handling in `main.c`)

On Debian/Ubuntu-based systems:

```sh
sudo apt install build-essential pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
```

## Building

### Using CMake

```sh
mkdir -p build && cd build
cmake ..
cmake --build .
# resulting binary: ../cmake-build-Release/output/relcast
```

## Quick start

All stream definitions live exclusively in the config file; there is no way to define a stream via the command line. Write a config file (for example streams.ini) describing one or more streams (see [Config file format](#config-file-format-multi-stream) below), then run:

```sh
./relcast -c /path/to/streams.ini
```

To increase log verbosity (repeatable) or silence everything but errors:

```sh
./relcast -c /etc/relcast/streams.ini -v -v     # Debug-level logs
./relcast -c /etc/relcast/streams.ini -q        # Errors only
```
Playlists are simple text files with one full path of a song file per row, just the way sc_trans was using.
The next example script lists all the mp3 files in a folder

```sh
#!/bin/sh
indirectory=$1
find "$indirectory"   -name "*mp3" -type f
```

## Command-Line Reference

```
relcast 0.1.0 - Shoutcast/Icecast stream transcoder

Usage:
  relcast -c <config.ini> [-v] [-q]

Options:
  -c, --config <file>        Load stream definitions from ini-like file (required)
  -v, --verbose              Increase log verbosity (can repeat)
  -q, --quiet                Only log errors
      --help                 Show this help
```

**Notes:**

- `-c`/`--config <file>` is mandatory; `relcast` refuses to start without it (there is no CLI-only single-stream mode).
- `-v` may be repeated to step through `error → warn → info → debug` verbosity levels; `-q` jumps straight to error-only.

## Config File Format

A simple INI-like format is supported via `-c`/`--config`:

```ini
[stream]
name = MyRadio
playlist_file = /etc/relcast/playlist.pls
crossfade_ms = 3000
output_host = shoutcast.example.com
output_port = 8000
output_mount = /radio3.mp3
output_user = source
output_password = hackme
codec = mp3
bitrate = 128000
live_enable = 1
live_ipv4_listen_addr = 0.0.0.0
live_ipv4_listen_port = 8001
live_ipv6_listen_addr = ::
live_ipv6_listen_port = 8011
live_credential = alice:secret1:10
live_credential = bob:secret2:5
```

## Runtime Control Signals

In addition to standard `SIGINT`/`SIGTERM` graceful-shutdown handling, `relcast` responds to several additional POSIX signals sent to the running process (`main.c`). These are useful for operating a long-running instance without requiring a restart.

| Signal | Effect |
|--------|--------|
| `SIGINT`, `SIGTERM` | Stop all streams cleanly and exit. |
| `SIGHUP` | Reload the config file (`-c`) live: add newly-defined streams, stop removed ones, and restart any stream whose settings changed. Running streams whose configuration is unchanged continue undisturbed. |
| `SIGWINCH` | Skip immediately to the next playlist entry (with crossfade) on every currently running playlist stream. |
| `SIGUSR1` | Reload each playlist stream's `playlist_file` from disk and restart iteration from the top, without interrupting the currently playing track. |
| `SIGPIPE` | Ignored (`SIG_IGN`), so a broken output socket surfaces as a normal write error instead of terminating the process. |

**Example:**

```sh
./relcast -c /etc/relcast/streams.ini &
echo $! > /tmp/relcast.pid

# ... edit /etc/relcast/streams.ini ...
kill -HUP "$(cat /tmp/relcast.pid)"      # Live config reload

kill -WINCH "$(cat /tmp/relcast.pid)"    # Skip current playlist track
kill -USR1 "$(cat /tmp/relcast.pid)"     # Reload playlist file(s) from disk
```