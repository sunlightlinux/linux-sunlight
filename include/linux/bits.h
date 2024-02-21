/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BITS_H
#define __LINUX_BITS_H

#include <linux/const.h>
#include <vdso/bits.h>
#include <asm/bitsperlong.h>

#define BITS_PER_TYPE(type)	(sizeof(type) * BITS_PER_BYTE)

#define BIT_MASK(nr)		(UL(1) << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)	(ULL(1) << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define BITS_PER_BYTE		8

/*
 * Create a contiguous bitmask starting at bit position @l and ending at
 * position @h. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#if !defined(__ASSEMBLY__)
#include <linux/build_bug.h>
#define GENMASK_INPUT_CHECK(h, l) \
	(BUILD_BUG_ON_ZERO(__builtin_choose_expr( \
		__is_constexpr((l) > (h)), (l) > (h), 0)))
#define BIT_INPUT_CHECK(type, b) \
	((BUILD_BUG_ON_ZERO(__builtin_choose_expr( \
		__is_constexpr(b), (b) >= BITS_PER_TYPE(type), 0))))
#else
/*
 * BUILD_BUG_ON_ZERO is not available in h files included from asm files,
 * disable the input check if that is the case.
 */
#define GENMASK_INPUT_CHECK(h, l) 0
#define BIT_INPUT_CHECK(type, b) 0
#endif

/*
 * Generate a mask for the specified type @t. Additional checks are made to
 * guarantee the value returned fits in that type, relying on
 * shift-count-overflow compiler check to detect incompatible arguments.
 * For example, all these create build errors or warnings:
 *
 * - GENMASK(15, 20): wrong argument order
 * - GENMASK(72, 15): doesn't fit unsigned long
 * - GENMASK_U32(33, 15): doesn't fit in a u32
 */
#define __GENMASK(t, h, l) \
	(GENMASK_INPUT_CHECK(h, l) + \
	 (((t)~0ULL - ((t)(1) << (l)) + 1) & \
	 ((t)~0ULL >> (BITS_PER_TYPE(t) - 1 - (h)))))

#define GENMASK(h, l)		__GENMASK(unsigned long,  h, l)
#define GENMASK_ULL(h, l)	__GENMASK(unsigned long long, h, l)
#define GENMASK_U8(h, l)	__GENMASK(u8,  h, l)
#define GENMASK_U16(h, l)	__GENMASK(u16, h, l)
#define GENMASK_U32(h, l)	__GENMASK(u32, h, l)
#define GENMASK_U64(h, l)	__GENMASK(u64, h, l)

/*
 * Fixed-type variants of BIT(), with additional checks like __GENMASK().  The
 * following examples generate compiler warnings due to shift-count-overflow:
 *
 * - BIT_U8(8)
 * - BIT_U32(-1)
 * - BIT_U32(40)
 */
#define BIT_U8(b)		((u8)(BIT_INPUT_CHECK(u8, b) + BIT(b)))
#define BIT_U16(b)		((u16)(BIT_INPUT_CHECK(u16, b) + BIT(b)))
#define BIT_U32(b)		((u32)(BIT_INPUT_CHECK(u32, b) + BIT(b)))
#define BIT_U64(b)		((u64)(BIT_INPUT_CHECK(u64, b) + BIT(b)))

#endif	/* __LINUX_BITS_H */
