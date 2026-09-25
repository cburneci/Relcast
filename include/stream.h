/*
 * stream.h - Single stream transcoding pipeline
 *
 * A stream pipeline:
 *   input (Icecast/Shoutcast source, HTTP) --demux/decode--> PCM frames
 *      --resample/reformat--> encoder --mux--> output (Icecast/Shoutcast push)
 *
 * Each rc_stream_t runs in its own thread and handles reconnect logic
 * for the input side. Output errors currently also trigger a full
 * pipeline restart (re-open input + output), which is the simplest robust
 * strategy for a live relay.
 *
 * When cfg.input_type == RC_INPUT_PLAYLIST, the input side instead reads a
 * looping sequence of local media files listed in cfg.playlist_file (one
 * path per line), with smooth crossfades between tracks. If cfg.live_enable
 * is set, a background listener accepts a single SHOUTcast-v1-style live
 * source connection (e.g. from Mixxx/SAM Broadcaster) that, once
 * authenticated, takes over the output (with a crossfade in/out) until it
 * disconnects, at which point the playlist resumes where it left off.
 */
#ifndef RC_STREAM_H
#define RC_STREAM_H

#include "relcast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize a stream object from a config. Does not start the thread. */
int rc_stream_init(rc_stream_t *st, const rc_stream_config_t *cfg);

/* Start the transcoding thread for this stream. */
int rc_stream_start(rc_stream_t *st);

/* Ask a running stream thread to stop, and join it. */
void rc_stream_stop(rc_stream_t *st);

/* Free resources associated with the stream object (after stop). */
void rc_stream_destroy(rc_stream_t *st);

/* Playlist control, safe to call from signal-driven code (main thread).
 * No-ops if the stream is not currently using a playlist input. */

/* SIGWINCH-style: skip to the next playlist entry immediately (still
 * performs a crossfade into the next track). */
void rc_stream_playlist_skip(rc_stream_t *st);

/* SIGUSR1-style: reload the playlist file from disk, discarding the
 * current position; takes effect once the current track finishes (or is
 * skipped), then playback restarts at the first entry. */
void rc_stream_playlist_reload(rc_stream_t *st);

#ifdef __cplusplus
}
#endif

#endif /* RC_STREAM_H */
