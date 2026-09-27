# Relcast
A lightweight command-line **Icecast/Shoutcast player and transcoder**, built on
top of FFmpeg's `libav*` libraries.
It behaves similar to the old sc_trans program, now abandoned.

## Table of contents

- [Features](#features)

## Features

- Transcodes the content of playlist file source or a live source (broadcaster application) to Icecast/Shoutcast servers.
- On-the-fly transcoding between MP3, Ogg Vorbis, Opus, AAC (ADTS) and FLAC.
- Per-stream sample rate / channel count override (resample/downmix via
  `libswresample`).
- **Playlist mode** loop over a local list with smooth crossfades
  between tracks (natural end-of-track fades, forced skip, and hot
  playlist-file reload, all controllable at runtime via signals).
- - **Live DJ takeover** (`live_enable = 1`): a
  SHOUTcast v1-style TCP listener that lets an authenticated DJ client take
  over the output live, crossfading in/out of the playlist automatically,
  with priority-based "kick" semantics between multiple credentials. IPv6 is supported

## Notes
- There is a minimum duration for the track, in order to allow it to be crossfaded with the previous or the next one. Tracks under 10s, will not be crossfaded


## Warnings
- In theory, the program supports multiple stream definitions (see config file format below) but this is yet to be tested.

## Limitations
- Shoutcast sources and servers support only MP3 and AAC as output formats, so the listener will only decode MP3 and AAC streams.

## Requirements

- A C11 compiler (`gcc` or `clang`).
- `pkg-config`.
- FFmpeg development libraries:
  - `libavformat`
  - `libavcodec`
  - `libavutil`
  - `libswresample`

  Tested against FFmpeg 7.x (`libavformat` 63.x, `libavcodec` 63.x,
  `libavutil` 61.x, `libswresample` 7.x). Older FFmpeg builds may need minor
  API adjustments (some `avcodec_get_supported_config`-style calls require
  a reasonably recent FFmpeg).
- POSIX threads (`pthread`) and `libm`.
- Linux (developed/tested on Linux; relies on POSIX signal handling in
  `main.c`).

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

All stream definitions live exclusively in the config
file; there is no way to define a stream via the command line. Write a
config file describing one or more streams (see
[Config file format](#config-file-format-multi-stream) below), then run:

```sh
./relcast -c /etc/relcast/streams.ini
```

Increase log verbosity (repeatable) or silence everything but errors:

```sh
./relcast -c /etc/relcast/streams.ini -v -v     # debug-level logs
./relcast -c /etc/relcast/streams.ini -q        # errors only
```

## Command-line reference

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

Notes:

- `-c`/`--config <file>` is mandatory; relcast refuses to start without it
  (there is no CLI-only single-stream mode).
- `-v` may be repeated to step through `error -> warn -> info -> debug`
  verbosity levels; `-q` jumps straight to error-only.

## Config file format

A simple INI-like format is supported via
`-c`/`--config`:

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
live_ipv6_listen_port = 8010
live_credential = alice:secret1:10
live_credential = bob:secret2:5

```ini


## Runtime control signals

In addition to the standard `SIGINT`/`SIGTERM` graceful-shutdown handling,
`relcast` responds to a few extra POSIX signals sent to the running process
(`main.c`), useful for operating a long-running instance without a restart:

| Signal     | Effect                                                          |
|------------|------------------------------------------------------------------|
| `SIGINT`, `SIGTERM` | Stop all streams cleanly and exit.                       |
| `SIGHUP`   | Reload the config file (`-c`) live: add newly-defined streams, stop removed ones, and restart any stream whose settings changed - running streams whose config is unchanged keep running undisturbed. |
| `SIGWINCH` | Skip to the next playlist entry now (crossfade immediately) on every currently-running playlist stream. |
| `SIGUSR1`  | Reload each playlist stream's `playlist_file` from disk and restart iteration from the top, without interrupting the track currently playing. |
| `SIGPIPE`  | Ignored (`SIG_IGN`) so a broken output socket surfaces as a normal write error instead of killing the process. |

Example:

```sh
./relcast -c /etc/relcast/streams.ini &
echo $! > /tmp/relcast.pid

# ... edit /etc/relcast/streams.ini ...
kill -HUP "$(cat /tmp/relcast.pid)"      # live config reload

kill -WINCH "$(cat /tmp/relcast.pid)"    # skip current playlist track
kill -USR1 "$(cat /tmp/relcast.pid)"     # reload playlist file(s) from disk
```