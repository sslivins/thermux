/**
 * @file version_utils.h
 * @brief Version comparison utilities (host-testable)
 */

#ifndef VERSION_UTILS_H
#define VERSION_UTILS_H

/**
 * @brief Compare version strings (semantic versioning)
 * 
 * Supports formats: "1.0.0", "v1.0.0", "V1.0.0"
 * 
 * @param v1 First version string
 * @param v2 Second version string
 * @return positive if v1 > v2, negative if v1 < v2, 0 if equal
 */
int version_compare(const char *v1, const char *v2);

/**
 * @brief Check if version v1 is newer than v2
 * 
 * @param v1 First version string
 * @param v2 Second version string
 * @return 1 if v1 is newer, 0 otherwise
 */
int version_is_newer(const char *v1, const char *v2);

#endif /* VERSION_UTILS_H */
