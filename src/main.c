/*
 * main.c - relcast entry point
 *
 * Sets up global FFmpeg state, parses configuration, launches one thread per
 * configured stream, and waits for signals to drive process behaviour:
 *   SIGINT/SIGTERM - graceful shutdown of all streams
 *   SIGHUP         - reload the config file live (add/remove/restart streams)
 *   SIGWINCH       - skip to the next playlist entry (all playlist streams)
 *   SIGUSR1        - reload each stream's playlist file from disk
 * Periodically logs per-stream throughput stats.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "relcast.h"
#include "config.h"
#include "log.h"
#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#include <libavformat/avformat.h>
#include <libavutil/log.h>

/* Fixed-size slot table so SIGHUP reload can add/remove/replace individual
 * streams without disturbing the ones that are unaffected. */
static rc_stream_t g_streams[RELCAST_MAX_STREAMS];
static int g_in_use[RELCAST_MAX_STREAMS];
static rc_app_config_t g_app_cfg;

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_reload_config = 0;
static volatile sig_atomic_t g_playlist_skip = 0;
static volatile sig_atomic_t g_playlist_reload = 0;

static void handle_signal(int sig)
{
    switch (sig) {
        case SIGINT:
        case SIGTERM:  g_shutdown = 1; break;
        case SIGHUP:   g_reload_config = 1; break;
        case SIGWINCH: g_playlist_skip = 1; break;
        case SIGUSR1:  g_playlist_reload = 1; break;
        default: break;
    }
}

static void ffmpeg_log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    if (level > AV_LOG_WARNING)
        return; /* suppress verbose/info/debug spam from libav* */

    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    /* strip trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';

    if (level <= AV_LOG_ERROR)
        rc_loge("ffmpeg", "%s", buf);
    else
        rc_logw("ffmpeg", "%s", buf);
}

static void log_stream_started(int slot, rc_stream_t *st)
{
        rc_logi("main", "stream[%d] '%s': playlist:%s%s -> %s:%d%s (%s @ %d bps)",
                slot, st->cfg.name, st->cfg.playlist_file,
                st->cfg.live_enable ? " (+live DJ)" : "",
                st->cfg.output_host, st->cfg.output_port,
                st->cfg.output_mount, rc_codec_to_string(st->cfg.codec),
                st->cfg.bitrate);
 }

static int find_free_slot(void)
{
    for (int i = 0; i < RELCAST_MAX_STREAMS; i++)
        if (!g_in_use[i])
            return i;
    return -1;
}

static void start_all_initial(void)
{
    for (int i = 0; i < g_app_cfg.stream_count; i++) {
        rc_stream_init(&g_streams[i], &g_app_cfg.streams[i]);
        g_in_use[i] = 1;
        log_stream_started(i, &g_streams[i]);
        if (rc_stream_start(&g_streams[i]) != 0) {
            rc_loge("main", "failed to start stream[%d] '%s'", i, g_streams[i].cfg.name);
        }
    }
}

/* Re-read the config file and reconcile the running stream set:
 *  - streams present before but absent now are stopped
 *  - streams absent before but present now are started
 *  - streams present in both, with unchanged settings, are left untouched
 *  - streams present in both, with changed settings, are stopped and
 *    restarted with the new settings */
static void reload_config(void)
{
    rc_app_config_t new_cfg;
    memset(&new_cfg, 0, sizeof(new_cfg));
    new_cfg.log_level = g_app_cfg.log_level;

    if (rc_config_load_file(g_app_cfg.config_path, &new_cfg) != 0) {
        rc_loge("main", "SIGHUP: failed to reload config file '%s', keeping current streams running",
                 g_app_cfg.config_path);
        return;
    }
    if (new_cfg.stream_count == 0) {
        rc_loge("main", "SIGHUP: reloaded config '%s' has no streams, keeping current streams running",
                 g_app_cfg.config_path);
        return;
    }

    int matched_new[RELCAST_MAX_STREAMS] = {0};
    int to_start[RELCAST_MAX_STREAMS];
    int to_start_count = 0;

    /* Step A: reconcile currently running slots against the new config. */
    for (int i = 0; i < RELCAST_MAX_STREAMS; i++) {
        if (!g_in_use[i])
            continue;
        int j = -1;
        for (int k = 0; k < new_cfg.stream_count; k++) {
            if (matched_new[k])
                continue;
            if (strcmp(g_streams[i].cfg.name, new_cfg.streams[k].name) == 0) {
                j = k;
                break;
            }
        }
        if (j < 0) {
            rc_logi("main", "SIGHUP: stream '%s' removed from config, stopping", g_streams[i].cfg.name);
            rc_stream_stop(&g_streams[i]);
            rc_stream_destroy(&g_streams[i]);
            g_in_use[i] = 0;
            continue;
        }
        matched_new[j] = 1;
        if (rc_stream_config_equal(&g_streams[i].cfg, &new_cfg.streams[j])) {
            /* unchanged, leave running untouched */
            continue;
        }
        rc_logi("main", "SIGHUP: stream '%s' settings changed, restarting", g_streams[i].cfg.name);
        rc_stream_stop(&g_streams[i]);
        rc_stream_destroy(&g_streams[i]);
        g_in_use[i] = 0;
        to_start[to_start_count++] = j;
    }

    /* Step B: brand new stream names not seen before. */
    for (int k = 0; k < new_cfg.stream_count; k++) {
        if (!matched_new[k])
            to_start[to_start_count++] = k;
    }

    /* Step C: (re)start everything queued up above in free slots. */
    for (int n = 0; n < to_start_count; n++) {
        int slot = find_free_slot();
        if (slot < 0) {
            rc_loge("main", "SIGHUP: no free stream slot to start '%s' (max %d streams)",
                     new_cfg.streams[to_start[n]].name, RELCAST_MAX_STREAMS);
            continue;
        }
        rc_stream_init(&g_streams[slot], &new_cfg.streams[to_start[n]]);
        g_in_use[slot] = 1;
        rc_logi("main", "SIGHUP: starting stream '%s'", g_streams[slot].cfg.name);
        log_stream_started(slot, &g_streams[slot]);
        if (rc_stream_start(&g_streams[slot]) != 0) {
            rc_loge("main", "failed to start stream '%s'", g_streams[slot].cfg.name);
        }
    }

    /* Keep g_app_cfg.streams roughly in sync for reference (not used for
     * diffing again, since diffing is always done against the live
     * g_streams[].cfg, but useful for consistency / future use). */
    memcpy(g_app_cfg.streams, new_cfg.streams, sizeof(new_cfg.streams));
    g_app_cfg.stream_count = new_cfg.stream_count;

    rc_logi("main", "SIGHUP: config reload complete");
}

int main(int argc, char **argv)
{
    //setvbuf(stdout, NULL, _IOLBF, 0); // Flush on every newline
    if (rc_config_parse_args(argc, argv, &g_app_cfg) != 0)
        return 1;

    rc_log_set_level(g_app_cfg.log_level);

    av_log_set_callback(ffmpeg_log_callback);

    /* avformat_network_init is deprecated/no-op on modern ffmpeg but kept
     * for compatibility with older builds; harmless to call. */
#if LIBAVFORMAT_VERSION_MAJOR < 61
    avformat_network_init();
#endif

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGWINCH, handle_signal);
    signal(SIGUSR1, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    rc_logi("main", "relcast %s starting with %d stream(s)", RELCAST_VERSION, g_app_cfg.stream_count);

    memset(g_in_use, 0, sizeof(g_in_use));
    start_all_initial();

    time_t last_stats = time(NULL);
    while (!g_shutdown) {
        usleep(200 * 1000);

        if (g_reload_config) {
            g_reload_config = 0;
            reload_config();
        }
        if (g_playlist_skip) {
            g_playlist_skip = 0;
            rc_logi("main", "SIGWINCH: skipping to next playlist entry on all playlist streams");
            for (int i = 0; i < RELCAST_MAX_STREAMS; i++)
                if (g_in_use[i])
                    rc_stream_playlist_skip(&g_streams[i]);
        }
        if (g_playlist_reload) {
            g_playlist_reload = 0;
            rc_logi("main", "SIGUSR1: reloading playlist file on all playlist streams");
            for (int i = 0; i < RELCAST_MAX_STREAMS; i++)
                if (g_in_use[i])
                    rc_stream_playlist_reload(&g_streams[i]);
        }

        time_t now = time(NULL);
        if (now - last_stats >= 30) {
            for (int i = 0; i < RELCAST_MAX_STREAMS; i++) {
                if (!g_in_use[i])
                    continue;
                rc_stream_t *st = &g_streams[i];
                rc_logi(st->cfg.name, "stats: running=%d in=%ld B out=%ld B reconnects=%d",
                        atomic_load(&st->running),
                        atomic_load(&st->bytes_in),
                        atomic_load(&st->bytes_out),
                        atomic_load(&st->reconnect_count));
            }
            last_stats = now;
        }
    }

    rc_logi("main", "shutdown signal received, stopping streams...");

    for (int i = 0; i < RELCAST_MAX_STREAMS; i++) {
        if (!g_in_use[i])
            continue;
        rc_stream_stop(&g_streams[i]);
        rc_stream_destroy(&g_streams[i]);
    }

#if LIBAVFORMAT_VERSION_MAJOR < 61
    avformat_network_deinit();
#endif

    rc_logi("main", "relcast stopped cleanly");
    return 0;
}
