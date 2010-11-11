/*
 * Copyright (C) 2010 Bluecherry, LLC
 *
 * Confidential, all rights reserved. No distribution is permitted.
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

#include "bc-server.h"

static void update_osd(struct bc_record *bc_rec)
{
	struct bc_handle *bc = bc_rec->bc;
	time_t t = time(NULL);
	char buf[20];
	struct tm tm;

	if (t == bc_rec->osd_time)
		return;

	bc_rec->osd_time = t;
	strftime(buf, 20, "%F %T", localtime_r(&t, &tm));
	bc_set_osd(bc, "%s %s", bc_rec->name, buf);
}

static void *bc_device_thread(void *data)
{
	struct bc_record *bc_rec = data;
	struct bc_handle *bc = bc_rec->bc;
	int file_started = 0;
	int ret;

	bc_log("I(%d): Starting record: %s", bc_rec->id, bc_rec->name);

	for (;;) {
		double audio_pts, video_pts;

		update_osd(bc_rec);

		if ((ret = bc_buf_get(bc)) == EAGAIN) {
			continue;
		} else if (ret == ERESTART) {
			if (file_started) {
				file_started = 0;
				bc_close_avcodec(bc_rec);
			}
			continue;
		} else if (ret) {
			bc_log("E(%d): Failed to get vid buf", bc_rec->id);
			continue;
			/* XXX Do something */
		}

		if (!file_started) {
			if (bc_open_avcodec(bc_rec)) {
				bc_log("E(%d): error opening avcodec",
				       bc_rec->id);
				continue;
			}
			file_started = 1;
		}

		if (bc_rec->audio_st) {
			AVStream *st;

			st = bc_rec->video_st;
			video_pts = (double)st->pts.val * st->time_base.num /
				st->time_base.den;

			st = bc_rec->audio_st;
			audio_pts = (double)st->pts.val * st->time_base.num /
				st->time_base.den;

			while (audio_pts < video_pts) {
				if (bc_aud_out(bc_rec)) {
					break;
					/* XXX Do something */
				}
				audio_pts = (double)st->pts.val *
						st->time_base.num /
						st->time_base.den;
			}
		}

		if (bc_vid_out(bc_rec)) {
			bc_log("E(%d): Error writing frame to outfile: %m",
			       bc_rec->id);
			/* XXX Do something */
		}
	}

	if (file_started)
		bc_close_avcodec(bc_rec);

	return NULL;
}

int bc_start_record(struct bc_record *bc_rec, char **rows, int ncols, int row)
{
	struct bc_handle *bc;
	char *motion_map;
	char *signal;
	int width, height;
	int vh;
	int ret;

	/* Open the device */
	if ((bc = bc_handle_get(bc_rec->dev)) == NULL) {
		bc_log("E(%d): error opening device: %m", bc_rec->id);
		return -1;
	}

	bc_rec->bc = bc;
	bc->__data = bc_rec;

	if (bc_rec->vid_interval > 0 && bc_set_interval(bc,
					bc_rec->vid_interval) != 0) {
		bc_log("E(%d): failed to set video interval", bc_rec->id);
		bc_handle_free(bc);
		return -1;
	}

	/* Set the format */
	width = bc_db_get_val_int(rows, ncols, row, "resolutionX");
	height = bc_db_get_val_int(rows, ncols, row, "resolutionY");

	ret = bc_set_format(bc, V4L2_PIX_FMT_MPEG, width, height);

	if (ret) {
		bc_log("E(%d): error setting format: %m", bc_rec->id);
		bc_handle_free(bc);
		return -1;
	}

	/* Set the motion threshold blocks */
	signal = bc_db_get_val(rows, ncols, row, "signal_type");
	motion_map = bc_db_get_val(rows, ncols, row, "motion_map");
	if (!strcasecmp(signal, "NTSC"))
		vh = 15;
	else
		vh = 18;
	if (strlen(motion_map) != vh * 22) {
		bc_log("W(%d): motion map is wrong length", bc_rec->id);
	} else {
		int i, ret = 0;

		for (i = 0; i < vh; i++) {
			int j;
			for (j = 0; j < 22; j++) {
				int pos = (j * 22) + i;
				int val = bc_motion_val(BC_MOTION_TYPE_SOLO,
							motion_map[pos]);

				ret |= bc_set_motion_thresh(bc, val,
						(j * 64) + i);
			}
		}

		if (ret)
			bc_log("W(%d): errors were encountered setting motion thresh",
			       bc_rec->id);
	}

	/* Check motion detection */
	ret = bc_db_get_val_bool(rows, ncols, row, "motion_detection_on");
	ret = bc_set_motion(bc, ret);
	if (ret) {
		bc_log("E(%d): error setting motion: %m", bc_rec->id);
		bc_handle_free(bc);
		return -1;
	}

	if (bc_handle_start(bc)) {
		bc_log("E(%d): error starting stream: %m", bc_rec->id);
		bc_handle_free(bc);
		return -1;
	}

	if (pthread_create(&bc_rec->thread, NULL, bc_device_thread,
			   bc_rec) != 0) {
		bc_log("E(%d): failed to start thread: %m", bc_rec->id);
		bc_handle_free(bc);
		return -1;
	}

	return 0;
}
