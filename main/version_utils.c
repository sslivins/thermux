/**
 * @file version_utils.c
 * @brief Version comparison utilities (host-testable)
 */

#include "version_utils.h"
#include <stdio.h>

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
    return patch1 - patch2;
}

int version_is_newer(const char *v1, const char *v2)
{
    return version_compare(v1, v2) > 0;
}
