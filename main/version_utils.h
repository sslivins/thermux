/**
 * @file version_utils.h
 * @brief Version comparison utilities (host-testable)
 */

#ifndef VERSION_UTILS_H
#define VERSION_UTILS_H

/**
 * @brief Compare version strings (semantic versioning)
 * 
 * Supports formats: "1.0.0", "v1.0.0", "V1.0.0", and pre-release
 * suffixes such as "1.2.0-beta.1" or "1.2.0-rc.2", per SemVer
 * precedence rules (https://semver.org/#spec-item-11):
 *   - major.minor.patch are compared numerically first.
 *   - A version with a pre-release suffix is lower than the same
 *     major.minor.patch without one (e.g. "1.2.0-beta.1" < "1.2.0").
 *   - When both have pre-release suffixes, dot-separated identifiers
 *     are compared left to right: numeric identifiers compare
 *     numerically, alphanumeric identifiers compare lexically, and a
 *     shorter identifier list that is otherwise a prefix of a longer
 *     one is lower (e.g. "1.2.0-beta.1" < "1.2.0-beta.1.1").
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
