/*
 * Kernel based bootsplash.
 *
 * (Rendering functions)
 *
 * Authors:
 * Max Staudt <mstaudt@suse.de>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#define pr_fmt(fmt) "bootsplash: " fmt


#include <linux/bootsplash.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/types.h>

#include "bootsplash_internal.h"




/*
 * Rendering: Internal drawing routines
 */


/*
 * Pack pixel into target format and do Big/Little Endian handling.
 * This would be a good place to handle endianness conversion if necessary.
 */
static inline u32 pack_pixel(const struct fb_var_screeninfo *dst_var,
			     u8 red, u8 green, u8 blue)
{
	u32 dstpix;

	/* Quantize pixel */
	red = red >> (8 - dst_var->red.length);
	green = green >> (8 - dst_var->green.length);
	blue = blue >> (8 - dst_var->blue.length);

	/* Pack pixel */
	dstpix = red << (dst_var->red.offset)
		| green << (dst_var->green.offset)
		| blue << (dst_var->blue.offset);

	/*
	 * Move packed pixel to the beginning of the memory cell,
	 * so we can memcpy() it out easily
	 */
#ifdef __BIG_ENDIAN
	switch (dst_var->bits_per_pixel) {
	case 16:
		dstpix <<= 16;
		break;
	case 24:
		dstpix <<= 8;
		break;
	case 32:
		break;
	}
#else
	/* This is intrinsically unnecessary on Little Endian */
#endif

	return dstpix;
}


void bootsplash_do_render_background(struct fb_info *info)
{
	unsigned int x, y;
	u32 dstpix;
	u32 dst_octpp = info->var.bits_per_pixel / 8;

	dstpix = pack_pixel(&info->var,
			    0,
			    0,
			    0);

	for (y = 0; y < info->var.yres_virtual; y++) {
		u8 *dstline = info->screen_buffer + (y * info->fix.line_length);

		for (x = 0; x < info->var.xres_virtual; x++) {
			memcpy(dstline, &dstpix, dst_octpp);

			dstline += dst_octpp;
		}
	}
}
