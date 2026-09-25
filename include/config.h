/*
 * config.h - CLI and config-file parsing for relcast
 */
#ifndef RC_CONFIG_H
#define RC_CONFIG_H

#include "relcast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse command line arguments into app config.
 *
 * All stream (source/destination) definitions must come from the config
 * file; -c/--config <file> is mandatory. The only other recognized flags
 * are -v/--verbose, -q/--quiet and --help. This keeps process-level
 * concerns (verbosity) separate from stream definitions, which can be
 * live-reloaded via SIGHUP (see rc_config_load_file).
 *
 * Returns 0 on success, non-zero on error (usage already printed). */
int rc_config_parse_args(int argc, char **argv, rc_app_config_t *cfg);

/* Load stream definitions from an ini-like config file, appending them to cfg. */
int rc_config_load_file(const char *path, rc_app_config_t *cfg);

/* Fill a stream config with sane defaults. */
void rc_config_set_defaults(rc_stream_config_t *sc);

/* Print usage information to stdout. */
void rc_config_print_usage(const char *prog);

/* Parse a codec name string ("mp3","vorbis","opus","aac","flac","copy") */
rc_codec_t rc_codec_from_string(const char *s);
const char *rc_codec_to_string(rc_codec_t c);

/* Compare two stream configs for equality (used by SIGHUP reload to decide
 * whether a stream needs to be restarted). Returns 1 if equal, 0 if not. */
int rc_stream_config_equal(const rc_stream_config_t *a, const rc_stream_config_t *b);
#define MIN_DURATION_TO_CROSSFADE 10
#ifdef __cplusplus
}
#endif

#endif /* RC_CONFIG_H */
