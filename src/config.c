/*
 * config.c - CLI and config-file parsing for relcast
 *
 * All stream (source/destination) definitions live exclusively in the
 * config file (-c/--config). The command line only controls process-level
 * concerns: which config file to load, and logging verbosity. This keeps
 * the set of running streams fully re-derivable from a single file, which
 * is what makes SIGHUP live-reload possible/sane.
 */
#include "relcast.h"
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <getopt.h>
#include <ctype.h>
#include <arpa/inet.h> 
#include <sys/socket.h>
#include <netinet/in.h>

void rc_config_set_defaults(rc_stream_config_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    snprintf(sc->name, sizeof(sc->name), "stream0");
    snprintf(sc->input_user_agent, sizeof(sc->input_user_agent), "relcast/%s", RELCAST_VERSION);
 
    sc->crossfade_ms = RELCAST_DEFAULT_CROSSFADE_MS;
    sc->live_enable = 0;
    sc->live_listen_port_4 = 0;
    sc->live_listen_port_6 = 0;
    sc->live_ip_addr_4[0] = '\0';
    sc->live_ip_addr_6[0] = '\0';
    sc->live_credential_count = 0;
    sc->output_port = 8000;
    snprintf(sc->output_mount, sizeof(sc->output_mount), "/stream");
    snprintf(sc->output_user, sizeof(sc->output_user), "source");
    sc->output_proto = RC_PROTO_ICECAST;
    sc->output_tls = 0;
    sc->codec = RC_CODEC_MP3;
    sc->bitrate = 128000;
    sc->sample_rate = 0;
    sc->channels = 0;
    sc->reconnect = 1;
    sc->reconnect_delay_ms = RELCAST_DEFAULT_RECONNECT_DELAY_MS;
    sc->max_reconnect_delay_ms = RELCAST_MAX_RECONNECT_DELAY_MS;
    sc->ice_public = 0;
    sc->random_order = 0;
    sc->playlist_file[0] = 0;
}

rc_codec_t rc_codec_from_string(const char *s)
{
    if (!s) return RC_CODEC_MP3;
    if (strcasecmp(s, "mp3") == 0) return RC_CODEC_MP3;
    if (strcasecmp(s, "vorbis") == 0 || strcasecmp(s, "ogg") == 0) return RC_CODEC_VORBIS;
    if (strcasecmp(s, "opus") == 0) return RC_CODEC_OPUS;
    if (strcasecmp(s, "aac") == 0 || strcasecmp(s, "aac_adts") == 0) return RC_CODEC_AAC;
    if (strcasecmp(s, "flac") == 0) return RC_CODEC_FLAC;
    if (strcasecmp(s, "copy") == 0) return RC_CODEC_COPY;
    return RC_CODEC_MP3;
}

const char *rc_codec_to_string(rc_codec_t c)
{
    switch (c) {
        case RC_CODEC_MP3: return "mp3";
        case RC_CODEC_VORBIS: return "vorbis";
        case RC_CODEC_OPUS: return "opus";
        case RC_CODEC_AAC: return "aac";
        case RC_CODEC_FLAC: return "flac";
        case RC_CODEC_COPY: return "copy";
        default: return "unknown";
    }
}



int rc_stream_config_equal(const rc_stream_config_t *a, const rc_stream_config_t *b)
{
    /* Straight memcmp is safe here: rc_stream_config_t has no pointer
     * members and is always fully initialized via rc_config_set_defaults
     * (memset to 0 first), so padding bytes are consistently zero for
     * both sides as long as both were built the same way. */
    return memcmp(a, b, sizeof(*a)) == 0;
}

void rc_config_print_usage(const char *prog)
{
    printf(
        "relcast %s - Shoutcast/Icecast stream transcoder\n\n"
        "Usage:\n"
        "  %s -c <config.ini> [-v] [-q]\n\n"
        "All stream (source/destination) definitions are read exclusively\n"
        "from the config file; there is no way to define a stream via the\n"
        "command line. See README.md for the full config file format.\n\n"
        "Options:\n"
        "  -c, --config <file>   Load stream definitions from ini-like file (required)\n"
        "  -v, --verbose          Increase log verbosity (can repeat)\n"
        "  -q, --quiet            Only log errors\n"
        "      --help             Show this help\n\n"
        "Signals:\n"
        "  SIGINT, SIGTERM   Graceful shutdown of all streams\n"
        "  SIGHUP            Reload the config file live: streams removed from\n"
        "                    the file are stopped, new ones are started, and\n"
        "                    changed ones are restarted with their new settings\n"
        "  SIGWINCH          Skip to the next playlist entry, on all streams\n"
        "                    that are currently playing from a playlist\n"
        "  SIGUSR1           Reload the playlist file from disk and restart\n"
        "                    at the first track once the current song ends\n",
        RELCAST_VERSION, prog);
}

static struct option long_opts[] = {
    {"config",            required_argument, 0, 'c'},
    {"verbose",           no_argument,       0, 'v'},
    {"quiet",             no_argument,       0, 'q'},
    {"help",              no_argument,       0, 1000},
    {0,0,0,0}
};

int rc_config_parse_args(int argc, char **argv, rc_app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->log_level = RC_LOG_INFO;

    char config_path[1024] = {0};
    int opt;
    int idx;

    while ((opt = getopt_long(argc, argv, "c:vq", long_opts, &idx)) != -1) {
        switch (opt) {
            case 'c':
                snprintf(config_path, sizeof(config_path), "%s", optarg);
                break;
            case 'v':
                cfg->log_level++;
                break;
            case 'q':
                cfg->log_level = RC_LOG_ERROR;
                break;
            case 1000: /* --help */
                rc_config_print_usage(argv[0]);
                exit(0);
            default:
                rc_config_print_usage(argv[0]);
                return RC_ERROR;
        }
    }

    if (config_path[0] == '\0') {
        fprintf(stderr, "error: -c/--config <file> is required (all streams are defined "
                         "in the config file)\n");
        rc_config_print_usage(argv[0]);
        return RC_ERROR;
    }

    snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", config_path);

    if (rc_config_load_file(config_path, cfg) != 0) {
        fprintf(stderr, "Failed to load config file: %s\n", config_path);
        return RC_ERROR;
    }

    if (cfg->stream_count == 0) {
        fprintf(stderr, "error: no streams configured in '%s'\n", config_path);
        return RC_ERROR;
    }

    return 0;
}

/* --- Simple INI-like config file parser ---
 *
 * [stream]
 * name = radio1
 * output_host = icecast.example.com
 * output_port = 8000
 * output_mount = /live.mp3
 * output_user = source
 * output_password = hackme
 * proto = icecast
 * codec = mp3
 * bitrate = 128000
 * sample_rate = 44100
 * channels = 2
 * ice_name = My Radio
 * ice_description = ...
 * ice_genre = Various
 * ice_url = http://example.com
 * ice_public = 0
 * reconnect = 1
 * reconnect_delay_ms = 2000
 * user_agent = relcast/0.1
 *
 * input_type = url | playlist   (default: playlist)
 *
 *
 * playlist_file = /etc/relcast/radio1.m3u
 * crossfade_ms = 3000
 *#include <arpa/inet.h> 
 * # optional live DJ-override listener 
 * live_enable = 1
 * live_listen_port = 8010
 * live_credential = user:password:priority   (repeatable)
 *
 * Multiple [stream] sections may appear in the same file.
 */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* Parse a "user:password:priority" live_credential value. Returns 0 on
 * success, -1 on malformed input. */
#define MAX_LINE_SIZE 256

static int parse_live_credential(const char *val, rc_live_credential_t *out)
{
    char buf[MAX_LINE_SIZE] = {0};
    char *prioend;
    long priores;
    memcpy(buf, val, sizeof(buf) - 1);
    //parse user
    char *user = buf;
    char *p1 = strchr(buf, ':');
    if (!p1) return RC_ERROR;
    *p1 = '\0';
    //parse pass
    char *pass = p1 + 1;
    char *p2 = strchr(pass, ':');
    if (!p2) return RC_ERROR;
    *p2 = '\0';
    //parse priority
    char *prio = p2 + 1;
    priores = strtol(prio, &prioend, 10);
    if (priores == 0 && prioend == prio) return RC_ERROR;
    if (user[0] == '\0' || pass[0] == '\0') return RC_ERROR;
    snprintf(out->user, sizeof(out->user), "%.63s", user);
    snprintf(out->password, sizeof(out->password), "%.127s", pass);
    out->priority = (int) priores;
    return RC_OK;
}

static int parse_ip4_addr (const char *ip_str, struct in_addr *dst4 ) {

    if (inet_pton(AF_INET, ip_str, dst4) == 1) {
        return RC_OK;
    }
    return RC_ERROR;
}

static int parse_ip6_addr (const char *ip_str, struct in6_addr *dst6 ) {
    if (inet_pton(AF_INET6, ip_str, dst6) == 1) {
        return RC_OK;
    }
    return RC_ERROR;
}
        
static int finalize_stream(rc_app_config_t *cfg, rc_stream_config_t *sc, const char *path)
{
    /* Basic sanity checks for the just-parsed stream. */
    if (sc->output_host[0] == '\0') {
        fprintf(stderr, "config error in '%s': stream '%s' missing output_host\n",
                path, sc->name);
        return RC_ERROR;
    }
   
     if (sc->live_enable && sc->live_ip_addr_4[0] != '\0' && sc->live_listen_port_4 <= 0) {
        fprintf(stderr, "config error in '%s': stream '%s' has live_enable=1, the IPv4 listen addr defined,  but no "
                         "live_listen_port set\n", path, sc->name);
        return RC_ERROR;
    }
    if (sc->live_enable && sc->live_ip_addr_6[0] != '\0' && sc->live_listen_port_6 <= 0) {
        fprintf(stderr, "config error in '%s': stream '%s' has live_enable=1, the IPv6 listen addr defined,  but no "
                         "live_listen_port set\n", path, sc->name);
        return RC_ERROR;
    }
    if (cfg->stream_count >= RELCAST_MAX_STREAMS) {
        fprintf(stderr, "too many streams in config file '%s'\n", path);
        return RC_ERROR;
    }
    cfg->streams[cfg->stream_count++] = *sc;
    
    return 0;
}

int rc_config_load_file(const char *path, rc_app_config_t *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open config file '%s'\n", path);
        return RC_ERROR;
    }

    char line[2048];
    rc_stream_config_t sc;
    int in_stream = 0;
    rc_config_set_defaults(&sc);
    int linecount = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
       linecount++; 
       char *l = trim(line);
        if (*l == '\0' || *l == '#' || *l == ';')
            continue;

        if (*l == '[') {
            /* section header */
            char *he = strchr(l, ']');
            if ( he == NULL) {
               rc_loge("configuration","Missing ']' at line %d", linecount);
               return RC_ERROR; 
            }
            *he = '\0';
            char *hb = trim(l + 1);
            if (strcasecmp(hb, "stream") == 0) {
               if (in_stream) {
                   if (finalize_stream(cfg, &sc, path) != 0) {
                       fclose(f);
                       return RC_ERROR;
                   }
               }
               rc_config_set_defaults(&sc);
               snprintf(sc.name, sizeof(sc.name), "stream%d", cfg->stream_count);
               in_stream = 1;
               continue;
            } else {
               fprintf(stderr, "Unknown configuration header \"%s\"at line %d", hb, linecount);
               return RC_ERROR;
            }
        }
        //parse lines without header markers
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);
        if (in_stream == 1) {
           if (strcasecmp(key, "name") == 0)
               snprintf(sc.name, sizeof(sc.name), "%s", val);
           else if (strcasecmp(key, "input") == 0)
               snprintf(sc.input_url, sizeof(sc.input_url), "%s", val);
           else if (strcasecmp(key, "user_agent") == 0)
               snprintf(sc.input_user_agent, sizeof(sc.input_user_agent), "%s", val);
           else if (strcasecmp(key, "playlist_file") == 0)
               snprintf(sc.playlist_file, sizeof(sc.playlist_file), "%s", val);
           else if (strcasecmp(key, "crossfade_ms") == 0)
               sc.crossfade_ms = atoi(val);
           else if (strcasecmp(key, "live_enable") == 0)
               sc.live_enable = atoi(val);
           else if (strcasecmp(key, "live_ipv4_listen_port") == 0)
               sc.live_listen_port_4 = atoi(val);
           else if (strcasecmp(key, "live_ipv6_listen_port") == 0)
               sc.live_listen_port_6 = atoi(val);    
           else if (strcasecmp(key, "live_credential") == 0) {
               if (sc.live_credential_count >= RELCAST_MAX_LIVE_CREDENTIALS) {
                   fprintf(stderr, "warning: too many live_credential entries for stream '%s', "
                                    "ignoring '%s'\n", sc.name, val);
               } else {
                   rc_live_credential_t cred;
                   if (parse_live_credential(val, &cred) != 0) {
                       fprintf(stderr, "warning: malformed live_credential '%s' for stream '%s' "
                                        "(expected user:password:priority), ignored\n", val, sc.name);
                   } else {
                       sc.live_credentials[sc.live_credential_count++] = cred;
                   }
               }
           }
           else if (strcasecmp(key, "live_ipv4_listen_addr") == 0) {    
                 struct in_addr dst4;
                 if (parse_ip4_addr(val, &dst4)) {
                     fprintf(stderr, "warning: malformed IPv4 address '%s' for stream '%s' ignored\n", val, sc.name);
                 } else {
                     snprintf(sc.live_ip_addr_4, 16, "%s", val);
                 }    
           } else if (strcasecmp(key, "live_ipv6_listen_addr") == 0) {    
                 struct in6_addr dst6;
                 if (parse_ip6_addr(val, &dst6)) {
                     fprintf(stderr, "warning: malformed IPv6  address '%s' for stream '%s' ignored\n", val, sc.name);
                 } else {
                     snprintf(sc.live_ip_addr_6, 46, "%s", val);
                 }
           }    
           else if (strcasecmp(key, "output_host") == 0)
               snprintf(sc.output_host, sizeof(sc.output_host), "%s", val);
           else if (strcasecmp(key, "output_port") == 0)
               sc.output_port = atoi(val);
           else if (strcasecmp(key, "output_mount") == 0)
               snprintf(sc.output_mount, sizeof(sc.output_mount), "%s", val);
           else if (strcasecmp(key, "output_user") == 0)
               snprintf(sc.output_user, sizeof(sc.output_user), "%s", val);
           else if (strcasecmp(key, "output_password") == 0)
               snprintf(sc.output_password, sizeof(sc.output_password), "%s", val);
           else if (strcasecmp(key, "proto") == 0)
               sc.output_proto = (strcasecmp(val, "shoutcast") == 0) ? RC_PROTO_SHOUTCAST : RC_PROTO_ICECAST;
           else if (strcasecmp(key, "tls") == 0)
               sc.output_tls = atoi(val);
           else if (strcasecmp(key, "codec") == 0)
               sc.codec = rc_codec_from_string(val);
           else if (strcasecmp(key, "bitrate") == 0)
               sc.bitrate = atoi(val);
           else if (strcasecmp(key, "sample_rate") == 0)
               sc.sample_rate = atoi(val);
           else if (strcasecmp(key, "channels") == 0)
               sc.channels = atoi(val);
           else if (strcasecmp(key, "ice_name") == 0)
               snprintf(sc.ice_name, sizeof(sc.ice_name), "%s", val);
           else if (strcasecmp(key, "ice_description") == 0)
               snprintf(sc.ice_description, sizeof(sc.ice_description), "%s", val);
           else if (strcasecmp(key, "ice_genre") == 0)
               snprintf(sc.ice_genre, sizeof(sc.ice_genre), "%s", val);
           else if (strcasecmp(key, "ice_url") == 0)
               snprintf(sc.ice_url, sizeof(sc.ice_url), "%s", val);
           else if (strcasecmp(key, "ice_public") == 0)
               sc.ice_public = atoi(val);
           else if (strcasecmp(key, "reconnect") == 0)
               sc.reconnect = atoi(val);
           else if (strcasecmp(key, "reconnect_delay_ms") == 0)
               sc.reconnect_delay_ms = atoi(val);
           else if (strcasecmp(key, "max_reconnect_delay_ms") == 0)
               sc.max_reconnect_delay_ms = atoi(val);
           else if (strcasecmp(key, "random_order") == 0)
               sc.random_order = atoi(val);
           else
               fprintf(stderr, "warning: unknown config key '%s' ignored\n", key);
        }
    }
    if (in_stream) {
        if (finalize_stream(cfg, &sc, path) != 0) {
            fclose(f);
            return RC_ERROR;
        }
    }
    printf ("finished parsing the configuration successfully\n");
    fclose(f);
    return RC_OK;
}
