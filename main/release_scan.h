/**
 * @file release_scan.h
 * @brief Streaming scanner for GitHub release JSON (host-testable)
 *
 * The OTA check used to buffer GitHub's whole release response (9-18 KB) and
 * then build a cJSON tree several times that size, next to the ~35 KB TLS
 * session. On a busy unit (BACnet + MQTT + many sensors) that no longer fit,
 * so update checks failed. This scanner reads the response as it arrives and
 * keeps only what the updater needs, in a fixed ~1.2 KB struct.
 *
 * Accepts either a single release object (/releases/latest) or an array of
 * releases, newest first (/releases?per_page=N). Picks the first release that
 * is not a draft, and from its assets the app binary: an exact match on the
 * given asset name, else the first ".bin" that is not bootloader.bin or
 * partition-table.bin.
 */

#ifndef RELEASE_SCAN_H
#define RELEASE_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELEASE_SCAN_TAG_MAX        31
#define RELEASE_SCAN_ASSET_NAME_MAX 63
#define RELEASE_SCAN_URL_MAX        511
#define RELEASE_SCAN_MAX_DEPTH      63

typedef struct {
    /* Result: valid once release_scan_finish() returns true. */
    char tag[RELEASE_SCAN_TAG_MAX + 1];
    char url[RELEASE_SCAN_URL_MAX + 1];  /* empty if the release has no app .bin */
    bool url_is_exact;                   /* url is the exact app asset, not a fallback */
    bool prerelease;

    /* Internal state */
    char app_asset_name[RELEASE_SCAN_ASSET_NAME_MAX + 1];
    bool error;
    bool done;               /* the first non-draft release has been closed */
    uint8_t release_depth;   /* 0 until the first byte; 1 = object root, 2 = array root */
    uint8_t depth;           /* number of open containers */
    uint64_t is_array;       /* bit d set: the container at depth d is an array */
    uint8_t key_id[5];       /* last key seen in the object at depth 0..4 */
    bool expect_key;

    uint8_t lex;
    uint8_t str_target;
    bool str_is_key;
    char keybuf[24];
    uint8_t keylen;
    bool key_overflow;
    char lit[8];
    uint8_t litlen;
    uint8_t u_count;
    uint16_t u_val;

    bool tag_ok;
    bool draft;
    char asset_name[RELEASE_SCAN_ASSET_NAME_MAX + 1];
    char asset_url[RELEASE_SCAN_URL_MAX + 1];
    uint16_t buflen;
    bool buf_overflow;
    bool asset_name_ok;
    bool asset_url_ok;
} release_scan_t;

/** Reset the scanner. app_asset_name is the preferred asset, e.g. "thermux.bin". */
void release_scan_init(release_scan_t *s, const char *app_asset_name);

/** Feed the next chunk of the response body. Chunks may split tokens anywhere. */
void release_scan_feed(release_scan_t *s, const char *data, size_t len);

/**
 * Finish scanning. Returns true if a non-draft release with a tag_name was
 * read completely; the result is then in s->tag, s->url, s->url_is_exact and
 * s->prerelease. Returns false for malformed or truncated JSON, or if every
 * release is a draft.
 */
bool release_scan_finish(release_scan_t *s);

#endif /* RELEASE_SCAN_H */
