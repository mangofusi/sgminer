/*
 * Copyright 2013 Con Kolivas <kernel@kolivas.org>
 * Copyright 2013 Hashfast Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdbool.h>

#include "usbutils.h"
#include "fpgautils.h"

#include "driver-hashfast.h"

static hf_info_t **hashfast_infos;
struct device_drv hashfast_drv;

static int hashfast_reset(struct cgpu_info __maybe_unused *hashfast)
{
	return 0;
}

static bool hashfast_detect_common(struct cgpu_info *hashfast, int baud)
{
	hf_core_t **c, *core;
	hf_info_t *info;
	hf_job_t *j;
	int i, k, ret;

	hashfast_infos = realloc(hashfast_infos, sizeof(hf_info_t *) * (total_devices + 1));
	if (unlikely(!hashfast_infos))
		quit(1, "Failed to realloc hashfast_infos in hashfast_detect_common");
	// Assume success, allocate info ahead of reset, so reset can fill fields in
	info = calloc(sizeof(hf_info_t), 1);
	if (unlikely(!info))
		quit(1, "Failed to calloc info in hashfast_detect_common");
	hashfast_infos[hashfast->device_id] = info;
	info->tacho_enable = 1;
	info->miner_count = 1;
	info->max_search_difficulty = 12;
	info->baud_rate = baud;

	ret = hashfast_reset(hashfast);
	if (unlikely(ret)) {
		free(info);
		hashfast_infos[hashfast->device_id] = NULL;
		return false;
	}

	/* 1 Pending, 1 active for each */
	info->inflight_target = info->asic_count * info->core_count *2;

	switch (info->device_type) {
		default:
		case HFD_G1:
			/* Implies hash_loops = 0 for full nonce range */
			break;
		case HFD_ExpressAGX:
			/* ExpressAGX */
			info->hash_loops = 1 << 26;
			break;
		case HFD_VC709:
			/* Virtex 7
			 * Adjust according to fast or slow configuration */
			if (info->core_count > 5)
				info->hash_loops = 1 << 26;
			else
				info->hash_loops = 1 << 30;
			break;
	}
	applog(LOG_INFO, "Hashfast Detect: chips %d cores %d inflight_target %d entries",
	       info->asic_count, info->core_count, info->inflight_target);

	/* Initialize list heads */
	info->active.next = &info->active;
	info->active.prev = &info->active;
	info->inactive.next = &info->inactive;
	info->inactive.prev = &info->inactive;

	/* Allocate core data structures */
	info->cores = calloc(info->asic_count, sizeof(hf_core_t  *));
	if (unlikely(!info->cores))
		quit(1, "Failed to calloc info cores in hashfast_detect_common");
	c = info->cores;

	for (i = 0; i < info->asic_count; i++) {
		*c = calloc(info->core_count, sizeof(hf_core_t));
		if (unlikely(!*c))
			quit(1, "Failed to calloc hf_core_t in hashfast_detect_common");
		for (k = 0, core = *c; k < info->core_count; k++, core++)
			core->enabled = 1;
		c++;
	}

	/* Now allocate enough structures to hold all the in-flight work
	 * 2 per core - one active and one pending. These go on the inactive
	 * queue, and get used/recycled as required. */
	for (i = 0; i < info->asic_count * info->core_count * 2; i++) {
		j = calloc(sizeof(hf_job_t), 1);
		if (unlikely(!j))
			quit(1, "Failed to calloc hf_job_t in hashfast_detect_common");
		list_add(&info->inactive, &j->l);
	}

	info->inactive_count = info->asic_count * info->core_count * 2;
	applog(LOG_INFO, "Hashfast Detect: Allocated %d job entries",
	       info->inflight_target);

		// Finally, allocate enough space to hold the work array.
	info->max_work = info->inflight_target;
	info->num_sequence = 1024;

	info->work = calloc(info->max_work, sizeof(hf_work_t));
	if (unlikely(!info->work))
		quit(1, "Failed to calloc info work in hashfast_detect_common");
	applog(LOG_INFO, "Hashfast Detect: Allocated space for %d work entries", info->inflight_target);

	return true;
}

static void hashfast_usb_initialise(struct cgpu_info *hashfast)
{
	if (hashfast->usbinfo.nodev)
		return;
	// FIXME Do necessary initialising here
}

static bool hashfast_detect_one_usb(libusb_device *dev, struct usb_find_devices *found)
{
	struct cgpu_info *hashfast;
	int baud = DEFAULT_BAUD_RATE;

	hashfast = usb_alloc_cgpu(&hashfast_drv, HASHFAST_MINER_THREADS);
	if (!hashfast)
		return false;

	if (!usb_init(hashfast, dev, found)) {
		free(hashfast->device_data);
		hashfast->device_data = NULL;
		hashfast = usb_free_cgpu(hashfast);
		return false;
	}

	hashfast->usbdev->usb_type = USB_TYPE_STD;
	hashfast->usbdev->PrefPacketSize = HASHFAST_USB_PACKETSIZE;

	hashfast_usb_initialise(hashfast);

	add_cgpu(hashfast);
	return hashfast_detect_common(hashfast, baud);
}

static void hashfast_detect(void)
{
	usb_detect(&hashfast_drv, hashfast_detect_one_usb);
}

static bool hashfast_prepare(struct thr_info __maybe_unused *thr)
{
	return true;
}

static bool hashfast_fill(struct cgpu_info __maybe_unused *hashfast)
{
	return true;
}

static int64_t hashfast_scanhash(struct thr_info __maybe_unused *thr)
{
	return 0;
}

static struct api_data *hashfast_api_stats(struct cgpu_info __maybe_unused *cgpu)
{
	return NULL;
}

static void hashfast_init(struct cgpu_info __maybe_unused *hashfast)
{
}

static void hashfast_shutdown(struct thr_info __maybe_unused *thr)
{
}

struct device_drv hashfast_drv = {
	.drv_id = DRIVER_HASHFAST,
	.dname = "Hashfast",
	.name = "HFA",
	.drv_detect = hashfast_detect,
	.thread_prepare = hashfast_prepare,
	.hash_work = hash_queued_work,
	.queue_full = hashfast_fill,
	.scanwork = hashfast_scanhash,
	.get_api_stats = hashfast_api_stats,
	.reinit_device = hashfast_init,
	.thread_shutdown = hashfast_shutdown,
};
