// SPDX-License-Identifier: GPL-2.0
/*
 * module_overlay: replace selected kernel modules at load time with a
 * fixed copy embedded (uncompressed) in the kernel image.
 *
 * Adapted for android12-5.4 from the OnePlus SM8750 (6.6) implementation.
 * The original embeds zstd-compressed modules; here they are stored raw
 * to avoid a decompress step on the early module-load path. The per-module
 * cmdline skip feature was dropped -- matched modules are always
 * intercepted. Used to substitute a corrected dwc3-sprd-core.ko (USB RNDIS
 * teardown fix) without touching the signed vendor_boot ramdisk it
 * normally loads from.
 */
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include "../module-internal.h"
#include "overlay_files.h"

static const struct overlay_file *find_overlay(const char *name)
{
	int i;

	for (i = 0; i < overlay_file_list_count; i++)
		if (strcmp(overlay_file_list[i].name, name) == 0)
			return &overlay_file_list[i];
	return NULL;
}

bool should_intercept_module(const char *name)
{
	return name && find_overlay(name) != NULL;
}

enum intercept_status intercept_module_load(struct load_info *info,
					    const char *name)
{
	const struct overlay_file *ov;
	void *buf;

	ov = find_overlay(name);
	if (!ov)
		return INTERCEPT_STATUS_SKIP;

	buf = vmalloc(ov->size);
	if (!buf) {
		pr_err("module_overlay: vmalloc(%zu) failed for %s\n",
		       ov->size, name);
		return INTERCEPT_STATUS_ERROR;
	}
	memcpy(buf, ov->data, ov->size);

	/* Drop the copy that came from userspace / vendor_boot. */
	vfree(info->hdr);
	info->hdr = buf;
	info->len = ov->size;

	pr_info("module_overlay: %s replaced with embedded version (%zu bytes)\n",
		name, ov->size);

	return INTERCEPT_STATUS_SUCCESS;
}
