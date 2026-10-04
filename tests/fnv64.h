/* Shared full-byte FNV-1a 64 hash for byte-identity equality claims.
 * One copy, one name (PixelHashAll): the per-suite sampled change-detection
 * hash (stride 97) must never be used for an equality claim, and three
 * private copies under three names invited exactly that mix-up
 * (2026-10-05 review). Suites include this from their own directory:
 * cl runs from the repo root, so use the relative form below. */
#ifndef MNPAPER_FNV64_H
#define MNPAPER_FNV64_H
#include <stddef.h>
static unsigned long long PixelHashAll(const unsigned char *p, size_t bytes) {
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
#endif
