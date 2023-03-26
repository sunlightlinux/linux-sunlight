/*
 * Kernel based bootsplash.
 *
 * (File format)
 *
 * Authors:
 * Max Staudt <mstaudt@suse.de>
 *
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 */

#ifndef __BOOTSPLASH_FILE_H
#define __BOOTSPLASH_FILE_H


#define BOOTSPLASH_VERSION 55561


#include <linux/kernel.h>
#include <linux/types.h>


/*
 * On-disk types
 *
 * A splash file consists of:
 *  - One single 'struct splash_file_header'
 *  - An array of 'struct splash_pic_header'
 *  - An array of raw data blocks, each padded to 16 bytes and
 *    preceded by a 'struct splash_blob_header'
 *
 * A single-frame splash may look like this:
 *
 * +--------------------+
 * |                    |
 * | splash_file_header |
 * |  -> num_blobs = 1  |
 * |  -> num_pics = 1   |
 * |                    |
 * +--------------------+
 * |                    |
 * | splash_pic_header  |
 * |                    |
 * +--------------------+
 * |                    |
 * | splash_blob_header |
 * |  -> type = 0       |
 * |  -> picture_id = 0 |
 * |                    |
 * | (raw RGB data)     |
 * | (pad to 16 bytes)  |
 * |                    |
 * +--------------------+
 *
 * All multi-byte values are stored on disk in the native format
 * expected by the system the file will be used on.
 */
#define BOOTSPLASH_MAGIC_BE "Linux bootsplash"
#define BOOTSPLASH_MAGIC_LE "hsalpstoob xuniL"

struct splash_file_header {
	uint8_t  id[16]; /* "Linux bootsplash" (no trailing NUL) */

	/* Splash file format version to avoid clashes */
	uint16_t version;

	/* The background color */
	uint8_t bg_red;
	uint8_t bg_green;
	uint8_t bg_blue;
	uint8_t bg_reserved;

	/*
	 * Number of pic/blobs so we can allocate memory for internal
	 * structures ahead of time when reading the file
	 */
	uint16_t num_blobs;
	uint8_t num_pics;

	uint8_t padding[103];
} __attribute__((__packed__));


struct splash_pic_header {
	uint16_t width;
	uint16_t height;

	/*
	 * Number of data packages associated with this picture.
	 * Currently, the only use for more than 1 is for animations.
	 */
	uint8_t num_blobs;

	uint8_t padding[27];
} __attribute__((__packed__));


struct splash_blob_header {
	/* Length of the data block in bytes. */
	uint32_t length;

	/*
	 * Type of the contents.
	 *  0 - Raw RGB data.
	 */
	uint16_t type;

	/*
	 * Picture this blob is associated with.
	 * Blobs will be added to a picture in the order they are
	 * found in the file.
	 */
	uint8_t picture_id;

	uint8_t padding[9];
} __attribute__((__packed__));

#endif
