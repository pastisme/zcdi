/* zcdi: replaces configure's config.h. Portable C only: no ARCH_* or HAVE_MMX,
 * so every platform decodes bit-identically. */
#ifndef LIBMPEG2_CONFIG_H
#define LIBMPEG2_CONFIG_H

#if defined(__GNUC__) || defined(__clang__)
#define HAVE_BUILTIN_EXPECT 1
/* idct.c accesses DCTblock as int32_t; ARM faults if it isn't aligned. */
#define ATTRIBUTE_ALIGNED_MAX 64
#endif

#endif
