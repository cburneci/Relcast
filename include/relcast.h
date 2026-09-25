/*
 * relcast.h - Common definitions for the relcast Shoutcast/Icecast transcoder
 */
#ifndef RELCAST_H
#define RELCAST_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define RELCAST_VERSION "0.1.0"
#define RELCAST_MAX_STREAMS 64
#define RELCAST_DEFAULT_RECONNECT_DELAY_MS 2000
#define RELCAST_MAX_RECONNECT_DELAY_MS 30000
#define RELCAST_MAX_LIVE_CREDENTIALS 64
#define RELCAST_DEFAULT_CROSSFADE_MS 5000
#define LIVE_USER_NAME_LEN 64
#define MAX_HTTP_GET_LEN 2047
#define MAX_HTTP_METHOD_LEN 7

/* Error results */
#define RC_OK  0
#define RC_ERROR  -1

/* Output server protocol */
typedef enum {
    RC_PROTO_ICECAST = 0,
    RC_PROTO_SHOUTCAST = 1
} rc_output_proto_t;

/* Input source type for a stream */
typedef enum {
    RC_INPUT_URL = 0,      /* remote Icecast/Shoutcast HTTP source */
    RC_INPUT_PLAYLIST = 1  /* local text file listing media files, one per line */
} rc_input_type_t;

/* Codec choice for output encoding */
typedef enum {
    RC_CODEC_MP3 = 0,
    RC_CODEC_VORBIS,
    RC_CODEC_OPUS,
    RC_CODEC_AAC,
    RC_CODEC_FLAC,
    RC_CODEC_COPY /* passthrough, no re-encode */
} rc_codec_t;

/* A single live-source (DJ) login credential with an associated priority.
 * Higher priority values may "kick" a currently connected lower-priority
 * session; equal-or-lower priority connection attempts are rejected while
 * a session is active. */
typedef struct {
    char user[64];
    char password[128];
    int  priority;
} rc_live_credential_t;

/* Per-stream configuration, filled from CLI/config file */
typedef struct {
    char name[64];          /* friendly identifier for logs */

    /* Input selection */
    rc_input_type_t input_type; /* RC_INPUT_URL or RC_INPUT_PLAYLIST */

    /* Input: RC_INPUT_URL */
    char input_url[1024];   /* http(s)://... source stream (Icecast/Shoutcast) */
    char input_user_agent[256];

    /* Input: RC_INPUT_PLAYLIST */
    char playlist_file[1024]; /* text file, one media file path per line */
    int  crossfade_ms;        /* fade duration used for all transitions */
    int  random_order;        /* files are played in random order */

    /* Optional live "DJ override" listener (SHOUTcast v1 protocol), only
     * meaningful when input_type == RC_INPUT_PLAYLIST. When a client
     * successfully authenticates, its audio replaces the playlist output
     * until it disconnects, at which point the playlist resumes from the
     * track it was on when interrupted. */
    int  live_enable;
    int  live_listen_port;
    rc_live_credential_t live_credentials[RELCAST_MAX_LIVE_CREDENTIALS];
    int  live_credential_count;

    /* Output */
    char output_host[256];
    int  output_port;
    char output_mount[256];  /* e.g. /stream.mp3 (icecast) or ignored for shoutcast */
    char output_user[64];    /* icecast user, usually "source" */
    char output_password[128];
    rc_output_proto_t output_proto;
    int  output_tls;

    char ice_name[256];
    char ice_description[512];
    char ice_genre[128];
    char ice_url[256];
    int  ice_public;

    /* Transcode parameters */
    rc_codec_t codec;
    int bitrate;          /* bits per second, e.g. 128000 */
    int sample_rate;      /* 0 = keep source sample rate */
    int channels;         /* 0 = keep source channel count */

    /* Behaviour */
    int reconnect;             /* auto-reconnect input on failure */
    int reconnect_delay_ms;
    int max_reconnect_delay_ms;

} rc_stream_config_t;

/* Runtime state for a single transcoding pipeline (input->output) */
typedef struct rc_stream rc_stream_t;

struct rc_stream {
    rc_stream_config_t cfg;
    pthread_t thread;
    atomic_int running;      /* 1 while thread active */
    atomic_int stop_request; /* set to 1 to ask the thread to terminate */
    atomic_long bytes_in;
    atomic_long bytes_out;
    atomic_int reconnect_count;
    char last_error[256];
    pthread_mutex_t err_mutex;

    /* --- Playlist runtime state (RC_INPUT_PLAYLIST only) ---
     * Persists across pipeline rebuilds/reconnects so that a skip/reload
     * signal or a resumed-after-live playlist position is not lost. */
    pthread_mutex_t playlist_mutex;
    char **playlist_files;       /* array of malloc'd path strings */
    int *playlist_indexes; //random indexes
    int playlist_count;
    int playlist_index;          /* index of the track currently playing */
    int random_order;
    atomic_int playlist_skip_request;   /* SIGWINCH: skip to next track now */
    atomic_int playlist_reload_request; /* SIGUSR1: reload list, restart at 0 */

    /* --- Live DJ listener runtime state (RC_INPUT_PLAYLIST + live_enable) --- */
    pthread_t live_thread;
    atomic_int live_thread_running;
    pthread_mutex_t live_mutex;
    int live_connected;    /* 1 if a DJ session is currently active */
    int live_fd;           /* socket fd of the active session, -1 if none */
    int live_priority;     /* priority of the active session */
    char live_user[LIVE_USER_NAME_LEN];    /* username of the active session */
    atomic_int live_generation; /* bumped on every new accept/kick, lets the
                                   pipeline distinguish "kicked and replaced"
                                   from "genuinely disconnected" */

    /* --- DJ metadata (admin.cgi/updinfo) listener runtime state ---
     * Listens on (live_listen_port - 1) for HTTP GET admin.cgi?pass=..&
     * mode=updinfo&song=.. requests from the live source client, and
     * forwards the decoded song title to the configured output server's
     * own admin.cgi endpoint (output_port - 1). Only meaningful when
     * live_enable is set. */
    pthread_t metadata_thread;
    atomic_int metadata_thread_running;
    pthread_mutex_t metadata_mutex;
    char live_song_title[256]; /* last song title received from the DJ client */
    char last_sent_song_title[256]; /* last title forwarded to the output server (dedup) */
};

/* Global application config */
typedef struct {
    rc_stream_config_t streams[RELCAST_MAX_STREAMS];
    int stream_count;
    char config_path[1024]; /* path passed to -c/--config, kept for SIGHUP reload */
    int log_level; /* 0=error,1=warn,2=info,3=debug */
    char pid_file[256];
    int daemonize;
} rc_app_config_t;

#ifdef __cplusplus
}
#endif

#endif /* RELCAST_H */
