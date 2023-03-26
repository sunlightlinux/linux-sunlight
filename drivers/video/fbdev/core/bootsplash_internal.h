/*
 * Kernel based bootsplash.
 *
 * (Internal data structures used at runtime)
 *
 * Authors:
 * Max Staudt <mstaudt@suse.de>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef __BOOTSPLASH_INTERNAL_H
#define __BOOTSPLASH_INTERNAL_H


#include <linux/types.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>


/*
 * Runtime types
 */
struct splash_priv {
	/*
	 * Enabled/disabled state, to be used with atomic bit operations.
	 *   Bit 0: 0 = Splash hidden
	 *          1 = Splash shown
	 *
	 * Note: fbcon.c uses this twice, by calling
	 *       bootsplash_would_render_now() in set_blitting_type() and
	 *       in fbcon_switch().
	 *       This is racy, but eventually consistent: Turning the
	 *       splash on/off will cause a redraw, which calls
	 *       fbcon_switch(), which calls set_blitting_type().
	 *       So the last on/off toggle will make things consistent.
	 */
	unsigned long enabled;

	/* Our gateway to userland via sysfs */
	struct platform_device *splash_device;

	struct work_struct work_redraw_vc;
};



/*
 * Rendering functions
 */
void bootsplash_do_render_background(struct fb_info *info);

#endif
