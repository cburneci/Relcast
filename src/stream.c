/*
 * stream.c - Single stream transcoding pipeline implementation
 *
 * Plays a looping local playlist (one media file per
 * line in cfg.playlist_file), with a persistent output connection that
 * survives track changes, smooth crossfades between tracks, and an
 * optional live "DJ override" listener (SHOUTcast v1 protocol) that can
 * interrupt the playlist and hand control back when it disconnects. See
 *  run_playlist_stream() and the rc_track_src_t / rc_out_t machinery.
 *
 * Output URL is built as:
 *   icecast://<user>:<password>@<host>:<port>/<mount>   (protocol=icecast)
 * For protocol=shoutcast, the icecast:// URL/handler is NOT used at all:
 * FFmpeg's "legacy_icecast" AVOption is actually a legacy *Icecast* source
 * mode (HTTP SOURCE + Basic Auth), not the real Shoutcast wire protocol.
 * Instead, a real SHOUTcast v1/ICY handshake is performed manually over a
 * raw tcp://(or tls://) transport - see shoutcast_open()/shoutcast_handshake().
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "stream.h"
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>

#define TAG_MAX 80
#define LIVE_AVIO_BUF_SIZE 32768


/* ===========================================================================
 * Shared helpers (used by both URL-mode and playlist-mode)
 * ===========================================================================
 */

static enum AVCodecID codec_to_avid(rc_codec_t c)
{
    switch (c) {
        case RC_CODEC_MP3:    return AV_CODEC_ID_MP3;
        case RC_CODEC_VORBIS: return AV_CODEC_ID_VORBIS;
        case RC_CODEC_OPUS:   return AV_CODEC_ID_OPUS;
        case RC_CODEC_AAC:    return AV_CODEC_ID_AAC;
        case RC_CODEC_FLAC:   return AV_CODEC_ID_FLAC;
        case RC_CODEC_COPY:   return AV_CODEC_ID_NONE;
        default:              return AV_CODEC_ID_MP3;
    }
}

static const char *codec_content_type(rc_codec_t c)
{
    switch (c) {
        case RC_CODEC_MP3:    return "audio/mpeg";
        case RC_CODEC_VORBIS: return "application/ogg";
        case RC_CODEC_OPUS:   return "application/ogg";
        case RC_CODEC_AAC:    return "audio/aac";
        case RC_CODEC_FLAC:   return "audio/flac";
        case RC_CODEC_COPY:   return "audio/mpeg";
        default:              return "audio/mpeg";
    }
}

/* muxer short name to use with avformat_alloc_output_context2 for each codec.
 * mp3 -> "mp3", vorbis/opus -> "ogg", aac -> "adts", flac -> "flac" */
static const char *codec_muxer_name(rc_codec_t c)
{
    switch (c) {
        case RC_CODEC_MP3:    return "mp3";
        case RC_CODEC_VORBIS: return "ogg";
        case RC_CODEC_OPUS:   return "ogg";
        case RC_CODEC_AAC:    return "adts";
        case RC_CODEC_FLAC:   return "flac";
        case RC_CODEC_COPY:   return "mp3";
        default:              return "mp3";
    }
}

static void set_err(rc_stream_t *st, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&st->err_mutex);
    snprintf(st->last_error, sizeof(st->last_error), "%s", buf);
    pthread_mutex_unlock(&st->err_mutex);
}

/* Build an icecast/http URL for the output PUT connection */
static void build_output_url(const rc_stream_config_t *cfg, char *out, size_t outlen)
{
    char user_enc[256], pass_enc[256];
    const char *hexd = "0123456789ABCDEF";

    #define PCTENC(dstbuf, src) do { \
        size_t di = 0; \
        for (size_t si = 0; src[si] && di + 4 < sizeof(dstbuf); si++) { \
            unsigned char c = (unsigned char)src[si]; \
            if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') { \
                dstbuf[di++] = (char)c; \
            } else { \
                dstbuf[di++] = '%'; \
                dstbuf[di++] = hexd[(c>>4)&0xF]; \
                dstbuf[di++] = hexd[c&0xF]; \
            } \
        } \
        dstbuf[di] = '\0'; \
    } while (0)

    PCTENC(user_enc, cfg->output_user);
    PCTENC(pass_enc, cfg->output_password);
    #undef PCTENC

    char mount[256];
    snprintf(mount, sizeof(mount), "%s", cfg->output_mount);
    if (mount[0] != '/') {
        char tmp[257];
        snprintf(tmp, sizeof(tmp), "/%.255s", mount);
        snprintf(mount, sizeof(mount), "%.255s", tmp);
    }

    snprintf(out, outlen, "icecast://%s:%s@%s:%d%s",
             user_enc, pass_enc, cfg->output_host, cfg->output_port, mount);
}

/* ===========================================================================
 * Real SHOUTcast v1 (ICY) source client handshake
 * ===========================================================================
 *
 * FFmpeg's icecast protocol handler's "legacy_icecast=1" option does NOT
 * implement the actual Shoutcast/ICY source wire protocol: it merely swaps
 * the HTTP request verb from PUT to SOURCE while still speaking HTTP with
 * Basic Auth and capitalized "Ice-*" headers. That is a legacy *Icecast*
 * source mode, not Shoutcast. A real Shoutcast v1 server (and a v2 server
 * running in legacy-compatibility mode) instead expects a much simpler,
 * non-HTTP, line-oriented handshake:
 *
 *   client -> "<password>\n"
 *   server -> "OK2\r\nicy-caps:11\r\n\r\n"   (v2/newer, capability negotiation)
 *          -> "OK\r\n\r\n" or just "OK\n"    (older v1 servers)
 *          -> "invalid password" (or similar) + close, on rejection
 *   client -> "icy-name:...\n"
 *             "icy-genre:...\n"
 *             "icy-pub:0|1\n"
 *             "icy-br:<bitrate_kbps>\n"
 *             "\n"                          (blank line terminates headers)
 *   client -> raw audio bytes (MP3 frames), rest of the connection
 *
 * This handshake is verified against the reference libshout implementation
 * (src/proto_icy.c / proto_xaudiocast.c) and matches relcast's own
 * SHOUTcast v1 *server*-side live-DJ-listener handshake elsewhere in this
 * file (see "Live DJ listener" section below), just from the client side.
 *
 * IMPORTANT: this handshake is implemented over a *raw POSIX socket*, not
 * an FFmpeg AVIOContext (tcp://), even though relcast otherwise relies on
 * FFmpeg for all networking. This is not a stylistic choice: FFmpeg's
 * AVIOContext buffering layer does not support genuine interactive
 * request/response duplex use on a single context opened with
 * AVIO_FLAG_READ_WRITE. Its write-mode flush_buffer() does not reset
 * buf_end, so a avio_r8()/fill_buffer() call performed shortly after a
 * avio_write()+avio_flush() on the same context can return bytes out of
 * our own not-yet-fully-flushed write buffer instead of the peer's actual
 * reply - this was confirmed experimentally: reading the handshake
 * response back this way silently echoed our own just-sent password
 * line instead of the server's greeting. A plain blocking POSIX socket
 * has no such ambiguity, so the handshake (password send / greeting
 * recv / icy-header send) is done directly on the fd. Once the
 * handshake completes, the *same* fd is wrapped in a custom AVIOContext
 * (via avio_alloc_context()) whose write_packet callback is a plain
 * send(); that wrapper is what gets handed to avformat_write_header(),
 * so the muxer only ever writes (never reads) through it - avoiding the
 * buggy interleaving entirely for the streaming phase.
 */

typedef struct {
    int fd;
#ifdef RC_WITH_TLS
    void *ssl; /* SSL* when tls in use, else NULL */
#endif
} rc_shoutcast_io_t;

static int shoutcast_io_write(void *opaque, const uint8_t *buf, int buf_size)
{
    rc_shoutcast_io_t *sio = (rc_shoutcast_io_t *)opaque;
    int total = 0;
    {
        static FILE *dbgf = NULL;
        if (!dbgf) dbgf = fopen("/tmp/relcast_sent_dump.bin", "wb");
        if (dbgf) { fwrite(buf, 1, buf_size, dbgf); fflush(dbgf); }
    }
    while (total < buf_size) {
        ssize_t n = send(sio->fd, buf + total, buf_size - total, MSG_NOSIGNAL);
        if (n < 0) {
            fprintf(stderr, "[DEBUG shoutcast_io_write] send() failed: errno=%d (%s), total_sent_so_far=%d/%d\n",
                    errno, strerror(errno), total, buf_size);
            if (errno == EINTR) continue;
            return AVERROR(errno);
        }
        if (n == 0) return AVERROR(EIO);
        total += (int)n;
    }
    return buf_size;
}

static void shoutcast_io_free(rc_shoutcast_io_t *sio)
{
    if (!sio) return;
    if (sio->fd >= 0) close(sio->fd);
    free(sio);
}

/* Connect a plain blocking TCP socket to host:port with a connect timeout
 * (in seconds). Returns the fd (>=0) on success, or -1 on failure. */
static int raw_tcp_connect(const char *host, int port, int timeout_sec)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int cret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (cret == 0) {
            break; /* connected immediately */
        }
        if (errno != EINPROGRESS) { close(fd); fd = -1; continue; }

        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pret = poll(&pfd, 1, timeout_sec * 1000);
        if (pret <= 0) { close(fd); fd = -1; continue; }

        int soerr = 0; socklen_t slen = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        if (soerr != 0) { close(fd); fd = -1; continue; }
        break; /* connected */
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    /* Switch back to blocking mode with a rw timeout for the rest of the
     * (line-based, inherently blocking) handshake. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return fd;
}

/* Read a single '\n'-terminated line from a blocking fd (subject to
 * SO_RCVTIMEO), up to bufsize-1 bytes. Strips a trailing '\r' if present.
 * Returns line length on success, -1 on error/timeout/EOF. */
static int raw_read_line(int fd, char *buf, size_t bufsize)
{
    size_t n = 0;
    while (n + 1 < bufsize) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1; /* EOF */
        if (c == '\n') {
            buf[n] = '\0';
            if (n > 0 && buf[n - 1] == '\r') buf[n - 1] = '\0';
            return (int)n;
        }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return (int)n;
}

static int raw_send_all(int fd, const char *data, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, data + total, len - total, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

/* Perform the real SHOUTcast v1/ICY source handshake over 'fd' using the
 * metadata in cfg. On success, *out_pb is set to a new AVIOContext (owning
 * 'fd') ready to be handed to avformat_write_header(); the muxer's writes
 * will be send()'d verbatim to 'fd'. On failure, 'fd' is closed by this
 * function and a negative AVERROR is returned. */
static int shoutcast_handshake(const rc_stream_config_t *cfg, int fd,
                                const char *content_type, AVIOContext **out_pb)
{
    char line[512];
    int n;

    /* Send the password unconditionally as the very first line, matching
     * libshout's default (non-poked) behaviour and every real-world
     * Shoutcast v1 source client. */
    snprintf(line, sizeof(line), "%s\n", cfg->output_password);
    if (raw_send_all(fd, line, strlen(line)) < 0) { close(fd); return AVERROR(EIO); }

    n = raw_read_line(fd, line, sizeof(line));
    if (n < 0) { close(fd); return AVERROR(EIO); }

    /* Accept "OK2..." (v2/newer) or a line containing "OK" (older v1). Any
     * other response (e.g. "invalid password") is a rejection. */
    if (strncmp(line, "OK", 2) != 0) {
        close(fd);
        return AVERROR(EACCES);
    }
    if (strncmp(line, "OK2", 3) == 0) {
        /* v2-style response is followed by "icy-caps:<n>" then a blank
         * line; consume both before sending our headers. */
        int caps_n = raw_read_line(fd, line, sizeof(line));
        if (caps_n < 0) { close(fd); return AVERROR(EIO); }
        if (line[0] != '\0') {
            /* Some servers omit the extra blank line separator; if the
             * icy-caps line already came back blank, we already consumed
             * the separator. Otherwise consume the real blank separator. */
            int blank_n = raw_read_line(fd, line, sizeof(line));
            if (blank_n < 0) { close(fd); return AVERROR(EIO); }
        }
    }

    int bitrate_kbps = cfg->bitrate > 0 ? cfg->bitrate / 1000 : 0;
    char hdrbuf[1024];
    int hlen = snprintf(hdrbuf, sizeof(hdrbuf),
        "icy-name:%s\n"
        "icy-url:%s\n"
        "icy-genre:%s\n"
        "icy-pub:%d\n"
        "icy-br:%d\n"
        "%s%s%s"
        "\n",
        cfg->ice_name[0] ? cfg->ice_name : "no name",
        cfg->ice_url[0] ? cfg->ice_url : "http://www.icecast.org/",
        cfg->ice_genre[0] ? cfg->ice_genre : "icecast",
        cfg->ice_public ? 1 : 0,
        bitrate_kbps,
        content_type ? "content-type:" : "", content_type ? content_type : "", content_type ? "\n" : "");
    if (hlen < 0 || (size_t)hlen >= sizeof(hdrbuf)) { close(fd); return AVERROR(EINVAL); }
    if (raw_send_all(fd, hdrbuf, (size_t)hlen) < 0) { close(fd); return AVERROR(EIO); }

    rc_shoutcast_io_t *sio = calloc(1, sizeof(*sio));
    if (!sio) { close(fd); return AVERROR(ENOMEM); }
    sio->fd = fd;

    unsigned char *iobuf = av_malloc(LIVE_AVIO_BUF_SIZE);
    if (!iobuf) { shoutcast_io_free(sio); return AVERROR(ENOMEM); }

    AVIOContext *pb = avio_alloc_context(iobuf, LIVE_AVIO_BUF_SIZE, 1, sio, NULL, shoutcast_io_write, NULL);
    if (!pb) { av_free(iobuf); shoutcast_io_free(sio); return AVERROR(ENOMEM); }
    pb->opaque = sio;

    *out_pb = pb;
    return 0;
}

/* Opens a raw TCP connection to cfg->output_host:port and performs the
 * SHOUTcast v1 handshake, returning a ready-to-write AVIOContext in
 * *out_pb on success (0). On failure, returns a negative AVERROR and
 * *out_pb is left untouched. Note: cfg->output_tls is NOT currently
 * supported for proto=shoutcast (real Shoutcast servers overwhelmingly
 * run plaintext source connections on a dedicated port); if set, this
 * returns AVERROR(ENOSYS). */
static int shoutcast_open(const rc_stream_config_t *cfg, const char *content_type,
                           AVIOContext **out_pb)
{
    if (cfg->output_tls) {
        /* TODO: wrap raw_tcp_connect()'d fd in TLS (e.g. OpenSSL BIO) if a
         * real deployment needs this; not implemented yet. */
        return AVERROR(ENOSYS);
    }

    int fd = raw_tcp_connect(cfg->output_host, cfg->output_port, 10);
    if (fd < 0) return AVERROR(ETIMEDOUT);

    return shoutcast_handshake(cfg, fd, content_type, out_pb);
}

/* Frees an AVIOContext previously returned by shoutcast_open(). Safe to call
 * with a NULL pb or a pb->opaque that isn't a rc_shoutcast_io_t (won't
 * happen in practice since this is only ever called on our own contexts). */
static void shoutcast_close(AVIOContext *pb)
{
    if (!pb) return;
    rc_shoutcast_io_t *sio = (rc_shoutcast_io_t *)pb->opaque;
    /* avio_closep() would try to call our (NULL) read_packet/seek and is not
     * appropriate here since we manually allocated buffer+context; free the
     * buffer and context ourselves, then release the underlying fd. */
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    shoutcast_io_free(sio);
}

/* Percent-encodes 'in' into 'out' (NUL-terminated, truncated to fit
 * outlen), suitable for embedding as a single query-string value in an
 * HTTP GET request line (e.g. admin.cgi?song=<this>). */
static void percent_encode(const char *in, char *out, size_t outlen)
{
    static const char *hexd = "0123456789ABCDEF";
    size_t di = 0;
    if (outlen == 0) return;
    for (size_t si = 0; in[si] != '\0' && di + 4 < outlen; si++) {
        unsigned char c = (unsigned char)in[si];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[di++] = (char)c;
        } else {
            out[di++] = '%';
            out[di++] = hexd[(c >> 4) & 0xF];
            out[di++] = hexd[c & 0xF];
        }
    }
    out[di] = '\0';
}

/* Decodes a percent-encoded ('%XX') and '+'-as-space query string value
 * from 'in' into 'out' (NUL-terminated, truncated to fit outlen). */
static void percent_decode(const char *in, char *out, size_t outlen)
{
    size_t di = 0;
    if (outlen == 0) return;
    for (size_t si = 0; in[si] != '\0' && di + 1 < outlen; si++) {
        if (in[si] == '%' && isxdigit((unsigned char)in[si+1]) && isxdigit((unsigned char)in[si+2])) {
            char hex[3] = { in[si+1], in[si+2], '\0' };
            out[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (in[si] == '+') {
            out[di++] = ' ';
        } else {
            out[di++] = in[si];
        }
    }
    out[di] = '\0';
}

/* Sends a "GET /admin.cgi?pass=..&mode=updinfo&song=.." HTTP request to
 * the given host:port (the output server's admin/base port, i.e.
 * output_port - 1) using 'password' for auth and 'song_title_encoded' as
 * the song value (e.g. an HTML-encoded title, per relcast's metadata
 * conventions - see html_encode()/send_song_metadata_update()). The song
 * value and password are percent-encoded here before being placed in the
 * query string, since this is what actually goes out over HTTP regardless
 * of any content-level encoding already applied to the string. Reads and
 * discards the HTTP response line. Returns 0 on success, negative on
 * failure. This is a short, best-effort, synchronous call (bounded by a
 * short connect/IO timeout) - failures are non-fatal to the audio
 * pipeline. */
static int send_admin_updinfo(const char *host, int port, const char *password,
                               const char *song_title_encoded)
{
    int fd = raw_tcp_connect(host, port, 3);
    if (fd < 0) return -1;

    char pass_enc[256];
    percent_encode(password, pass_enc, sizeof(pass_enc));

    char song_enc[768];
    char *song_title_stripped = (char *)song_title_encoded;
    if (song_title_stripped)
    {
         char *pend = song_title_stripped + strlen(song_title_stripped);
         while(!isalnum((int)song_title_stripped[0]) && song_title_stripped < pend)
         {
            song_title_stripped++ ;
         }
         percent_encode(song_title_stripped , song_enc, sizeof(song_enc));
    } else {
         percent_encode ("Unrecognizable title", song_enc, sizeof(song_enc));
    }
    
    char req[1024];
    int rlen = snprintf(req, sizeof(req),
        "GET /admin.cgi?pass=%s&mode=updinfo&song=%s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: relcast\r\n"
        "\r\n",
        pass_enc, song_enc, host);
    if (rlen < 0 || (size_t)rlen >= sizeof(req)) { close(fd); return -1; }

    if (raw_send_all(fd, req, (size_t)rlen) < 0) { close(fd); return -1; }

    char line[512];
    raw_read_line(fd, line, sizeof(line)); /* best-effort, ignore result */
    close(fd);
    return 0;
}

/* Forwards a song-title metadata update to this stream's configured output
 * server via its admin.cgi endpoint (output_port - 1), using the stream's
 * output_password. Only meaningful for proto=shoutcast destinations (real
 * Icecast servers use a different metadata-update mechanism); a no-op
 * (returns 0) otherwise. 'song_title_encoded' should already be encoded
 * the way the caller wants it embedded (HTML-encoded, per relcast's
 * playlist/DJ metadata conventions - see run_playlist_stream()). */
static int shoutcast_forward_metadata(const rc_stream_config_t *cfg, const char *song_title_encoded)
{
    if (cfg->output_proto != RC_PROTO_SHOUTCAST) return 0;
    int admin_port = cfg->output_port;
    if (admin_port <= 0) return -1;
    return send_admin_updinfo(cfg->output_host, admin_port, cfg->output_password, song_title_encoded);
}


/* ===========================================================================
 * Playlist-mode: per-track decoded-audio source (local file or live socket)
 * ===========================================================================
 *
 * Every source (whether a playlist file or a live DJ connection) is decoded
 * and resampled into a common "mixing format": planar float (FLTP) at the
 * stream's fixed target sample rate/channel count. This makes it trivial to
 * crossfade between two arbitrary sources (different codecs/rates/channel
 * counts) by simply scaling and summing sample values.
 */

typedef struct {
    AVFormatContext *ifmt_ctx;
    AVCodecContext *dec_ctx;
    int stream_idx;
    SwrContext *swr;            /* decoder fmt -> FLTP @ target_rate/target_ch */
    AVFrame *dec_frame;
    AVPacket *pkt;
    struct AVAudioFifo *fifo;   /* holds converted FLTP samples awaiting consumption */

    int is_live;
    int is_synced;              /* stream is synced*/
    int fd;                     /* live only, -1 otherwise */
    int is_input_opened;
    AVIOContext *avio;          /* live only, custom read-only context */

    int64_t samples_consumed;   /* running count of samples handed to the mixer */
    int64_t samples_total;      /* estimated total samples, -1 if unknown */
    int input_eof;              /* 1 once no more packets/frames will come */

    int target_rate;
    int target_ch;
    int codec_type;

    char *song_title;       /* HTML-encoded "now playing" metadata value */
    char *label;
    rc_stream_t *st;
} rc_track_src_t;


static int find_audio_sync_word(uint8_t *ptr, size_t size, int type) {
   if (type < 1 || type >2) {
      return -1;
   }
   int retval = -1; 
   for (size_t i = 0; i < size - 1; i++) {
        
            // --- Caz MP3 ---
            //Caută 0xFF urmat de 0xE0-0xFF (cei 11 biți de 1
             if (type == 1 &&  ptr[i] == 0xFF && (ptr[i+1] & 0xE0) == 0xE0) {
                retval = i;
                break;   
             }
            // --- Caz AAC ADTS ---
            // Caută 0xFF urmat de 0xF0 sau 0xF1 (cei 12 biți de 1)
            if (type == 2  &&  ptr[i] == 0xFF && (ptr[i+1] & 0xF0) == 0xF0) {    
               retval = i; 
               break; // Returnează indexul unde începe primul cadru audio
             } 
   }      
    return retval; // Nu s-a găsit încă un cadru valid în acest chunk
}

/* Live socket read callback for a custom AVIOContext. Blocking socket read
 * with EINTR retry; returns AVERROR_EOF on orderly close, AVERROR(errno) on
 * hard error. */
static int live_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    rc_track_src_t *t = (rc_track_src_t *)opaque;
    uint8_t *tempbuf = (uint8_t *)malloc(buf_size);
    if (!tempbuf) return AVERROR(ENOMEM);
    for (;;) {
        ssize_t n = recv(t->fd, tempbuf, buf_size, 0);
        if (n < 0) {
            // Check if the OS unblocked because of the timeout
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                free (tempbuf);
                return AVERROR(ETIMEDOUT); // Tell FFmpeg the read timed out
            }
            if (errno == EINTR) continue;
            free (tempbuf);
            return AVERROR(errno);
        }
         if (n == 0) {
            free (tempbuf);  
            return AVERROR_EOF;
        }
        if (n > 0) {
            //cazul bun, citim date corecte sau, nu știm ce codec avem
            if (t->codec_type == 0 || t->is_synced) {            
                memcpy (buf, tempbuf, n);
                free (tempbuf);
                return n;
            }
            if (!t->is_synced) {
                uint8_t *tempptr = tempbuf;
                int tempsize = n;
                int sync_offset = find_audio_sync_word(tempptr, n, t->codec_type);
                
                if (sync_offset >= 0) {
                    // Am găsit primul cadru valid! Sărim peste junk-ul de dinainte.
                    tempptr += sync_offset;
                    tempsize -= sync_offset;
                    t->is_synced = 1; // Marcam că stream-ul este acum sincronizat
                    //datele sunt acum în tempptr, și au tempsize
                    memcpy(buf, tempptr,tempsize);
                    free (tempbuf);
                    return tempsize;
                } else {
                    //mai trebuie să citim   și alte date
                    continue;        
                }  //if (sync_offset >= 0)
    
            } 
        }
    }
}

static void track_close(rc_track_src_t *t)
{
    if (!t) return;
    rc_logi (t->st->cfg.name,"closing track");
   

    if (t->ifmt_ctx) {
        /* For live tracks, ifmt_ctx->pb is our custom AVIOContext; per the
         * standard ffmpeg custom-IO pattern (see doc/examples/avio_reading.c)
         * avformat_close_input() does not free a manually-assigned pb, so we
         * free it (and its current internal buffer, which avio may have
         * reallocated) ourselves afterwards. */
       
       
        if (t->avio) {
            av_freep(&t->avio->buffer);
            avio_context_free(&t->avio);
        } 
        if (t->is_input_opened)
        {
            avformat_close_input(&t->ifmt_ctx);
        }
        else
        {
           if (t->ifmt_ctx) avformat_free_context(t->ifmt_ctx) ;
        }   
    }
    if (t->fifo) av_audio_fifo_free(t->fifo);
    if (t->swr) swr_free(&t->swr);
    if (t->dec_frame) av_frame_free(&t->dec_frame);
    if (t->pkt) { av_packet_free(&t->pkt);}
    if (t->dec_ctx) avcodec_free_context(&t->dec_ctx);
    if (t->label) free (t->label);
    if (t->song_title) free (t->song_title);
    
    if (t->is_live && t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    t->input_eof = 1;
    free(t);
}

static rc_track_src_t *track_alloc_common(rc_stream_t *st, int target_rate, int target_ch)
{
    rc_track_src_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fd = -1;
    t->is_input_opened = 0;
    t->target_rate = target_rate;
    t->target_ch = target_ch;
    t->samples_total = -1;
    t->dec_frame = av_frame_alloc();
    t->pkt = av_packet_alloc();
    t->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, target_ch, 1);
    t->ifmt_ctx = NULL;
    t->dec_ctx = NULL;
    t->avio = NULL;
    t->swr = NULL;
    t->label = NULL;
    t->song_title = NULL; 
    t->st = st;
    if (!t->dec_frame || !t->pkt || !t->fifo) {
        track_close(t);
        return NULL;
    }
    return t;
}

/* Common decoder+swr setup once t->ifmt_ctx/stream_idx are known. */
static int track_setup_decoder(rc_track_src_t *t)
{
    AVStream *ist = t->ifmt_ctx->streams[t->stream_idx];
    const AVCodec *dec = avcodec_find_decoder(ist->codecpar->codec_id);
    if (!dec) return -1;

    t->dec_ctx = avcodec_alloc_context3(dec);
    if (!t->dec_ctx) return -1;
    if (avcodec_parameters_to_context(t->dec_ctx, ist->codecpar) < 0) return -1;
    t->dec_ctx->pkt_timebase = ist->time_base;
    if (avcodec_open2(t->dec_ctx, dec, NULL) < 0) return -1;

    AVChannelLayout target_layout;
    av_channel_layout_default(&target_layout, t->target_ch);
    int ret = swr_alloc_set_opts2(&t->swr,
        &target_layout, AV_SAMPLE_FMT_FLTP, t->target_rate,
        &t->dec_ctx->ch_layout, t->dec_ctx->sample_fmt, t->dec_ctx->sample_rate,
        0, NULL);
    av_channel_layout_uninit(&target_layout);
    if (ret < 0 || !t->swr) return -1;
    if (swr_init(t->swr) < 0) return -1;

    if (t->ifmt_ctx->duration != AV_NOPTS_VALUE && t->ifmt_ctx->duration > 0) {
        t->samples_total = av_rescale(t->ifmt_ctx->duration, t->target_rate, AV_TIME_BASE);
    }

    return 0;
}

/* HTML-encodes 'in' into 'out' (NUL-terminated, truncated to fit outlen).
 * Escapes the characters that matter for embedding a value inside HTML
 * text/attributes: & < > " ' */
/*static void html_encode(const char *in, char *out, size_t outlen)
{
    size_t di = 0;
    if (outlen == 0) return;
    for (size_t si = 0; in[si] != '\0'; si++) {
        const char *ent = NULL;
        switch (in[si]) {
            case '&':  ent = "&amp;";  break;
            case '<':  ent = "&lt;";   break;
            case '>':  ent = "&gt;";   break;
            case '"':  ent = "&quot;"; break;
            case '\'': ent = "&#39;";  break;
            default: break;
        }
        if (ent) {
            size_t elen = strlen(ent);
            if (di + elen >= outlen) break;
            memcpy(out + di, ent, elen);
            di += elen;
        } else {
            if (di + 1 >= outlen) break;
            out[di++] = in[si];
        }
    }
    out[di] = '\0';
}*/

/* Extracts the file name from 'path' (strips leading directories) and
 * strips a trailing ".ext" extension (if any), writing the result into
 * 'out'. */
static char* basename_no_ext(const char *path)
{
    size_t namelen = 0;
    char *out = NULL;
    char *base = strrchr(path, '/');
    base = base ? base + 1 : (char*)path;
    char *dot = strrchr(base, '.');
    if (dot) { 
         namelen = dot - base;
    } else {
         namelen = strlen(base);
    }
    if (namelen) {
          out = malloc(namelen + 1);
          if (out) {
              memcpy(out, base, namelen);
              out[namelen] = '\0';
          }
    }
    return out;    
}

static rc_track_src_t *track_open_file(const char *path, int target_rate, int target_ch)
{
    rc_track_src_t *t = track_alloc_common(NULL, target_rate, target_ch);
    if (t == NULL) return NULL;
    if (path == NULL) return NULL;
    t->is_live = 0;
    t->is_synced = 1;
    static size_t path_max = 0;
    static size_t name_max = 0;
    if (path_max == 0) {
        path_max = pathconf("/", _PC_PATH_MAX);
    }
    if (name_max == 0) {
        name_max = pathconf("/", _PC_NAME_MAX);
    }
    size_t label_len = strlen(path) < path_max ? strlen(path) : path_max;
    t->label = malloc(label_len + 1);
    if (t->label == NULL) {
        return NULL;
    }
    memcpy(t->label, path, label_len);
    t->label[label_len] = '\0';
    
    
    char *title = basename_no_ext(path);
    if (title == NULL) {
         track_close(t);
         return NULL;
    }
    t->song_title = title;   
    //no need to html encode for now
    //html_encode(title, t->song_title, sizeof(t->song_title));
    
    if (avformat_open_input(&t->ifmt_ctx, path, NULL, NULL) < 0) {
        track_close(t);
        return NULL;
    }
    if (avformat_find_stream_info(t->ifmt_ctx, NULL) < 0) {
        track_close(t);
        return NULL;
    }
    t->stream_idx = av_find_best_stream(t->ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (t->stream_idx < 0) {
        track_close(t);
        return NULL;
    }
    if (track_setup_decoder(t) != 0) {
        track_close(t);
        return NULL;
    }
    return t;
}

#define MAX_PROBE_SIZE 8192

static rc_track_src_t *track_open_live(rc_stream_t *st,  int fd, const char *user, int target_rate, int target_ch)
{
    rc_track_src_t *t = track_alloc_common(st, target_rate, target_ch);
    if (!t) return NULL;
    t->is_live = 1;
    t->fd = fd;
    t->is_synced = 0;
    t->codec_type = 0;
    t->label = (char*)malloc(LIVE_USER_NAME_LEN);
    if (t->label == NULL)
    {
        return NULL;
    } 
    snprintf(t->label, LIVE_USER_NAME_LEN, "live:%s", user ? user : "?");

    unsigned char *avio_buf = av_malloc(LIVE_AVIO_BUF_SIZE);
    if (!avio_buf) {  return NULL; }

    t->avio = avio_alloc_context(avio_buf, LIVE_AVIO_BUF_SIZE, 0, t, live_read_packet, NULL, NULL);
    if (!t->avio) { /*track_close(t)*/; return NULL; }

    t->ifmt_ctx = avformat_alloc_context();
    if (!t->ifmt_ctx) { track_close(t); return NULL; }
    t->ifmt_ctx->pb = t->avio;

    /* SHOUTcast v1 sources are, by convention/protocol history, plain MP3
     * audio with no container framing beyond the MPEG frame headers
     * themselves, so we force the mp3 demuxer rather than relying on
     * probing over a live, non-seekable socket. */
   
    const AVInputFormat *fmt = NULL;
    //using av_probe_input_buffer2 to find out the stream type
   int ret = av_probe_input_buffer2(t->avio, &fmt, NULL, NULL, 0, MAX_PROBE_SIZE);  
   if (ret >= 0 && fmt != NULL) {
      rc_logi (st->cfg.name,"Detected stream format: %s (%s)", fmt->name, fmt->long_name);
      if (strncmp(fmt->name,"mp3",strlen("mp3")) == 0) {
         t->codec_type = 1;
      } else if (strncmp(fmt->name,"aac",strlen("aac")) == 0) {
         t->codec_type = 2;
      } else {
         t->codec_type = 0; //other format
         rc_loge (st->cfg.name,"Unsupported format. Closing track");
         track_close(t); 
         return NULL; 
      }
      // Rezultatul va fi "mp3" sau "aac" (care acoperă AAC/AAC+)
   } else {
      rc_loge(st->cfg.name, "Could not detect the stream format: assuming mp3");
      fmt = av_find_input_format("mp3");
      t->codec_type = 1;
   }
   if (!fmt) { track_close(t); return NULL; }
 
    if (avformat_open_input(&t->ifmt_ctx, NULL, fmt, NULL) < 0) {
        /* avformat_open_input frees ifmt_ctx on failure; avoid double-free */
        t->ifmt_ctx = NULL;
        track_close(t);
        return NULL;
      
    }
    t->is_input_opened = 1;
    if (avformat_find_stream_info(t->ifmt_ctx, NULL) < 0) {
        track_close(t);
        return NULL;
    }
    t->stream_idx = av_find_best_stream(t->ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (t->stream_idx < 0) {
        track_close(t);
        return NULL;
    }
    if (track_setup_decoder(t) != 0) {
        track_close(t);
        return NULL;
    }
    t->samples_total = -1; /* live source: duration unknown/unbounded */
    return t;
}

/* Attempt to make progress on decoding: read one packet, decode any frames
 * it yields, push the resulting FLTP samples into t->fifo.
 * Returns: 1 = progress made (fifo may have grown), 0 = source exhausted
 * (clean EOF or unrecoverable read error), -1 = hard allocation error. */
static int track_fill_more(rc_track_src_t *t)
{
    if (t->input_eof)
        return 0;

    av_packet_unref(t->pkt);
    int ret = av_read_frame(t->ifmt_ctx, t->pkt);
    if (ret < 0) {
        /* Clean EOF or read error (incl. live socket disconnect): flush the
         * decoder's internal buffer, then mark this source exhausted. */
        avcodec_send_packet(t->dec_ctx, NULL);
        for (;;) {
            ret = avcodec_receive_frame(t->dec_ctx, t->dec_frame);
            if (ret < 0) break;

            int64_t delay = swr_get_delay(t->swr, t->dec_ctx->sample_rate);
            int out_samples = (int)av_rescale_rnd(delay + t->dec_frame->nb_samples,
                    t->target_rate, t->dec_ctx->sample_rate, AV_ROUND_UP);
            if (out_samples <= 0) out_samples = 1;
            uint8_t **converted = NULL;
            int linesize = 0;
            if (av_samples_alloc_array_and_samples(&converted, &linesize, t->target_ch,
                    out_samples, AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                int n = swr_convert(t->swr, converted, out_samples,
                        (const uint8_t **)t->dec_frame->extended_data, t->dec_frame->nb_samples);
                if (n > 0) {
                    if (av_audio_fifo_realloc(t->fifo, av_audio_fifo_size(t->fifo) + n) < 0)
                        break;
                    av_audio_fifo_write(t->fifo, (void **)converted, n);
                }
                av_freep(&converted[0]);
                av_freep(&converted);
            }
            av_frame_unref(t->dec_frame);
        }
        t->input_eof = 1;
        return av_audio_fifo_size(t->fifo) > 0 ? 1 : 0;
    }

    if (t->pkt->stream_index != t->stream_idx) {
        av_packet_unref(t->pkt);
        return 1; /* nothing decoded, but not EOF either; caller will retry */
    }
    if (t->st != NULL) {
        atomic_fetch_add(&t->st->bytes_in, t->pkt->size);
    }
    ret = avcodec_send_packet(t->dec_ctx, t->pkt);
    av_packet_unref(t->pkt);
    if (ret < 0) {
        /* Skip bad packet; not fatal for playlist/live playback. */
        return 1;
    }

    for (;;) {
        ret = avcodec_receive_frame(t->dec_ctx, t->dec_frame);
        if (ret == AVERROR(EAGAIN))
            break; 
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0)
            break;

        int64_t delay = swr_get_delay(t->swr, t->dec_ctx->sample_rate);
        int out_samples = (int)av_rescale_rnd(delay + t->dec_frame->nb_samples,
                t->target_rate, t->dec_ctx->sample_rate, AV_ROUND_UP);
        if (out_samples <= 0) out_samples = 1;
        uint8_t **converted = NULL;
        int linesize = 0;
        if (av_samples_alloc_array_and_samples(&converted, &linesize, t->target_ch,
                out_samples, AV_SAMPLE_FMT_FLTP, 0) >= 0) {
            int n = swr_convert(t->swr, converted, out_samples,
                    (const uint8_t **)t->dec_frame->extended_data, t->dec_frame->nb_samples);
            if (n > 0) {
                if (av_audio_fifo_realloc(t->fifo, av_audio_fifo_size(t->fifo) + n) < 0)
                    break;
                av_audio_fifo_write(t->fifo, (void **)converted, n);
            } 
            av_freep(&converted[0]);
            av_freep(&converted);
        }
        av_frame_unref(t->dec_frame);
    }

    return 1;
}

/* Fetch up to `want` samples per channel of FLTP audio from this track into
 * caller-provided planar buffers out[0..target_ch-1] (each with room for
 * `want` floats). Returns the number of samples actually written (may be
 * less than `want` at end of track, 0 if the track is fully exhausted). */
static int track_get_samples(rc_track_src_t *t, float **out, int want)
{
    if (t == NULL) {
       return 0;
    }
    while (av_audio_fifo_size(t->fifo) < want && !t->input_eof) {
        if (track_fill_more(t) <= 0)
            break;
    }

    int avail = av_audio_fifo_size(t->fifo);
    int n = avail < want ? avail : want;
    if (n <= 0)
        return 0;

    int got = av_audio_fifo_read(t->fifo, (void **)out, n);
    if (got < 0) return 0;
    t->samples_consumed += got;
    return got;
}

/* ===========================================================================
 * Playlist-mode: persistent output (encoder + muxer), independent of which
 * track/source is currently feeding it. Survives track-to-track transitions;
 * only torn down and rebuilt on an actual output I/O error.
 * ===========================================================================
 */

typedef struct {
    AVFormatContext *ofmt_ctx;
    AVCodecContext *enc_ctx;
    SwrContext *post_swr; /* FLTP @ target -> enc_ctx->sample_fmt (format-only) */
    struct AVAudioFifo *fifo; /* holds samples in enc_ctx->sample_fmt */
    AVFrame *enc_frame;
    AVPacket *pkt;
    int out_stream_idx;
    int64_t next_pts;
    int target_rate;
    int target_ch;
    char tag[TAG_MAX];
    int is_shoutcast_output; /* 1 if ofmt_ctx->pb was opened via shoutcast_open() */
} rc_out_t;

static void out_close(rc_out_t *o)
{
    if (!o) return;
    if (o->fifo) av_audio_fifo_free(o->fifo);
    if (o->post_swr) swr_free(&o->post_swr);
    if (o->enc_frame) av_frame_free(&o->enc_frame);
    if (o->pkt) av_packet_free(&o->pkt);
    if (o->enc_ctx) avcodec_free_context(&o->enc_ctx);
    if (o->ofmt_ctx) {
        if (!(o->ofmt_ctx->oformat->flags & AVFMT_NOFILE) && o->ofmt_ctx->pb) {
            if (o->is_shoutcast_output)
                shoutcast_close(o->ofmt_ctx->pb);
            else
                avio_closep(&o->ofmt_ctx->pb);
            o->ofmt_ctx->pb = NULL;
        }
        avformat_free_context(o->ofmt_ctx);
    }
    free(o);
}

static rc_out_t *out_open(rc_stream_t *st, int target_rate, int target_ch)
{
    rc_out_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    snprintf(o->tag, sizeof(o->tag), "%s", st->cfg.name);
    o->target_rate = target_rate;
    o->target_ch = target_ch;

    enum AVCodecID enc_id = codec_to_avid(st->cfg.codec);
    const AVCodec *enc = avcodec_find_encoder(enc_id);
    if (!enc) { set_err(st, "no encoder available for codec %s", rc_codec_to_string(st->cfg.codec)); goto fail; }

    o->enc_ctx = avcodec_alloc_context3(enc);
    if (!o->enc_ctx) { set_err(st, "avcodec_alloc_context3 (encoder) failed"); goto fail; }

    o->enc_ctx->sample_rate = target_rate;
    av_channel_layout_default(&o->enc_ctx->ch_layout, target_ch);
    o->enc_ctx->time_base = (AVRational){1, target_rate};
    o->enc_ctx->bit_rate = st->cfg.bitrate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 3, 100) // FFmpeg 7.0+
    const enum AVSampleFormat *supported_fmts = NULL;
    int n_fmts = 0;
    avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                  (const void **)&supported_fmts, &n_fmts);
    enum AVSampleFormat chosen_fmt = (supported_fmts && n_fmts > 0) ? supported_fmts[0] : AV_SAMPLE_FMT_FLTP;
#else
    const enum AVSampleFormat *supported_fmts = enc->sample_fmts;
    enum AVSampleFormat chosen_fmt = (supported_fmts && supported_fmts[0] != AV_SAMPLE_FMT_NONE) ? supported_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
    o->enc_ctx->sample_fmt = chosen_fmt;
    o->enc_ctx->rc_max_rate = st->cfg.bitrate;
    o->enc_ctx->rc_min_rate = st->cfg.bitrate;
    o->enc_ctx->rc_buffer_size = st->cfg.bitrate; 

    int ret = avcodec_open2(o->enc_ctx, enc, NULL);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        set_err(st, "avcodec_open2 (encoder) failed: %s", errbuf);
        goto fail;
    }

    rc_logi(o->tag, "output codec: %s, %d Hz, %d ch, %lld bps",
            enc->name, o->enc_ctx->sample_rate, o->enc_ctx->ch_layout.nb_channels,
            (long long)o->enc_ctx->bit_rate);

    AVChannelLayout src_layout;
    av_channel_layout_default(&src_layout, target_ch);
    ret = swr_alloc_set_opts2(&o->post_swr,
        &o->enc_ctx->ch_layout, o->enc_ctx->sample_fmt, target_rate,
        &src_layout, AV_SAMPLE_FMT_FLTP, target_rate,
        0, NULL);
    av_channel_layout_uninit(&src_layout);
    if (ret < 0 || !o->post_swr) { set_err(st, "swr_alloc_set_opts2 (post) failed"); goto fail; }
    if (swr_init(o->post_swr) < 0) { set_err(st, "swr_init (post) failed"); goto fail; }

    const char *muxer_name = codec_muxer_name(st->cfg.codec);
    ret = avformat_alloc_output_context2(&o->ofmt_ctx, NULL, muxer_name, NULL);
    if (ret < 0 || !o->ofmt_ctx) { set_err(st, "avformat_alloc_output_context2 failed"); goto fail; }

    AVStream *ost = avformat_new_stream(o->ofmt_ctx, NULL);
    if (!ost) { set_err(st, "avformat_new_stream failed"); goto fail; }
    o->out_stream_idx = ost->index;
    if (avcodec_parameters_from_context(ost->codecpar, o->enc_ctx) < 0) {
        set_err(st, "avcodec_parameters_from_context failed"); goto fail;
    }
    ost->time_base = o->enc_ctx->time_base;

    char url[1600];
    build_output_url(&st->cfg, url, sizeof(url));

    if (st->cfg.output_proto == RC_PROTO_SHOUTCAST) {
        /* Real SHOUTcast v1/ICY source handshake, see shoutcast_open(). */
        if (!(o->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            ret = shoutcast_open(&st->cfg, codec_content_type(st->cfg.codec), &o->ofmt_ctx->pb);
            if (ret < 0) {
                char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
                set_err(st, "shoutcast handshake failed: %s", errbuf);
                goto fail;
            }
            o->is_shoutcast_output = 1;
        }
    } else {
        AVDictionary *opts = NULL;
        if (st->cfg.ice_name[0]) av_dict_set(&opts, "ice_name", st->cfg.ice_name, 0);
        if (st->cfg.ice_description[0]) av_dict_set(&opts, "ice_description", st->cfg.ice_description, 0);
        if (st->cfg.ice_genre[0]) av_dict_set(&opts, "ice_genre", st->cfg.ice_genre, 0);
        if (st->cfg.ice_url[0]) av_dict_set(&opts, "ice_url", st->cfg.ice_url, 0);
        av_dict_set(&opts, "ice_public", st->cfg.ice_public ? "1" : "0", 0);
        av_dict_set(&opts, "content_type", codec_content_type(st->cfg.codec), 0);
        if (st->cfg.output_tls) av_dict_set(&opts, "tls", "1", 0);
        av_dict_set(&opts, "user_agent", st->cfg.input_user_agent, 0);

        if (!(o->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open2(&o->ofmt_ctx->pb, url, AVIO_FLAG_WRITE, NULL, &opts);
            if (ret < 0) {
                char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
                set_err(st, "avio_open2 failed for output: %s", errbuf);
                av_dict_free(&opts);
                goto fail;
            }
        }
        av_dict_free(&opts);
    }

    ret = avformat_write_header(o->ofmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        set_err(st, "avformat_write_header failed: %s", errbuf);
        goto fail;
    }

    o->fifo = av_audio_fifo_alloc(o->enc_ctx->sample_fmt, target_ch, 1);
    o->enc_frame = av_frame_alloc();
    o->pkt = av_packet_alloc();
    if (!o->fifo || !o->enc_frame || !o->pkt) { set_err(st, "allocation failed"); goto fail; }

    rc_logi(o->tag, "connected to output %s://%s:%d%s",
            (st->cfg.output_proto == RC_PROTO_SHOUTCAST) ? "shoutcast" : "icecast",
            st->cfg.output_host, st->cfg.output_port, st->cfg.output_mount);

    return o;

fail:
    out_close(o);
    return NULL;
}

static int out_drain_and_encode(rc_stream_t *st, rc_out_t *o, int flush)
{
    int frame_size = o->enc_ctx->frame_size;
    if (frame_size <= 0) frame_size = 1152;

    for (;;) {
        int avail = av_audio_fifo_size(o->fifo);
        if (!flush && avail < frame_size) break;
        if (avail <= 0) break;

        int want = flush ? avail : frame_size;
        if (want > frame_size) want = frame_size;

        av_frame_unref(o->enc_frame);
        o->enc_frame->format = o->enc_ctx->sample_fmt;
        o->enc_frame->sample_rate = o->enc_ctx->sample_rate;
        av_channel_layout_copy(&o->enc_frame->ch_layout, &o->enc_ctx->ch_layout);
        o->enc_frame->nb_samples = want;
        if (av_frame_get_buffer(o->enc_frame, 0) < 0) { set_err(st, "av_frame_get_buffer failed"); return -1; }

        int nread = av_audio_fifo_read(o->fifo, (void **)o->enc_frame->data, want);
        if (nread < want) o->enc_frame->nb_samples = nread;
        o->enc_frame->pts = o->next_pts;
        o->next_pts += nread;

        int ret = avcodec_send_frame(o->enc_ctx, o->enc_frame);
        if (ret < 0) { set_err(st, "avcodec_send_frame (encoder) failed"); return -1; }

        for (;;) {
            av_packet_unref(o->pkt);
            ret = avcodec_receive_packet(o->enc_ctx, o->pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { set_err(st, "avcodec_receive_packet (encoder) failed"); return -1; }

            o->pkt->stream_index = o->out_stream_idx;
            av_packet_rescale_ts(o->pkt, o->enc_ctx->time_base,
                                  o->ofmt_ctx->streams[o->out_stream_idx]->time_base);
            int pkt_size = o->pkt->size;
            ret = av_interleaved_write_frame(o->ofmt_ctx, o->pkt);
            if (ret < 0) {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                set_err(st, "av_interleaved_write_frame failed: %s (%d)", errbuf, ret);
                return -1;
            }
            atomic_fetch_add(&st->bytes_out, pkt_size);
        }

        if (flush && nread == 0) break;
    }
    return 0;
}

/* Push one chunk of mixed FLTP samples (planar, target_ch channels, n
 * samples per channel) into the output pipeline. Returns 0 on success, -1
 * on an output error (caller should trigger an output reconnect). */
static int out_push_fltp(rc_stream_t *st, rc_out_t *o, float **planes, int n)
{
    if (n <= 0) return 0;

    int64_t delay = swr_get_delay(o->post_swr, o->target_rate);
    int out_samples = (int)av_rescale_rnd(delay + n, o->target_rate, o->target_rate, AV_ROUND_UP);
    if (out_samples <= 0) out_samples = n;

    uint8_t **converted = NULL;
    int linesize = 0;
    if (av_samples_alloc_array_and_samples(&converted, &linesize, o->target_ch,
            out_samples, o->enc_ctx->sample_fmt, 0) < 0) {
        set_err(st, "av_samples_alloc_array_and_samples failed");
        return -1;
    }

    int converted_samples = swr_convert(o->post_swr, converted, out_samples,
            (const uint8_t **)planes, n);
    if (converted_samples < 0) {
        set_err(st, "swr_convert (post) failed");
        av_freep(&converted[0]); av_freep(&converted);
        return -1;
    }

    int rc = 0;
    if (converted_samples > 0) {
        if (av_audio_fifo_realloc(o->fifo, av_audio_fifo_size(o->fifo) + converted_samples) < 0 ||
            av_audio_fifo_write(o->fifo, (void **)converted, converted_samples) < converted_samples) {
            set_err(st, "audio fifo write failed");
            rc = -1;
        }
    }
    av_freep(&converted[0]);
    av_freep(&converted);

    if (rc == 0)
        rc = out_drain_and_encode(st, o, 0);

    return rc;
}

static void out_flush_and_finish(rc_out_t *o, rc_stream_t *st)
{
    out_drain_and_encode(st, o, 1);
    avcodec_send_frame(o->enc_ctx, NULL);
    for (;;) {
        av_packet_unref(o->pkt);
        int ret = avcodec_receive_packet(o->enc_ctx, o->pkt);
        if (ret < 0) break;
        o->pkt->stream_index = o->out_stream_idx;
        av_packet_rescale_ts(o->pkt, o->enc_ctx->time_base,
                              o->ofmt_ctx->streams[o->out_stream_idx]->time_base);
        av_interleaved_write_frame(o->ofmt_ctx, o->pkt);
    }
    av_write_trailer(o->ofmt_ctx);
}

/* ===========================================================================
 * Playlist file loading / advancing
 * ===========================================================================
 */

static void idx_shuffle(int **idx, int cnt)
{
   int *indexes = *idx; 
   for (int i = cnt - 1; i > 0; i--) {
        // Generează un index aleatoriu j între 0 și i (inclusiv)
        int j = rand() % (i + 1);

        // Schimbă elementul de pe poziția i cu cel de pe poziția j
        int temp = indexes[i];
        indexes[i] = indexes[j];
        indexes[j] = temp;
    }
}


static void playlist_free_list(rc_stream_t *st)
{
    if (st->playlist_files) {
        for (int i = 0; i < st->playlist_count; i++)
            free(st->playlist_files[i]);
        free(st->playlist_files);
    }
    st->playlist_files = NULL;
    st->playlist_count = 0;
    free (st->playlist_indexes);
    st->playlist_indexes = NULL;
}

static int playlist_load(rc_stream_t *st)
{
    FILE *f = fopen(st->cfg.playlist_file, "r");
    if (!f) {
        rc_loge(st->cfg.name, "cannot open playlist file '%s': %s",
                st->cfg.playlist_file, strerror(errno));
        return -1;
    }

    char **arr = NULL;
    int *idx_arr = NULL;
    int cnt = 0, cap = 0;
    char line[2048];

    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        if (cnt >= cap) {
            cap = cap ? cap * 2 : 16;
            char **tmp = realloc(arr, (size_t)cap * sizeof(char *));
            if (!tmp) { fclose(f); for (int i=0;i<cnt;i++) free(arr[i]); free(arr); return -1; }
            arr = tmp;
        }
        arr[cnt++] = strdup(p);
    }
    fclose(f);
    
    idx_arr = malloc(cnt*sizeof(int));
    if (!idx_arr) return -1;
    int i;
    for (i = 0; i < cnt; i++) {
       idx_arr[i] = i;
    }
    
    if (st->cfg.random_order == 1){ 
      idx_shuffle(&idx_arr, cnt);
      rc_logi(st->cfg.name , "The list is shuffled");
    }
   
    pthread_mutex_lock(&st->playlist_mutex);
    playlist_free_list(st);
    st->playlist_files = arr;
    st->playlist_indexes = idx_arr;
    st->playlist_count = cnt;
    st->playlist_index = -1;
    
    pthread_mutex_unlock(&st->playlist_mutex);

    rc_logi(st->cfg.name, "playlist loaded: %d entries from %s", cnt, st->cfg.playlist_file);
    return cnt > 0 ? 0 : -1;
}

/* Advance the shared playlist index by one (wrapping around at the end)
 * and copy the corresponding path out. Returns 0 on success, -1 if the
 * playlist is currently empty. */
static char* playlist_advance(rc_stream_t *st)
{
    pthread_mutex_lock(&st->playlist_mutex);
    int count = st->playlist_count;
    if (count <= 0) {
        pthread_mutex_unlock(&st->playlist_mutex);
        return NULL;
    }
    int idx = (st->playlist_index + 1) % count;
    if (idx == 0 && st->cfg.random_order != 0) {
       idx_shuffle(&st->playlist_indexes, count);
    }
    st->playlist_index = idx;
    //snprintf(outbuf, outbuflen, "%s", st->playlist_files[st->playlist_indexes[idx]]);
    char *rv = st->playlist_files[st->playlist_indexes[idx]];
    pthread_mutex_unlock(&st->playlist_mutex);
    return rv;
}

/* Consume a pending SIGUSR1 reload request, if any, at a track boundary. */
static void maybe_apply_playlist_reload(rc_stream_t *st)
{
    int expected = 1;
    if (atomic_compare_exchange_strong(&st->playlist_reload_request, &expected, 0)) {
        rc_logi(st->cfg.name, "reloading playlist from disk (SIGUSR1), restarting at track 1");
        playlist_load(st);
    }
}

/* Open the next playable playlist track, skipping (and logging) any entries
 * that fail to open, looping the whole list if necessary. Blocks (checking
 * stop_request) if the playlist is empty or entirely unplayable. Returns
 * NULL only if a shutdown was requested meanwhile. */
static rc_track_src_t *acquire_next_playlist_track(rc_stream_t *st, int target_rate, int target_ch)
{
    char *path;

    for (;;) {
        if (atomic_load(&st->stop_request)) return NULL;

        pthread_mutex_lock(&st->playlist_mutex);
        int count = st->playlist_count;
        pthread_mutex_unlock(&st->playlist_mutex);

        if (count <= 0) {
            rc_loge(st->cfg.name, "playlist is empty, retrying in 2s");
            for (int s = 0; s < 20 && !atomic_load(&st->stop_request); s++) usleep(100 * 1000);
            continue;
        }

        int attempts = 0;
        int opened = 0;
        while (attempts < count) {
            if (atomic_load(&st->stop_request)) return NULL;
            path = playlist_advance(st);
            if (path == NULL) break;
            
            rc_track_src_t *t = track_open_file(path, target_rate, target_ch);
            if (t) { opened = 1; return t; }

            rc_loge(st->cfg.name, "playlist entry not found or unreadable, skipping: %s", path);
            attempts++;
        }

        if (!opened) {
            rc_loge(st->cfg.name, "no playable files in playlist, retrying in 2s");
            for (int s = 0; s < 20 && !atomic_load(&st->stop_request); s++) usleep(100 * 1000);
        }
    }
}

/* ===========================================================================
 * Live DJ listener (SHOUTcast v1 source protocol)
 * ===========================================================================
 *
 * Handshake:
 *   client -> "password\r\n"  OR  "user:password\r\n"   (relcast extension)
 *   server -> "OK2\r\nicy-caps:11\r\n\r\n"  on success
 *          -> "invalid password\r\n" + close   on rejection
 *   client -> icy-name:...\r\n icy-genre:...\r\n ... \r\n   (headers, optional)
 *   client -> raw MP3 audio bytes (rest of the connection)
 *
 * Only one live session may be active per stream at a time. A connecting
 * user with strictly higher priority than the active session kicks it;
 * a connecting user with equal or lower priority is rejected.
 */

static int find_matching_credential(const rc_stream_config_t *cfg, const char *user,
                                     const char *password, rc_live_credential_t *out)
{
    for (int i = 0; i < cfg->live_credential_count; i++) {
        const rc_live_credential_t *c = &cfg->live_credentials[i];
        if (user[0] == '\0' || password[0] == '\0') continue;
        if (strcmp(c->password, password) != 0) continue;
        if (strcmp(c->user, user) != 0) continue;
        *out = *c;
        return 1;
    }
    return 0;
}

static void send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
//TODO: send with timeout 
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        sent += (size_t)n;
    }
}

/* Read a single CRLF/LF-terminated line from fd, blocking, up to bufsize-1
 * bytes. Returns line length (without terminator) on success, -1 on error
 * or if the peer disconnected before a full line arrived. */
static int read_line(int fd, char *buf, size_t bufsize)
{
    size_t n = 0;
    while (n + 1 < bufsize) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') { buf[n] = '\0'; if (n > 0 && buf[n-1] == '\r') buf[n-1] = '\0'; return (int)strlen(buf); }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return (int)n;
}

static void *live_listener_main(void *arg)
{
    rc_stream_t *st = (rc_stream_t *)arg;
    atomic_store(&st->live_thread_running, 1);
    int listen_fd  = -1;
    int listen_fd6 = -1;

    if (st->cfg.live_ip_addr_4[0] != '\0' && st->cfg.live_listen_port_4 != 0)
    {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        if (listen_fd < 0) {
            rc_loge(st->cfg.name, "live listener: socket() failed: %s", strerror(errno));
        }
        if (listen_fd >= 0) {
            int one = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
             if (inet_pton(AF_INET, st->cfg.live_ip_addr_4, (void*)&(addr.sin_addr)) <= 0) {
                rc_loge(st->cfg.name, "invalid IPv4 listen address %s", st->cfg.live_ip_addr_4);
                close(listen_fd);
                listen_fd = -1;
            }
        }
        if (listen_fd >= 0) {
            addr.sin_port = htons((uint16_t)st->cfg.live_listen_port_4);

            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                rc_loge(st->cfg.name, "live listener IPv4: bind() to port %d failed: %s",
                        st->cfg.live_listen_port_4, strerror(errno));
                close(listen_fd);
                listen_fd = -1;
            }
        }
        if (listen_fd >= 0) {
            if (listen(listen_fd, 1) != 0) {
                rc_loge(st->cfg.name, "live listener IPv4: listen() failed: %s", strerror(errno));
                close(listen_fd);
                listen_fd = -1;
            }
        }
        if (listen_fd >= 0) {     
            rc_logi(st->cfg.name, "live IPv4 DJ listener ready on port %d (SHOUTcast v1)", st->cfg.live_listen_port_4);
        } else {
            rc_loge(st->cfg.name, "NO live IPv4 DJ listener could be started (SHOUTcast v1)");
        }
    }
    
    if (st->cfg.live_ip_addr_6[0] != '\0' && st->cfg.live_listen_port_6 != 0)
    {
        listen_fd6 = socket(AF_INET6, SOCK_STREAM, 0);
        struct sockaddr_in6 addr;
        if (listen_fd6 < 0) {
            rc_loge(st->cfg.name, "live listener: socket6() failed: %s", strerror(errno));
        }
        if (listen_fd6 >= 0) {
            int one = 1;
            setsockopt(listen_fd6, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            int v6_only = 1;
            setsockopt(listen_fd6, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));
            
           
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            if (inet_pton(AF_INET6, st->cfg.live_ip_addr_6, &addr.sin6_addr) <= 0) {
                rc_loge(st->cfg.name, "invalid IPv6 listen address %s", st->cfg.live_ip_addr_6);
                close(listen_fd6);
                listen_fd6 = -1;
            }
        }
        if (listen_fd6 >= 0) {
            addr.sin6_port = htons((uint16_t)st->cfg.live_listen_port_6);
            if (bind(listen_fd6, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                rc_loge(st->cfg.name, "live listener IPv6: bind() to port %d failed: %s",
                        st->cfg.live_listen_port_6, strerror(errno));
                close(listen_fd6);
                listen_fd6 = -1;
            }
        }
        if (listen_fd6 >= 0) {
            if (listen(listen_fd6, 1) != 0) {
                rc_loge(st->cfg.name, "live listener IPv6: listen() failed: %s", strerror(errno));
                close(listen_fd);
                listen_fd6 = -1;
            }
        }
         if (listen_fd6 >= 0) {
            rc_logi(st->cfg.name, "live IPv6 DJ listener ready on port %d (SHOUTcast v1)", st->cfg.live_listen_port_6);
        } else {
            rc_loge(st->cfg.name, "NO live IPv6 DJ listener could be started (SHOUTcast v1)");
        }
    }
    if (listen_fd == -1 && listen_fd6 == -1)
    {
        atomic_store(&st->live_thread_running, 0);
        rc_loge(st->cfg.name, "Live DJ listener thread exiting (SHOUTcast v1)");
        return NULL;
    }
    struct pollfd pfd[2]; /*{ .fd = listen_fd, .events = POLLIN }*/
    pfd[0].fd = listen_fd;
    pfd[0].events = POLLIN;
    pfd[1].fd = listen_fd6;
    pfd[1].events = POLLIN;
    struct sockaddr_storage incoming;
    socklen_t incoming_len;
    char ip_text[INET6_ADDRSTRLEN];
    while (!atomic_load(&st->stop_request)) {
 
        int pr = poll(pfd, 2, 200);
        if (pr <= 0) continue;
        int i;
        for (i = 0; i < 2 ;i++) {
            if (pfd[i].revents & POLLIN) {   
                int cfd = accept(pfd[i].fd, (struct sockaddr*)&incoming, &incoming_len);
                if (incoming.ss_family == AF_INET) {
                    // --- Cazul IPv4 ---
                    struct sockaddr_in *addr_ipv4 = (struct sockaddr_in *)&incoming;
                    inet_ntop(AF_INET, &(addr_ipv4->sin_addr), ip_text, sizeof(ip_text));
                    //int port_client = ntohs(addr_ipv4->sin_port);
                } else if (incoming.ss_family == AF_INET6) {
                    // --- Cazul IPv6 ---
                    struct sockaddr_in6 *addr_ipv6 = (struct sockaddr_in6 *)&incoming;
                    inet_ntop(AF_INET6, &(addr_ipv6->sin6_addr), ip_text, sizeof(ip_text));
                    //int port_client = ntohs(addr_ipv6->sin6_port);
                }
                if (cfd < 0) {
                    rc_loge (st->cfg.name,"Could not accept connection from %s: %s",ip_text, strerror(errno));    
                    continue;
                } else {
                    rc_logi (st->cfg.name,"Incoming connection from %s",ip_text);
                }
                struct timeval tv;
                tv.tv_sec = 10;  // 10 seconds timeout
                tv.tv_usec = 0;

                // Apply the timeout specifically to this new client socket
                if (setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0) {
                   rc_loge (st->cfg.name,"Could not set timeout on socket: %s", strerror(errno));    
                   continue;
                }

                char line[512];
                int n = read_line(cfd, line, sizeof(line));
                if (n < 0) { close(cfd); continue; }

                char user[64] = {0}, password[128] = {0};
                char *colon = strchr(line, ':');
                if (colon) {
                    size_t ulen = (size_t)(colon - line);
                    if (ulen >= sizeof(user)) ulen = sizeof(user) - 1;
                    memcpy(user, line, ulen);
                    user[ulen] = '\0';
                    snprintf(password, sizeof(password), "%.127s", colon + 1);
                } else {
                    snprintf(password, sizeof(password), "%.127s", line);
                }

                rc_live_credential_t cred;
                if (!find_matching_credential(&st->cfg, user, password, &cred)) {
                    rc_logw(st->cfg.name, "live listener: rejected connection (bad credentials, user='%s')", user);
                    send_all(cfd, "invalid password\r\n", 19);
                    close(cfd);
                    continue;
                }
                pthread_mutex_lock(&st->live_mutex);
                if (st->live_connected) {
                    if (cred.priority > st->live_priority) {
                        rc_logw(st->cfg.name, "live listener: kicking user '%s' (prio %d) for higher-priority "
                                               "user '%s' (prio %d)",
                                st->live_user, st->live_priority, cred.user, cred.priority);
                        close(st->live_fd);
                        st->live_fd = cfd;
                        snprintf(st->live_user, sizeof(st->live_user), "%s", cred.user);
                        st->live_priority = cred.priority;
                        atomic_fetch_add(&st->live_generation, 1);
                        pthread_mutex_unlock(&st->live_mutex);
                    } else {
                        rc_logw(st->cfg.name, "live listener: denied user '%s' (prio %d): '%s' (prio %d) "
                                               "already connected",
                                cred.user, cred.priority, st->live_user, st->live_priority);
                        pthread_mutex_unlock(&st->live_mutex);
                        send_all(cfd, "invalid password\r\n", 19);
                        close(cfd);
                        continue;
                    }
                } else {
                    st->live_connected = 1;
                    st->live_fd = cfd;
                    snprintf(st->live_user, sizeof(st->live_user), "%s", cred.user);
                    st->live_priority = cred.priority;
                    atomic_fetch_add(&st->live_generation, 1);
                    pthread_mutex_unlock(&st->live_mutex);
                }

                send_all(cfd, "OK2\r\nicy-caps:11\r\n\r\n", 21);

                /* Drain optional ICY header lines the client sends before raw audio. */
                for (;;) {
                    char hline[512];
                    int hn = read_line(cfd, hline, sizeof(hline));
                    if (hn <= 0) break; /* blank line, disconnect, or error: audio starts (or already ended) */
                    rc_logd(st->cfg.name, "live listener: header from '%s': %s", cred.user, hline);
                }

                rc_logi(st->cfg.name, "live listener: user '%s' connected (priority %d)", cred.user, cred.priority);
                /* The engine thread (run_playlist_stream) picks up st->live_connected
                 * and reads audio from st->live_fd from here on; this thread goes
                 * back to accepting further connection attempts (for kicks). */
            }
        }
    }
    if (listen_fd != -1) close(listen_fd);
    if (listen_fd6 != 1) close (listen_fd6);
    atomic_store(&st->live_thread_running, 0);
    return NULL;
}

/* Updates the stream's "last sent" song title dedup state and, if the title
 * actually changed, forwards it to the configured output server's admin.cgi
 * endpoint. 'song_title_encoded' must already be HTML-encoded (per
 * relcast's metadata conventions - see html_encode()). Safe to call from
 * either the metadata listener thread or the playlist engine thread. */
static void send_song_metadata_update(rc_stream_t *st, const char *song_title_encoded)
{
    pthread_mutex_lock(&st->metadata_mutex);
    int changed = strcmp(st->last_sent_song_title, song_title_encoded) != 0;
    if (changed) snprintf(st->last_sent_song_title, sizeof(st->last_sent_song_title), "%s", song_title_encoded);
    pthread_mutex_unlock(&st->metadata_mutex);

    if (!changed) return;

    if (shoutcast_forward_metadata(&st->cfg, song_title_encoded) != 0) {
        rc_logw(st->cfg.name, "metadata: failed to forward song update to output server");
    } else {
        rc_logi(st->cfg.name, "metadata: forwarded song update to output server: %s", song_title_encoded);
    }
}

/* Very small helper: extracts the value of query-string parameter 'key'
 * from an HTTP request-line's path+query part (e.g.
 * "/admin.cgi?pass=foo&mode=updinfo&song=bar"). Writes the raw
 * (still percent-encoded) value into 'out'. Returns 1 if found, 0
 * otherwise. */
static int qs_get(const char *query, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq || (amp && eq > amp)) { /* malformed pair, skip to next */
            if (!amp) break;
            p = amp + 1;
            continue;
        }
        size_t namelen = (size_t)(eq - p);
        const char *valstart = eq + 1;
        size_t vallen = amp ? (size_t)(amp - valstart) : strlen(valstart);
        if (namelen == klen && strncmp(p, key, klen) == 0) {
            if (vallen >= outlen) vallen = outlen - 1;
            memcpy(out, valstart, vallen);
            out[vallen] = '\0';
            return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

/* DJ metadata listener: binds (live_listen_port - 1) and accepts plain HTTP
 * GET requests of the form
 *   GET /admin.cgi?pass=<password>&mode=updinfo&song=<encoded> HTTP/1.x
 * validates 'pass' against the stream's configured live_credentials
 * password(s), decodes 'song', stores it, and forwards it (HTML-encoded, in
 * an equivalent outgoing admin.cgi request) to the stream's own output
 * server admin port (output_port - 1). This mirrors real Shoutcast source
 * client behaviour, where the DJ encoder pushes now-playing metadata out
 * of band from the audio stream itself. */
static void *metadata_listener_main(void *arg)
{
    rc_stream_t *st = (rc_stream_t *)arg;
    atomic_store(&st->metadata_thread_running, 1);
    int listen_fd  = -1;
    int listen6_fd = -1;
    char pass[256];
    char song[512];
    if (st->cfg.live_ip_addr_4[0] != '\0' && st->cfg.live_listen_port_4 != 0) {
        int admin_port = st->cfg.live_listen_port_4 - 1;

        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        if (listen_fd < 0) {
            rc_loge(st->cfg.name, "metadata listener: socket() failed: %s", strerror(errno));
            //atomic_store(&st->metadata_thread_running, 0);
            //return NULL;
        }
        if (listen_fd >= 0) {
            int one = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            if (inet_pton(AF_INET, st->cfg.live_ip_addr_4, (void*)&(addr.sin_addr)) <= 0) {
                rc_loge(st->cfg.name, "invalid IPv4 listen address %s", st->cfg.live_ip_addr_4);
                //atomic_store(&st->live_thread_running, 0);
                close(listen_fd);
                listen_fd = -1;
            }
        }
        if (listen_fd >= 0) {
            addr.sin_port = htons((uint16_t)admin_port);
            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                rc_loge(st->cfg.name, "metadata listener: IPv4 bind() to port %d failed: %s",
                        admin_port, strerror(errno));
                close(listen_fd);
                listen_fd = -1;
            }
        }
        if (listen_fd >= 0) {
            if (listen(listen_fd, 4) != 0) {
                rc_loge(st->cfg.name, "metadata listener: IPv4 listen() failed: %s", strerror(errno));
                close(listen_fd);
                listen_fd = -1;
            }
        }
        if (listen_fd >= 0) {
            rc_logi(st->cfg.name, "DJ metadata listener ready on port %d (admin.cgi)", admin_port);
        }
    }
    if (st->cfg.live_ip_addr_6[0] != '\0' && st->cfg.live_listen_port_6 != 0) {
        int admin_port = st->cfg.live_listen_port_6 - 1;

        listen6_fd = socket(AF_INET6, SOCK_STREAM, 0);
        struct sockaddr_in6 addr;
        if (listen6_fd < 0) {
            rc_loge(st->cfg.name, "metadata listener: socket() failed: %s", strerror(errno));
        }
        if (listen6_fd >= 0) {
            int one = 1;
            setsockopt(listen6_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            int v6_only = 1;
            setsockopt(listen6_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            if (inet_pton(AF_INET6, st->cfg.live_ip_addr_6, (void*)&(addr.sin6_addr)) <= 0) {
                rc_loge(st->cfg.name, "invalid IPv6 listen address %s", st->cfg.live_ip_addr_4);
                close(listen6_fd);
                listen6_fd = -1;
            } 
        }
        if (listen6_fd >= 0) {
             int one = 1;
            setsockopt(listen6_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            int v6_only = 1;
            setsockopt(listen6_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            if (inet_pton(AF_INET6, st->cfg.live_ip_addr_6, (void*)&(addr.sin6_addr)) <= 0) {
                rc_loge(st->cfg.name, "invalid IPv6 listen address %s", st->cfg.live_ip_addr_4);
                close(listen6_fd);
                listen6_fd = -1;
            }
        }
        if (listen6_fd >= 0) {
            addr.sin6_port = htons((uint16_t)admin_port);
            if (bind(listen6_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                rc_loge(st->cfg.name, "metadata listener: IPv6 bind() to port %d failed: %s",
                        admin_port, strerror(errno));
                close(listen6_fd);
                listen6_fd = -1;
            }
        }
        if (listen6_fd >= 0) {
            if (listen(listen6_fd, 4) != 0) {
                rc_loge(st->cfg.name, "metadata listener: IPv6 listen() failed: %s", strerror(errno));
                close(listen6_fd);
                listen6_fd = -1;
            }
        }
        if (listen6_fd >= 0) {
             rc_logi(st->cfg.name, "DJ IPv6 metadata listener ready on port %d (admin.cgi)", admin_port);
        } else {
             rc_loge(st->cfg.name, "NO live IPv6 metadata listener could be started (SHOUTcast v1)");
        }
    }
    if (listen_fd < 0 && listen6_fd < 0)
    {
         atomic_store(&st->metadata_thread_running, 0);
         rc_loge(st->cfg.name, "Live metadata listener thread exiting (SHOUTcast v1)");
         return NULL;
    }    
    struct pollfd pfd[2];
    pfd[0].fd = listen_fd;
    pfd[0].events = POLLIN;
    pfd[1].fd = listen6_fd;
    pfd[1].events = POLLIN;
  
    while (!atomic_load(&st->stop_request)) {
        int pr = poll(pfd, 2, 200);
        if (pr <= 0) continue;
        int i;
        for (i = 0; i < 2 ;i++) {
            if (pfd[i].revents & POLLIN) {   
                int cfd = accept(pfd[i].fd, NULL, NULL);        
                if (cfd < 0) {
                    rc_loge (st->cfg.name,"Could not accept metadata connection" );    
                    continue;
                } else {
                    rc_logi (st->cfg.name,"Incoming metadata");
                }
                char line[MAX_HTTP_GET_LEN + 1];
                int n = read_line(cfd, line, sizeof(line));
                if (n < 0) { close(cfd); continue; }

                /* Expect: GET /admin.cgi?...  HTTP/1.x */
                char method[MAX_HTTP_METHOD_LEN + 1] = {0}, path[MAX_HTTP_GET_LEN + 1] = {0};
                if (sscanf(line, "%" STR(MAX_HTTP_METHOD_LEN) "s %" STR(MAX_HTTP_GET_LEN) "s", method, path) != 2 || strcmp(method, "GET") != 0) {
                    send_all(cfd, "HTTP/1.0 400 Bad Request\r\n\r\n", 29);
                    close(cfd);
                    continue;
                    }
                /* Drain remaining request headers (until blank line); we don't need
                 * them but must consume them to be a well-behaved HTTP peer. */
                for (;;) {
                    char hline[512];
                    int hn = read_line(cfd, hline, sizeof(hline));
                    if (hn <= 0) break;
                }

                char *query = strchr(path, '?');
                query = query ? query + 1 : "";

                char pass_raw[256] = {0}, song_raw[512] = {0};
                qs_get(query, "pass", pass_raw, sizeof(pass_raw));
                qs_get(query, "song", song_raw, sizeof(song_raw));

                percent_decode(pass_raw, pass, sizeof(pass));
                percent_decode(song_raw, song, sizeof(song));

                int authed = 0;
                for (int i = 0; i < st->cfg.live_credential_count; i++) {
                       int streampasswdlen = strlen(st->cfg.live_credentials[i].user) + strlen(st->cfg.live_credentials[i].password) + 1; //":"
                       char *streampasswd = malloc(streampasswdlen + 1); //null terminated
                       if (!streampasswd) { rc_loge(st->cfg.name, "metadata listener: out of memory"); break;}
                       snprintf(streampasswd,streampasswdlen + 1,"%s:%s",st->cfg.live_credentials[i].user, st->cfg.live_credentials[i].password);
                       if (strcmp(streampasswd, pass) == 0) { authed = 1; }
                       free(streampasswd);
                       if (authed) { break;}
                    }

                if (!authed) {
                    rc_logw(st->cfg.name, "metadata listener: rejected admin.cgi request (bad pass)");
                    send_all(cfd, "HTTP/1.0 401 Unauthorized\r\n\r\n", 30);
                    close(cfd);
                    continue;
                }

                send_all(cfd, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<html><body>OK</body></html>", 76);
                close(cfd);
                rc_logi(st->cfg.name, "metadata listener: received song update: %s", song);
                            //skip html encode for now
                //char song_html[256];
                //html_encode(song, song_html, sizeof(song_html));
                
                // Is this really needed?
                //pthread_mutex_lock(&st->metadata_mutex);
                //snprintf(st->live_song_title, sizeof(st->live_song_title), "%s", song);
                //pthread_mutex_unlock(&st->metadata_mutex);

                send_song_metadata_update(st, song);
            }
        }
    }
    if (listen_fd  > 0) close(listen_fd );
    if (listen6_fd > 0) close(listen6_fd);
    atomic_store(&st->metadata_thread_running, 0);
    return NULL;
}

/* ===========================================================================
 * Playlist-mode engine: crossfade mixing + playlist/live arbitration
 * ===========================================================================
 */

#define MIX_CHUNK_SAMPLES 4096

/* Equal-power-ish (actually linear, kept simple) fade gains for a position
 * `pos` out of `total` samples into a transition. */
static void fade_gains(int64_t pos, int64_t total, float *out_gain, float *in_gain)
{
    if (total <= 0) { *out_gain = 0.0f; *in_gain = 1.0f; return; }
    float frac = (float)pos / (float)total;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    *out_gain = 1.0f - frac;
    *in_gain = frac;
}

static void run_playlist_stream(rc_stream_t *st)
{
    if (st->cfg.codec == RC_CODEC_COPY) {
        rc_loge(st->cfg.name, "codec 'copy' is not supported with input_type=playlist "
                               "(crossfading requires decoding); stream will not start");
        return;
    }

    int target_rate = st->cfg.sample_rate > 0 ? st->cfg.sample_rate : 44100;
    int target_ch = st->cfg.channels > 0 ? st->cfg.channels : 2;
    int crossfade_ms = st->cfg.crossfade_ms > 0 ? st->cfg.crossfade_ms : RELCAST_DEFAULT_CROSSFADE_MS;
    int64_t crossfade_samples = (int64_t)crossfade_ms * target_rate / 1000;
    int this_will_fade = 1;
    int next_will_fade = 1;
    int64_t remaining;
    if (playlist_load(st) != 0) {
        rc_logw(st->cfg.name, "initial playlist load found no entries; will keep retrying");
    }

    if (st->cfg.live_enable) {
        pthread_create(&st->live_thread, NULL, live_listener_main, st);
        pthread_create(&st->metadata_thread, NULL, metadata_listener_main, st);
    }

    rc_out_t *out = NULL;
    int out_delay_ms = st->cfg.reconnect_delay_ms > 0 ? st->cfg.reconnect_delay_ms : RELCAST_DEFAULT_RECONNECT_DELAY_MS;
    int out_max_delay_ms = st->cfg.max_reconnect_delay_ms > 0 ? st->cfg.max_reconnect_delay_ms : RELCAST_MAX_RECONNECT_DELAY_MS;

    rc_track_src_t *cur = NULL; //current song
    rc_track_src_t *nxt = NULL; //next song
    rc_track_src_t *paused_playlist = NULL; /* stashed playlist track while live is active */
    rc_track_src_t *trackA = NULL; //crossfader sources
    rc_track_src_t *trackB = NULL;
    
    int in_transition = 0;
    int64_t transition_pos = 0;
    int cur_generation = 0; /* live_generation captured when cur became the live track */

    /* Real-time pacing: unlike URL-mode (naturally throttled by the network
     * source), local playlist files decode as fast as the CPU allows, so we
     * must self-throttle output to real playback speed here. A live DJ
     * socket already paces itself via blocking reads, but pacing against it
     * too is harmless (the sleep amount will simply be ~0), and lets a
     * single mechanism cover both cases including crossfades between them. */
    struct timespec pace_ref;
    clock_gettime(CLOCK_MONOTONIC, &pace_ref);
    int64_t pace_samples = 0;
    static double min_time = 1;

    float *bufA[8] = {0}, *bufB[8] = {0}, *bufMix[8] = {0};
    for (int c = 0; c < target_ch; c++) {
        bufA[c] = av_malloc(MIX_CHUNK_SAMPLES * sizeof(float));
        bufB[c] = av_malloc(MIX_CHUNK_SAMPLES * sizeof(float));
        bufMix[c] = av_malloc(MIX_CHUNK_SAMPLES * sizeof(float));
    }
    //send 1 second of data first, then do it in chunks of 0.1
    while (!atomic_load(&st->stop_request)) {
        if (!out) {
            out = out_open(st, target_rate, target_ch);
            if (!out) {
                pthread_mutex_lock(&st->err_mutex);
                rc_loge(st->cfg.name, "output open failed: %s", st->last_error);
                pthread_mutex_unlock(&st->err_mutex);
                atomic_fetch_add(&st->reconnect_count, 1);
                int slept = 0;
                while (slept < out_delay_ms && !atomic_load(&st->stop_request)) { usleep(50*1000); slept += 50; }
                out_delay_ms *= 2;
                if (out_delay_ms > out_max_delay_ms) out_delay_ms = out_max_delay_ms;
                continue;
            }
            out_delay_ms = st->cfg.reconnect_delay_ms > 0 ? st->cfg.reconnect_delay_ms : RELCAST_DEFAULT_RECONNECT_DELAY_MS;
        }

        if (!cur) {
            maybe_apply_playlist_reload(st);
            cur = acquire_next_playlist_track(st, target_rate, target_ch);
            if (!cur) break; /* shutdown requested */
            rc_logi(st->cfg.name, "now playing: %s", cur->label);
            if (!cur->is_live) send_song_metadata_update(st, cur->song_title);
            if (cur->ifmt_ctx->duration != AV_NOPTS_VALUE) {
              // Durata este stocată în microsecunde. O convertim în secunde.
              int64_t duration_micros = cur->ifmt_ctx->duration;
              double duration_seconds = (double)duration_micros / AV_TIME_BASE;
              rc_logi (st->cfg.name, "song duration %f", duration_seconds);
              if (duration_seconds < (double)MIN_DURATION_TO_CROSSFADE) {
                  this_will_fade = 0;
              } else {
                  this_will_fade = 1;
                  crossfade_samples = (int64_t)crossfade_ms * target_rate / 1000;
              }
            }
        }

        /* --- Live takeover / handback arbitration --- */
        pthread_mutex_lock(&st->live_mutex);
        int live_now = st->live_connected;
        int live_fd_now = st->live_fd;
        char live_user_now[LIVE_USER_NAME_LEN]; snprintf(live_user_now, LIVE_USER_NAME_LEN , "%s", st->live_user);
        int live_gen_now = atomic_load(&st->live_generation);
        pthread_mutex_unlock(&st->live_mutex);

        /* Detect the DJ's audio socket reaching EOF (clean disconnect or
         * dropped connection) as early as possible, rather than waiting for
         * cur's FIFO to fully drain (which would otherwise force a hard cut
         * in the "track ended" fallback below instead of a crossfade). Flip
         * st->live_connected here so the normal "DJ disconnected" branch
         * just below picks it up and crossfades back into the playlist
         * while whatever audio is still buffered in cur's FIFO fades out. */
        if (live_now && cur->is_live && cur->input_eof && cur->fd == live_fd_now) {
            pthread_mutex_lock(&st->live_mutex);
            if (st->live_connected && st->live_fd == cur->fd) {
                st->live_connected = 0;
                rc_loge(st->cfg.name, "Got EOF on live track!");
            }
            pthread_mutex_unlock(&st->live_mutex);
            live_now = 0;
        }

        if (live_now && !cur->is_live && !in_transition) {
            /* DJ connected: crossfade from playlist track into live audio. */
            rc_track_src_t *live_t = track_open_live(st, live_fd_now, live_user_now, target_rate, target_ch);
            if (live_t) {
                rc_logi(st->cfg.name, "live source connected ('%s'), fading in", live_user_now);
                paused_playlist = cur; /* keep old playlist track open, paused */
                nxt = live_t;
                next_will_fade = 1;
                cur_generation = live_gen_now;
                in_transition = 1; transition_pos = 0; trackA = cur; trackB = nxt;
            } else {
               live_now = 0;
               rc_logi(st->cfg.name, "live source tried to connect ('%s') but failed during the process", live_user_now);
               pthread_mutex_lock(&st->live_mutex);
               st->live_connected = 0;
               pthread_mutex_unlock(&st->live_mutex);
            }
        } else if (!live_now && cur->is_live && !in_transition) {
            /* DJ disconnected: crossfade from live back to playlist. */
            rc_logi(st->cfg.name, "live source disconnected, resuming playlist");
            
            if (paused_playlist) {
                nxt = paused_playlist;
                paused_playlist = NULL;
            } else {
                maybe_apply_playlist_reload(st);
                nxt = acquire_next_playlist_track(st, target_rate, target_ch);
                if (!nxt) break;
            }
            in_transition = 1; transition_pos = 0; trackA = cur; trackB = nxt;
        } else if (live_now && cur->is_live && cur_generation != live_gen_now && !in_transition) {
            /* Kicked and replaced by a different live user: hard-swap the fd
             * onto the same logical "live" slot without a crossfade (the old
             * socket is already closed by the listener thread). */
            rc_track_src_t *live_t = track_open_live(st, live_fd_now, live_user_now, target_rate, target_ch);
            if (live_t) {
                rc_logi(st->cfg.name, "live source replaced by higher-priority user '%s'", live_user_now);
                track_close(cur);
                cur = live_t;
                cur_generation = live_gen_now;
            }
        }

        /* --- Skip request (SIGWINCH): force a transition to start now --- */
        int skip_expected = 1;
        if (!in_transition && !cur->is_live &&
            atomic_compare_exchange_strong(&st->playlist_skip_request, &skip_expected, 0)) {
            rc_logi(st->cfg.name, "skip requested, advancing playlist now");
            maybe_apply_playlist_reload(st);
            rc_track_src_t *t = acquire_next_playlist_track(st, target_rate, target_ch);
            if (!t) break; 
            crossfade_samples = (int64_t)crossfade_ms * target_rate / 1000;
            next_will_fade = 0;
            if (t->ifmt_ctx->duration != AV_NOPTS_VALUE) {
              // Durata este stocată în microsecunde. O convertim în secunde.
              int64_t duration_micros = t->ifmt_ctx->duration;
              double duration_seconds = (double)duration_micros / AV_TIME_BASE;
              rc_logd (st->cfg.name, "song duration %f", duration_seconds);
              if (duration_seconds >= (double)MIN_DURATION_TO_CROSSFADE)
              {
                   next_will_fade = 1;
              }  
            }
            nxt = t;
            rc_logi(st->cfg.name, "crossfading into: %s", nxt->label);
            in_transition = 1; transition_pos = 0; trackA = cur; trackB = nxt;
            
            if (this_will_fade == 1 && next_will_fade == 0) {
                  crossfade_samples = target_rate; // 1 s;
            }
            
        }
        /* --- Natural lookahead crossfade: start fading into the next track
         * before the current one physically ends, when we know its total
         * duration. --- */
        if (!in_transition && !cur->is_live && this_will_fade == 1 && cur->samples_total > 0) {
            remaining = cur->samples_total - cur->samples_consumed;
            if (remaining <= crossfade_samples) {
                  maybe_apply_playlist_reload(st);
                  nxt = acquire_next_playlist_track(st, target_rate, target_ch);
                  if (nxt) {
                     crossfade_samples = (int64_t)crossfade_ms * target_rate / 1000;
                     next_will_fade = 0;
                     if (nxt->ifmt_ctx->duration != AV_NOPTS_VALUE) {
                        // Durata este stocată în microsecunde. O convertim în secunde.
                        int64_t duration_micros = nxt->ifmt_ctx->duration;
                        double duration_seconds = (double)duration_micros / AV_TIME_BASE;
                        rc_logd (st->cfg.name, "song duration %f", duration_seconds);
                        if (duration_seconds > (double)MIN_DURATION_TO_CROSSFADE) { 
                           next_will_fade = 1;
                        }
                     }
                     if ( next_will_fade == 0) {
                          this_will_fade = 0;
                     } 
                     if (this_will_fade == 1) {
                        rc_logi(st->cfg.name, "crossfading into: %s", nxt->label);
                        in_transition = 1; transition_pos = 0; trackA = cur, trackB = nxt;
                        
                     }
                  } else {
                    break;
                  }
            }
        }
        int n_out = 0;
        int write_err = 0;

        if (in_transition) {
            //rc_logi("DEBUG", "Transition: current will fade = %d next will fade = %d, cross samples = %ld", this_will_fade, next_will_fade, crossfade_samples);
            int n, n_old, n_new;
            n_old = trackA ? track_get_samples(trackA, bufA, MIX_CHUNK_SAMPLES) : 0;
            if (n_old == 0 && trackA != NULL) { track_close(trackA); trackA = NULL; }
            n_new = trackB ? track_get_samples(trackB, bufB, MIX_CHUNK_SAMPLES) : 0;
            if (n_new == 0 && trackB != NULL) { track_close(trackB); trackB = NULL; }
            n = n_old > n_new ? n_old : n_new;
            if (n > 0) {
               float g_out, g_in;
               for (int c = 0; c < target_ch; c++) {
                  for (int i = 0; i < n; i++) {
                     float sold = (i < n_old) ? bufA[c][i] : 0.0f;
                           float snew = (i < n_new) ? bufB[c][i] : 0.0f;
                           fade_gains(transition_pos + i, crossfade_samples, &g_out, &g_in);
                           bufMix[c][i] = sold * (this_will_fade ? g_out : 1) + snew * (next_will_fade ? g_in : 1);
                       }
                   }
                   if (out_push_fltp(st, out, bufMix, n) != 0) write_err = 1;
                   n_out = n;
                   transition_pos += n;
               }
               if (transition_pos >= crossfade_samples || (n_old == 0 && n_new == 0)) {
                   /* Transition complete (or old source ran out early). */
                   if (trackA && trackA != paused_playlist) track_close(trackA);
                   cur = trackB; //can be NULL, next track will be selected
                   nxt = NULL;
                   in_transition = 0;
                   transition_pos = 0;
                   if (cur) {
                       rc_logi(st->cfg.name, "now playing: %s", cur->label);
                       if (cur->is_live) {
                           cur_generation = live_gen_now;
                       } else {
                           send_song_metadata_update(st, cur->song_title);
                           this_will_fade = next_will_fade;

                       }
                       
                   }
                   
               }
        } else {
            int n = track_get_samples(cur, bufA, MIX_CHUNK_SAMPLES);
            if (n > 0) {
                if (out_push_fltp(st, out, bufA, n) != 0) write_err = 1;
                n_out = n;
            } else {
                /* Track ended without a prior lookahead crossfade (e.g.
                 * unknown duration, like a live source that just dropped
                 * without being caught above, or a very short file). Cut
                 * straight to the next one with a short fade-in. */
                rc_logi(st->cfg.name, "track ended: %s", cur->label);
                if (cur->is_live) {
                    /* Will be handled by the live-disconnect branch above on
                     * the next loop iteration once st->live_connected drops;
                     * but if we got here the socket already EOFed, so force
                     * the handback now. */
                    pthread_mutex_lock(&st->live_mutex);
                    if (st->live_connected && st->live_fd == cur->fd)
                        st->live_connected = 0;
                    pthread_mutex_unlock(&st->live_mutex);
                } else {
                   track_close(cur);
                   cur = nxt; //can be NULL, just make sure it is either NULL either a valid track
                   nxt = NULL;
                }
            }
        }
        if (write_err) {
            pthread_mutex_lock(&st->err_mutex);
            rc_loge(st->cfg.name, "output write error: %s", st->last_error);
            pthread_mutex_unlock(&st->err_mutex);
            out_close(out);
            out = NULL;
            atomic_fetch_add(&st->reconnect_count, 1);

            /* Hold off before attempting to reconnect the output. Without
             * this delay, a server that drops the connection (e.g. a
             * SHOUTcast server closing the socket a few seconds in) would
             * cause an immediate reconnect attempt on the very next loop
             * iteration, which some servers reject/hang up on again right
             * away, leading to a tight reconnect loop. Use the same
             * exponential backoff as output-open failures, with a floor of
             * 1 second. */
            int wait_ms = out_delay_ms > 1000 ? out_delay_ms : 1000;
            int slept = 0;
            while (slept < wait_ms && !atomic_load(&st->stop_request)) {
                usleep(50 * 1000);
                slept += 50;
            }
            out_delay_ms *= 2;
            if (out_delay_ms > out_max_delay_ms) out_delay_ms = out_max_delay_ms;
        }
        if (n_out == 0 && !in_transition) {
            /* Avoid a tight spin if both tracks momentarily produced nothing
             * (e.g. waiting on live socket data). */
            usleep(10 * 1000);
            clock_gettime(CLOCK_MONOTONIC, &pace_ref);
            pace_samples = 0;
        } else if (n_out > 0) {
            /* Throttle to real playback speed: sleep off however much wall
             * clock time we're currently ahead of, based on samples emitted
             * since pace_ref. This keeps local-file playlist playback (which
             * would otherwise decode/output as fast as the CPU allows) in
             * sync with real time; a live DJ socket paces itself naturally
             * via blocking reads, so this is mostly a no-op in that case. */
            pace_samples += n_out;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - pace_ref.tv_sec) +
                              (now.tv_nsec - pace_ref.tv_nsec) / 1e9;
            double target = (double)pace_samples / (double)target_rate;
            double behind = target - elapsed;
            //do not fragment too much - allow buffering, for instance, 2s, but do not try to sleep too less
            //otherwise we'll go too fast
            if (behind > min_time) {
                if (behind > 60) behind = 60; /* clamp: avoid huge catch-up sleeps after stalls */
                //rc_logi("Debug", "Sleeping %f sec", behind);
                usleep((useconds_t)(behind * 1e6));
                //rc_logi("Debug", "Wakeing up");
                if (min_time == 1) min_time = 0.1;
            } else if (behind < -0.5) {
                /* We've fallen far behind real time (e.g. after a slow
                 * output write); resync the reference so we don't try to
                 * sprint to catch up. */
                clock_gettime(CLOCK_MONOTONIC, &pace_ref);
                pace_samples = 0;
            }
        }
    }

    if (cur && cur != paused_playlist) track_close(cur);
    if (nxt) track_close(nxt);
    if (paused_playlist) track_close(paused_playlist);

    if (out) {
        out_flush_and_finish(out, st);
        out_close(out);
    }

    for (int c = 0; c < target_ch; c++) {
        av_free(bufA[c]); av_free(bufB[c]); av_free(bufMix[c]);
    }

    if (st->cfg.live_enable) {
        pthread_join(st->live_thread, NULL);
        pthread_join(st->metadata_thread, NULL);
    }
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

static void *stream_thread_main(void *arg)
{
    rc_stream_t *st = (rc_stream_t *)arg;
    atomic_store(&st->running, 1);
    run_playlist_stream(st);
    atomic_store(&st->running, 0);
    return NULL;
}

int rc_stream_init(rc_stream_t *st, const rc_stream_config_t *cfg)
{
    memset(st, 0, sizeof(*st));
    st->cfg = *cfg;
    atomic_init(&st->running, 0);
    atomic_init(&st->stop_request, 0);
    atomic_init(&st->bytes_in, 0);
    atomic_init(&st->bytes_out, 0);
    atomic_init(&st->reconnect_count, 0);
    pthread_mutex_init(&st->err_mutex, NULL);
    st->last_error[0] = '\0';

    pthread_mutex_init(&st->playlist_mutex, NULL);
    st->playlist_files = NULL;
    st->playlist_count = 0;
    st->playlist_index = -1;
    atomic_init(&st->playlist_skip_request, 0);
    atomic_init(&st->playlist_reload_request, 0);

    atomic_init(&st->live_thread_running, 0);
    pthread_mutex_init(&st->live_mutex, NULL);
    st->live_connected = 0;
    st->live_fd = -1;
    st->live_priority = 0;
    st->live_user[0] = '\0';
    atomic_init(&st->live_generation, 0);

    atomic_init(&st->metadata_thread_running, 0);
    pthread_mutex_init(&st->metadata_mutex, NULL);
    //is this really needed?
    //st->live_song_title[0] = '\0';
    st->last_sent_song_title[0] = '\0';

    return 0;
}

int rc_stream_start(rc_stream_t *st)
{
    atomic_store(&st->stop_request, 0);
    int ret = pthread_create(&st->thread, NULL, stream_thread_main, st);
    if (ret != 0) {
        rc_loge(st->cfg.name, "pthread_create failed: %s", strerror(ret));
        return -1;
    }
    return 0;
}

void rc_stream_stop(rc_stream_t *st)
{
    atomic_store(&st->stop_request, 1);
    if (atomic_load(&st->running) || st->thread) {
        pthread_join(st->thread, NULL);
    }
}

void rc_stream_destroy(rc_stream_t *st)
{
    playlist_free_list(st);
    pthread_mutex_destroy(&st->err_mutex);
    pthread_mutex_destroy(&st->playlist_mutex);
    pthread_mutex_destroy(&st->live_mutex);
    pthread_mutex_destroy(&st->metadata_mutex);
}

void rc_stream_playlist_skip(rc_stream_t *st)
{
        atomic_store(&st->playlist_skip_request, 1);
}

void rc_stream_playlist_reload(rc_stream_t *st)
{
        atomic_store(&st->playlist_reload_request, 1);
}
