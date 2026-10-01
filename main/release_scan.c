/**
 * @file release_scan.c
 * @brief Streaming scanner for GitHub release JSON
 *
 * A small JSON lexer that tracks nesting and the current key per object, and
 * copies out only the few string/boolean values the OTA updater needs. All
 * state lives in release_scan_t, so a token can be split across any number of
 * chunks.
 */

#include "release_scan.h"
#include <string.h>

enum { LEX_VALUE, LEX_STRING, LEX_ESCAPE, LEX_UNICODE, LEX_LITERAL };

enum { KEY_NONE, KEY_TAG_NAME, KEY_DRAFT, KEY_PRERELEASE, KEY_ASSETS,
       KEY_NAME, KEY_BROWSER_URL };

enum { TGT_NONE, TGT_TAG, TGT_ASSET_NAME, TGT_ASSET_URL };

#define IS_ARRAY(s, d) (((s)->is_array >> (d)) & 1u)

static void reset_release(release_scan_t *s)
{
    s->tag[0] = '\0';
    s->url[0] = '\0';
    s->url_is_exact = false;
    s->prerelease = false;
    s->tag_ok = false;
    s->draft = false;
}

static void reset_asset(release_scan_t *s)
{
    s->asset_name[0] = '\0';
    s->asset_url[0] = '\0';
    s->asset_name_ok = false;
    s->asset_url_ok = false;
}

void release_scan_init(release_scan_t *s, const char *app_asset_name)
{
    memset(s, 0, sizeof(*s));
    if (app_asset_name) {
        strncpy(s->app_asset_name, app_asset_name, sizeof(s->app_asset_name) - 1);
    }
}

static uint8_t lookup_key(const char *k)
{
    if (strcmp(k, "tag_name") == 0) return KEY_TAG_NAME;
    if (strcmp(k, "draft") == 0) return KEY_DRAFT;
    if (strcmp(k, "prerelease") == 0) return KEY_PRERELEASE;
    if (strcmp(k, "assets") == 0) return KEY_ASSETS;
    if (strcmp(k, "name") == 0) return KEY_NAME;
    if (strcmp(k, "browser_download_url") == 0) return KEY_BROWSER_URL;
    return KEY_NONE;
}

static uint8_t key_at(const release_scan_t *s, unsigned d)
{
    return d < sizeof(s->key_id) ? s->key_id[d] : KEY_NONE;
}

/* True when depth d is an object inside the release's "assets" array. */
static bool in_asset(const release_scan_t *s, unsigned d)
{
    unsigned r = s->release_depth;
    return d == r + 2 && !IS_ARRAY(s, d) && IS_ARRAY(s, r + 1) &&
           key_at(s, r) == KEY_ASSETS;
}

/* True when depth d is a release object. */
static bool is_release(const release_scan_t *s, unsigned d)
{
    return d == s->release_depth && !IS_ARRAY(s, d) &&
           (d == 1 || IS_ARRAY(s, d - 1));
}

static char *target_buf(release_scan_t *s, size_t *cap)
{
    switch (s->str_target) {
    case TGT_TAG:        *cap = sizeof(s->tag);        return s->tag;
    case TGT_ASSET_NAME: *cap = sizeof(s->asset_name); return s->asset_name;
    case TGT_ASSET_URL:  *cap = sizeof(s->asset_url);  return s->asset_url;
    default:             *cap = 0;                     return NULL;
    }
}

static void str_putc(release_scan_t *s, char c)
{
    if (s->str_is_key) {
        if ((size_t)s->keylen + 1 < sizeof(s->keybuf)) {
            s->keybuf[s->keylen++] = c;
        } else {
            s->key_overflow = true;
        }
        return;
    }
    size_t cap;
    char *buf = target_buf(s, &cap);
    if (buf == NULL) {
        return;
    }
    if (s->buflen + 1u < cap) {
        buf[s->buflen++] = c;
    } else {
        s->buf_overflow = true;
    }
}

static void begin_string(release_scan_t *s)
{
    s->lex = LEX_STRING;
    s->str_is_key = s->expect_key;
    s->keylen = 0;
    s->key_overflow = false;
    s->buflen = 0;
    s->buf_overflow = false;
    s->str_target = TGT_NONE;
    if (s->str_is_key) {
        return;
    }

    unsigned d = s->depth;
    if (is_release(s, d) && key_at(s, d) == KEY_TAG_NAME) {
        s->str_target = TGT_TAG;
    } else if (in_asset(s, d) && key_at(s, d) == KEY_NAME) {
        s->str_target = TGT_ASSET_NAME;
    } else if (in_asset(s, d) && key_at(s, d) == KEY_BROWSER_URL) {
        s->str_target = TGT_ASSET_URL;
    }
}

static void end_string(release_scan_t *s)
{
    s->lex = LEX_VALUE;
    if (s->str_is_key) {
        s->keybuf[s->keylen] = '\0';
        if (s->depth < sizeof(s->key_id)) {
            s->key_id[s->depth] = s->key_overflow ? KEY_NONE : lookup_key(s->keybuf);
        }
        return;
    }
    size_t cap;
    char *buf = target_buf(s, &cap);
    if (buf == NULL) {
        return;
    }
    buf[s->buflen] = '\0';
    bool ok = !s->buf_overflow;
    switch (s->str_target) {
    case TGT_TAG:        s->tag_ok = ok && s->buflen > 0; if (!ok) s->tag[0] = '\0'; break;
    case TGT_ASSET_NAME: s->asset_name_ok = ok; break;
    case TGT_ASSET_URL:  s->asset_url_ok = ok; break;
    default: break;
    }
}

static bool ends_with_bin(const char *name)
{
    size_t n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, ".bin") == 0;
}

static void finish_asset(release_scan_t *s)
{
    if (!s->asset_name_ok || !s->asset_url_ok || s->url_is_exact) {
        return;
    }
    const char *nm = s->asset_name;
    if (strcmp(nm, "bootloader.bin") == 0 || strcmp(nm, "partition-table.bin") == 0 ||
        !ends_with_bin(nm)) {
        return;
    }
    if (s->app_asset_name[0] && strcmp(nm, s->app_asset_name) == 0) {
        strcpy(s->url, s->asset_url);
        s->url_is_exact = true;
    } else if (s->url[0] == '\0') {
        strcpy(s->url, s->asset_url);
    }
}

static void end_literal(release_scan_t *s)
{
    s->lit[s->litlen] = '\0';
    s->lex = LEX_VALUE;
    unsigned d = s->depth;
    if (!is_release(s, d)) {
        return;
    }
    bool is_true = strcmp(s->lit, "true") == 0;
    if (key_at(s, d) == KEY_DRAFT) {
        s->draft = is_true;
    } else if (key_at(s, d) == KEY_PRERELEASE) {
        s->prerelease = is_true;
    }
}

static bool is_literal_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '+' || c == '.' || c == 'E';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void open_container(release_scan_t *s, bool array)
{
    if (s->depth >= RELEASE_SCAN_MAX_DEPTH) {
        s->error = true;
        return;
    }
    if (s->release_depth == 0) {
        s->release_depth = array ? 2 : 1;
    }
    s->depth++;
    if (array) {
        s->is_array |= (uint64_t)1 << s->depth;
    } else {
        s->is_array &= ~((uint64_t)1 << s->depth);
    }
    if (s->depth < sizeof(s->key_id)) {
        s->key_id[s->depth] = KEY_NONE;
    }
    s->expect_key = !array;

    if (is_release(s, s->depth)) {
        reset_release(s);
    } else if (in_asset(s, s->depth)) {
        reset_asset(s);
    }
}

static void close_container(release_scan_t *s, bool array)
{
    if (s->depth == 0 || (bool)IS_ARRAY(s, s->depth) != array) {
        s->error = true;
        return;
    }
    unsigned d = s->depth;
    if (!array && in_asset(s, d)) {
        finish_asset(s);
    } else if (!array && is_release(s, d) && !s->draft) {
        s->done = true;
    }
    s->depth--;
    s->expect_key = false;
}

void release_scan_feed(release_scan_t *s, const char *data, size_t len)
{
    for (size_t i = 0; i < len && !s->error && !s->done; i++) {
        char c = data[i];
        switch (s->lex) {
        case LEX_STRING:
            if (c == '"') {
                end_string(s);
            } else if (c == '\\') {
                s->lex = LEX_ESCAPE;
            } else if ((unsigned char)c < 0x20) {
                s->error = true;
            } else {
                str_putc(s, c);
            }
            continue;

        case LEX_ESCAPE:
            s->lex = LEX_STRING;
            switch (c) {
            case '"': case '\\': case '/': str_putc(s, c); break;
            case 'b': str_putc(s, '\b'); break;
            case 'f': str_putc(s, '\f'); break;
            case 'n': str_putc(s, '\n'); break;
            case 'r': str_putc(s, '\r'); break;
            case 't': str_putc(s, '\t'); break;
            case 'u': s->lex = LEX_UNICODE; s->u_count = 0; s->u_val = 0; break;
            default: s->error = true; break;
            }
            continue;

        case LEX_UNICODE: {
            int h = hex_val(c);
            if (h < 0) {
                s->error = true;
                continue;
            }
            s->u_val = (uint16_t)((s->u_val << 4) | (unsigned)h);
            if (++s->u_count == 4) {
                /* None of the fields we keep use non-ASCII; mark it, don't decode it. */
                str_putc(s, s->u_val < 0x80 ? (char)s->u_val : '?');
                s->lex = LEX_STRING;
            }
            continue;
        }

        case LEX_LITERAL:
            if (is_literal_char(c)) {
                if ((size_t)s->litlen + 1 < sizeof(s->lit)) {
                    s->lit[s->litlen++] = c;
                } else {
                    s->litlen = 0;  /* long number: not a boolean, value unused */
                }
                continue;
            }
            end_literal(s);
            break;  /* this char ends the literal; handle it as structure below */

        default:
            break;
        }

        /* LEX_VALUE: structural characters and whitespace */
        switch (c) {
        case ' ': case '\t': case '\r': case '\n':
            break;
        case '{': open_container(s, false); break;
        case '[': open_container(s, true); break;
        case '}': close_container(s, false); break;
        case ']': close_container(s, true); break;
        case ':':
            if (s->depth == 0 || IS_ARRAY(s, s->depth)) s->error = true;
            s->expect_key = false;
            break;
        case ',':
            if (s->depth == 0) s->error = true;
            s->expect_key = !IS_ARRAY(s, s->depth);
            break;
        case '"':
            if (s->depth == 0) s->error = true;
            else begin_string(s);
            break;
        default:
            if (s->depth > 0 && !s->expect_key && is_literal_char(c)) {
                s->lex = LEX_LITERAL;
                s->lit[0] = c;
                s->litlen = 1;
            } else {
                s->error = true;
            }
            break;
        }
    }
}

bool release_scan_finish(release_scan_t *s)
{
    if (s->error || !s->done || !s->tag_ok) {
        s->tag[0] = '\0';
        s->url[0] = '\0';
        s->url_is_exact = false;
        s->prerelease = false;
        return false;
    }
    return true;
}
