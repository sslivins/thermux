/**
 * @file version_utils.c
 * @brief Version comparison utilities (host-testable)
 */

/* Needed so glibc/newlib expose strtok_r's prototype under -std=c11
 * (strict ISO C mode hides POSIX extensions by default). */
#define _POSIX_C_SOURCE 200809L

#include "version_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* Compare a single dot-separated pre-release identifier per SemVer
 * rules: numeric identifiers compare numerically and are always
 * lower than alphanumeric ones; alphanumeric identifiers compare
 * lexically (ASCII). */
static int compare_prerelease_identifier(const char *a, const char *b)
{
    bool a_numeric = (a[0] != '\0');
    bool b_numeric = (b[0] != '\0');
    for (const char *p = a; *p; p++) {
        if (!isdigit((unsigned char)*p)) { a_numeric = false; break; }
    }
    for (const char *p = b; *p; p++) {
        if (!isdigit((unsigned char)*p)) { b_numeric = false; break; }
    }

    if (a_numeric && b_numeric) {
        long an = strtol(a, NULL, 10);
        long bn = strtol(b, NULL, 10);
        if (an != bn) return (an > bn) ? 1 : -1;
        return 0;
    }
    if (a_numeric != b_numeric) {
        /* Numeric identifiers always have lower precedence than
         * alphanumeric identifiers. */
        return a_numeric ? -1 : 1;
    }
    return strcmp(a, b) < 0 ? -1 : (strcmp(a, b) > 0 ? 1 : 0);
}

/* Compare two pre-release strings (the part after '-', or NULL/"" if
 * there is no pre-release suffix). A version without a pre-release
 * suffix has higher precedence than one with, given equal
 * major.minor.patch. */
static int compare_prerelease(const char *pre1, const char *pre2)
{
    bool has1 = (pre1 != NULL && pre1[0] != '\0');
    bool has2 = (pre2 != NULL && pre2[0] != '\0');

    if (!has1 && !has2) return 0;
    if (!has1) return 1;   /* v1 has no pre-release -> v1 is newer */
    if (!has2) return -1;  /* v2 has no pre-release -> v2 is newer */

    char buf1[64], buf2[64];
    strncpy(buf1, pre1, sizeof(buf1) - 1); buf1[sizeof(buf1) - 1] = '\0';
    strncpy(buf2, pre2, sizeof(buf2) - 1); buf2[sizeof(buf2) - 1] = '\0';

    char *save1 = NULL, *save2 = NULL;
    char *tok1 = strtok_r(buf1, ".", &save1);
    char *tok2 = strtok_r(buf2, ".", &save2);

    while (tok1 != NULL && tok2 != NULL) {
        int cmp = compare_prerelease_identifier(tok1, tok2);
        if (cmp != 0) return cmp;
        tok1 = strtok_r(NULL, ".", &save1);
        tok2 = strtok_r(NULL, ".", &save2);
    }

    if (tok1 == NULL && tok2 == NULL) return 0;
    /* A shorter set of identifiers (that matched so far) has lower
     * precedence than a longer one. */
    return (tok1 == NULL) ? -1 : 1;
}

int version_compare(const char *v1, const char *v2)
{
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    if (v1 == NULL || v2 == NULL) {
        return 0;
    }
    
    /* Skip 'v' prefix if present */
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    if (patch1 != patch2) return patch1 - patch2;

    /* Numeric core is equal; fall back to pre-release precedence. */
    const char *dash1 = strchr(v1, '-');
    const char *dash2 = strchr(v2, '-');
    return compare_prerelease(dash1 ? dash1 + 1 : NULL, dash2 ? dash2 + 1 : NULL);
}

int version_is_newer(const char *v1, const char *v2)
{
    return version_compare(v1, v2) > 0;
}
