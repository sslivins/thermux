/**
 * @file test_release_scan.c
 * @brief Unit tests for the streaming GitHub release scanner
 *
 * The fixtures under test/fixtures are real responses captured from the
 * GitHub API (/releases/latest and /releases?per_page=2).
 */

#include "unity.h"
#include "release_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "fixtures"
#endif

#define APP "thermux.bin"
#define URL_351 "https://github.com/sslivins/thermux/releases/download/v3.5.1/thermux.bin"

static char *load_fixture(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  cannot open fixture %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

/* Feed the whole text in chunks of the given size (0 = one chunk). */
static bool scan(release_scan_t *s, const char *json, size_t len, size_t chunk)
{
    release_scan_init(s, APP);
    if (chunk == 0) {
        chunk = len ? len : 1;
    }
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        release_scan_feed(s, json + off, n);
    }
    return release_scan_finish(s);
}

static bool scan_str(release_scan_t *s, const char *json)
{
    return scan(s, json, strlen(json), 0);
}

void test_release_scan_real_latest_any_chunking(void)
{
    size_t len;
    char *json = load_fixture("release_latest.json", &len);
    TEST_ASSERT_NOT_NULL(json);
    if (!json) return;

    /* Every chunk size from 1 to 64 bytes, then the sizes esp_http_client uses. */
    static const size_t big[] = {0, 100, 512, 1000, 4096};
    for (size_t c = 1; c <= 64 + sizeof(big) / sizeof(big[0]); c++) {
        size_t chunk = c <= 64 ? c : big[c - 65];
        release_scan_t s;
        bool ok = scan(&s, json, len, chunk);
        if (!ok || strcmp(s.tag, "v3.5.1") != 0 || strcmp(s.url, URL_351) != 0) {
            printf("  chunk size %zu failed: ok=%d tag='%s' url='%s'\n",
                   chunk, ok, s.tag, s.url);
            TEST_ASSERT_TRUE(false);
            break;
        }
        TEST_ASSERT_TRUE(s.url_is_exact);
        TEST_ASSERT_FALSE(s.prerelease);
    }
    free(json);
}

void test_release_scan_real_list_picks_first(void)
{
    size_t len;
    char *json = load_fixture("releases_list.json", &len);
    TEST_ASSERT_NOT_NULL(json);
    if (!json) return;
    for (size_t chunk = 1; chunk <= 700; chunk += 37) {
        release_scan_t s;
        TEST_ASSERT_TRUE(scan(&s, json, len, chunk));
        TEST_ASSERT_EQUAL_STRING("v3.5.1", s.tag);
        TEST_ASSERT_EQUAL_STRING(URL_351, s.url);
    }
    free(json);
}

void test_release_scan_rejects_truncated_response(void)
{
    size_t len;
    char *json = load_fixture("release_latest.json", &len);
    TEST_ASSERT_NOT_NULL(json);
    if (!json) return;
    /* Any cut before the closing brace must fail, never return a partial answer. */
    for (size_t cut = 0; cut < len; cut += 97) {
        release_scan_t s;
        TEST_ASSERT_FALSE(scan(&s, json, cut, 128));
        TEST_ASSERT_EQUAL_STRING("", s.tag);
    }
    release_scan_t s;
    TEST_ASSERT_FALSE(scan(&s, json, len - 1, 0));
    free(json);
}

void test_release_scan_skips_drafts_in_list(void)
{
    release_scan_t s;
    TEST_ASSERT_TRUE(scan_str(&s,
        "[{\"tag_name\":\"v9.0.0\",\"draft\":true,\"prerelease\":false,"
        "\"assets\":[{\"name\":\"thermux.bin\",\"browser_download_url\":\"https://x/draft.bin\"}]},"
        "{\"assets\":[{\"browser_download_url\":\"https://x/beta.bin\",\"name\":\"thermux.bin\"}],"
        "\"prerelease\":true,\"draft\":false,\"tag_name\":\"v8.0.0-beta\"}]"));
    TEST_ASSERT_EQUAL_STRING("v8.0.0-beta", s.tag);
    TEST_ASSERT_EQUAL_STRING("https://x/beta.bin", s.url);
    TEST_ASSERT_TRUE(s.prerelease);
}

void test_release_scan_all_drafts_fails(void)
{
    release_scan_t s;
    TEST_ASSERT_FALSE(scan_str(&s,
        "[{\"tag_name\":\"v9.0.0\",\"draft\":true,\"assets\":[]}]"));
    TEST_ASSERT_FALSE(scan_str(&s, "[]"));
}

void test_release_scan_asset_selection(void)
{
    release_scan_t s;
    /* Exact app asset wins even when listed after a fallback .bin. */
    TEST_ASSERT_TRUE(scan_str(&s,
        "{\"tag_name\":\"v1.2.3\",\"assets\":["
        "{\"name\":\"bootloader.bin\",\"browser_download_url\":\"https://x/boot\"},"
        "{\"name\":\"other.bin\",\"browser_download_url\":\"https://x/other\"},"
        "{\"name\":\"partition-table.bin\",\"browser_download_url\":\"https://x/pt\"},"
        "{\"name\":\"thermux.bin\",\"browser_download_url\":\"https://x/app\"},"
        "{\"name\":\"zzz.bin\",\"browser_download_url\":\"https://x/zzz\"}]}"));
    TEST_ASSERT_EQUAL_STRING("https://x/app", s.url);
    TEST_ASSERT_TRUE(s.url_is_exact);

    /* No exact match: first plausible .bin, skipping bootloader/partition table. */
    TEST_ASSERT_TRUE(scan_str(&s,
        "{\"tag_name\":\"v1.2.3\",\"assets\":["
        "{\"name\":\"bootloader.bin\",\"browser_download_url\":\"https://x/boot\"},"
        "{\"name\":\"notes.txt\",\"browser_download_url\":\"https://x/txt\"},"
        "{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw\"},"
        "{\"name\":\"fw2.bin\",\"browser_download_url\":\"https://x/fw2\"}]}"));
    TEST_ASSERT_EQUAL_STRING("https://x/fw", s.url);
    TEST_ASSERT_FALSE(s.url_is_exact);

    /* Only non-app binaries: tag found, no URL. */
    TEST_ASSERT_TRUE(scan_str(&s,
        "{\"tag_name\":\"v1.2.3\",\"assets\":["
        "{\"name\":\"bootloader.bin\",\"browser_download_url\":\"https://x/boot\"},"
        "{\"name\":\"partition-table.bin\",\"browser_download_url\":\"https://x/pt\"}]}"));
    TEST_ASSERT_EQUAL_STRING("v1.2.3", s.tag);
    TEST_ASSERT_EQUAL_STRING("", s.url);
}

void test_release_scan_ignores_nested_lookalikes(void)
{
    release_scan_t s;
    /* tag_name / name / url keys nested inside other objects must not leak in. */
    TEST_ASSERT_TRUE(scan_str(&s,
        "{\"author\":{\"tag_name\":\"evil\",\"draft\":true},"
        "\"name\":\"thermux.bin\",\"browser_download_url\":\"https://x/release-level\","
        "\"tag_name\":\"v2.0.0\","
        "\"assets\":[{\"uploader\":{\"name\":\"thermux.bin\",\"browser_download_url\":\"https://x/evil\"},"
        "\"name\":\"thermux.bin\",\"browser_download_url\":\"https://x/good\"}],"
        "\"reactions\":{\"assets\":[{\"name\":\"thermux.bin\",\"browser_download_url\":\"https://x/evil2\"}]}}"));
    TEST_ASSERT_EQUAL_STRING("v2.0.0", s.tag);
    TEST_ASSERT_EQUAL_STRING("https://x/good", s.url);
}

void test_release_scan_escapes_and_whitespace(void)
{
    release_scan_t s;
    TEST_ASSERT_TRUE(scan_str(&s,
        " {\n \"body\" : \"quote \\\" brace } bracket ] \\\\\\\" \\u00e9 \\n\",\n"
        "  \"tag_name\" : \"v1\\u002e0\\/1\" ,\n"
        "  \"draft\" : false , \"prerelease\" : true , \"id\" : -1.5e+3 , \"x\" : null,\n"
        "  \"assets\" : [ { \"name\" : \"thermux.bin\" ,"
        " \"browser_download_url\" : \"https:\\/\\/x\\/a.bin\" } ]\n } \n"));
    TEST_ASSERT_EQUAL_STRING("v1.0/1", s.tag);
    TEST_ASSERT_EQUAL_STRING("https://x/a.bin", s.url);
    TEST_ASSERT_TRUE(s.prerelease);
}

void test_release_scan_rejects_malformed(void)
{
    release_scan_t s;
    TEST_ASSERT_FALSE(scan_str(&s, ""));
    TEST_ASSERT_FALSE(scan_str(&s, "{\"tag_name\":\"v1\"]"));
    TEST_ASSERT_FALSE(scan_str(&s, "{\"tag_name\":\"v1\" @ }"));
    TEST_ASSERT_FALSE(scan_str(&s, "{\"tag_name\":\"v1\\q\"}"));
    TEST_ASSERT_FALSE(scan_str(&s, "{\"tag_name\":\"\"}"));
    TEST_ASSERT_FALSE(scan_str(&s, "{\"tag_name\":123}"));
    TEST_ASSERT_FALSE(scan_str(&s, "{\"message\":\"API rate limit exceeded\"}"));
    TEST_ASSERT_FALSE(scan_str(&s, "<html>"));
}

void test_release_scan_overlong_values(void)
{
    release_scan_t s;
    char json[1400];
    char longtag[64];
    memset(longtag, 'a', sizeof(longtag) - 1);
    longtag[sizeof(longtag) - 1] = '\0';
    snprintf(json, sizeof(json), "{\"tag_name\":\"%s\"}", longtag);
    TEST_ASSERT_FALSE(scan_str(&s, json));

    /* An overlong asset URL is dropped rather than truncated. */
    char longurl[700];
    memset(longurl, 'u', sizeof(longurl) - 1);
    longurl[sizeof(longurl) - 1] = '\0';
    snprintf(json, sizeof(json),
             "{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"thermux.bin\","
             "\"browser_download_url\":\"%s\"}]}", longurl);
    TEST_ASSERT_TRUE(scan_str(&s, json));
    TEST_ASSERT_EQUAL_STRING("", s.url);
}

void run_release_scan_tests(void)
{
    RUN_TEST(test_release_scan_real_latest_any_chunking);
    RUN_TEST(test_release_scan_real_list_picks_first);
    RUN_TEST(test_release_scan_rejects_truncated_response);
    RUN_TEST(test_release_scan_skips_drafts_in_list);
    RUN_TEST(test_release_scan_all_drafts_fails);
    RUN_TEST(test_release_scan_asset_selection);
    RUN_TEST(test_release_scan_ignores_nested_lookalikes);
    RUN_TEST(test_release_scan_escapes_and_whitespace);
    RUN_TEST(test_release_scan_rejects_malformed);
    RUN_TEST(test_release_scan_overlong_values);
}
