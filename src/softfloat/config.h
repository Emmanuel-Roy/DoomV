/* Minimal replacement for the config.h Berkeley SoftFloat expects.
 *
 * Upstream, spike generates this with autoconf; DoomV builds SoftFloat
 * directly from the vendored sources, so the one thing platform.h actually
 * reads from it has to be supplied here.
 *
 * That one thing is WORDS_BIGENDIAN. Leaving it undefined selects the
 * little-endian layout, which is correct for every host DoomV builds on and
 * for RISC-V itself. If this is ever ported to a big-endian host, define it
 * -- SoftFloat's 64-bit-pair layouts depend on it, and getting it wrong
 * produces wrong results rather than a build error.
 */
#ifndef DOOMV_SOFTFLOAT_CONFIG_H
#define DOOMV_SOFTFLOAT_CONFIG_H
/* #undef WORDS_BIGENDIAN */
#endif
