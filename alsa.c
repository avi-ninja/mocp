/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* Based on aplay copyright (c) by Jaroslav Kysela <perex@suse.cz> */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <inttypes.h>
#include <alsa/asoundlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define DEBUG

#include "common.h"
#include "server.h"
#include "audio.h"
#include "options.h"
#include "log.h"

#define BUFFER_MAX_USEC	300000

static snd_pcm_t *handle = NULL;

static struct
{
	unsigned int channels;
	unsigned int rate;
	snd_pcm_format_t format;
} params = { 0, 0, SND_PCM_FORMAT_UNKNOWN };

static snd_pcm_uframes_t buffer_frames;
static snd_pcm_uframes_t chunk_frames;
static int chunk_bytes = -1;
static char alsa_buf[512 * 1024];
static int alsa_buf_fill = 0;
static int bytes_per_frame;

static snd_mixer_t *mixer_handle = NULL;
static snd_mixer_elem_t *mixer_elem1 = NULL;
static snd_mixer_elem_t *mixer_elem2 = NULL;
static snd_mixer_elem_t *mixer_elem_curr = NULL;

/* Percentage volume setting for first and second mixer. */
static int volume1 = -1;
static int volume2 = -1;

/* Map ALSA's mask to MOC's format (and visa versa). */
static const struct {
	snd_pcm_format_t mask;
	long format;
} format_masks[] = {
	{SND_PCM_FORMAT_S8, SFMT_S8},
	{SND_PCM_FORMAT_U8, SFMT_U8},
	{SND_PCM_FORMAT_S16, SFMT_S16},
	{SND_PCM_FORMAT_U16, SFMT_U16},
	{SND_PCM_FORMAT_S32, SFMT_S32},
	{SND_PCM_FORMAT_U32, SFMT_U32}
};

/* Given an ALSA mask, return a MOC format or zero if unknown. */
static inline long mask_to_format (const snd_pcm_format_mask_t *mask)
{
	long result = 0;

	for (size_t ix = 0; ix < ARRAY_SIZE(format_masks); ix += 1) {
		if (snd_pcm_format_mask_test (mask, format_masks[ix].mask))
			result |= format_masks[ix].format;
	}

#if 0
	if (snd_pcm_format_mask_test (mask, SND_PCM_FORMAT_S24))
		result |= SFMT_S32; /* conversion needed */
#endif

	return result;
}

/* Given a MOC format, return an ALSA mask.
 * Return SND_PCM_FORMAT_UNKNOWN if unknown. */
static inline snd_pcm_format_t format_to_mask (long format)
{
	snd_pcm_format_t result = SND_PCM_FORMAT_UNKNOWN;

	for (size_t ix = 0; ix < ARRAY_SIZE(format_masks); ix += 1) {
		if (format_masks[ix].format == format) {
			result = format_masks[ix].mask;
			break;
		}
	}

	return result;
}

#ifndef NDEBUG
static void alsa_log_cb (const char *unused1 ATTR_UNUSED,
                         int unused2 ATTR_UNUSED,
                         const char *unused3 ATTR_UNUSED,
                         int unused4 ATTR_UNUSED, const char *fmt, ...)
{
	char *msg;
	va_list va;

	assert (fmt);

	va_start (va, fmt);
	msg = format_msg_va (fmt, va);
	va_end (va);

	logit ("ALSA said: %s", msg);
	free (msg);
}
#endif

static void alsa_shutdown ()
{
	int err;

	if (mixer_handle && (err = snd_mixer_close(mixer_handle)) < 0)
		logit ("Can't close mixer: %s", snd_strerror(err));

#ifndef NDEBUG
	snd_lib_error_set_handler (NULL);
#endif
}

static snd_pcm_hw_params_t *alsa_open_device (const char *device)
{
	snd_pcm_hw_params_t *result;
	int rc;

	rc = snd_pcm_open (&handle, device, SND_PCM_STREAM_PLAYBACK,
	                                    SND_PCM_NONBLOCK);
	if (rc < 0) {
		error ("Can't open audio: %s", snd_strerror (rc));
		return NULL;
	}

	rc = snd_pcm_hw_params_malloc (&result);
	if (rc < 0) {
		error ("Can't allocate alsa hardware parameters structure: %s",
		        snd_strerror (rc));
		snd_pcm_close (handle);
		return NULL;
	}

	rc = snd_pcm_hw_params_any (handle, result);
	if (rc < 0) {
		error ("Can't initialize hardware parameters structure: %s",
		        snd_strerror (rc));
		snd_pcm_hw_params_free (result);
		snd_pcm_close (handle);
		return NULL;
	}

	return result;
}

/* Fill caps with the device capabilities. Return 0 on error. */
static int fill_capabilities (struct output_driver_caps *caps)
{
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_format_mask_t *format_mask;
	int err;
	unsigned int val;

	hw_params = alsa_open_device (options_get_str ("ALSADevice"));
	if (!hw_params)
		return 0;

	if ((err = snd_pcm_hw_params_get_channels_min (hw_params, &val)) < 0) {
		error ("Can't get the minimum number of channels: %s",
				snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		snd_pcm_close (handle);
		return 0;
	}
	caps->min_channels = val;

	if ((err = snd_pcm_hw_params_get_channels_max (hw_params, &val)) < 0) {
		error ("Can't get the maximum number of channels: %s",
				snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		snd_pcm_close (handle);
		return 0;
	}
	caps->max_channels = val;

	if ((err = snd_pcm_format_mask_malloc(&format_mask)) < 0) {
		error ("Can't allocate format mask: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		snd_pcm_close (handle);
		return 0;
	}
	snd_pcm_hw_params_get_format_mask (hw_params, format_mask);
	caps->formats = mask_to_format (format_mask) | SFMT_NE;

	snd_pcm_format_mask_free (format_mask);
	snd_pcm_hw_params_free (hw_params);
	snd_pcm_close (handle);
	handle = NULL;

	return 1;
}

static void handle_mixer_events (snd_mixer_t *mixer_handle)
{
	int count;

	if ((count = snd_mixer_poll_descriptors_count(mixer_handle)) < 0)
		logit ("snd_mixer_poll_descriptors_count() failed: %s",
				snd_strerror(count));
	else {
		struct pollfd *fds;
		int rc;

		fds = xcalloc (count, sizeof(struct pollfd));

		if ((rc = snd_mixer_poll_descriptors(mixer_handle, fds,
						count)) < 0)
			logit ("snd_mixer_poll_descriptors() failed: %s",
					snd_strerror(rc));
		else {
			rc = poll (fds, count, 0);
			if (rc < 0)
				error_errno ("poll() failed", errno);
			else if (rc > 0) {
				debug ("Mixer event");
				if ((rc = snd_mixer_handle_events(mixer_handle)) < 0)
					logit ("snd_mixer_handle_events() failed: %s",
							snd_strerror(rc));
			}
		}

		free (fds);
	}
}

static int alsa_read_mixer_raw (snd_mixer_elem_t *elem)
{
	int rc, nchannels = 0, volume = 0;
	bool joined;
	snd_mixer_selem_channel_id_t chan_id;

	if (!mixer_handle)
		return -1;

	assert (elem);

	handle_mixer_events (mixer_handle);

	joined = snd_mixer_selem_has_playback_volume_joined (elem);

	for (chan_id = 0; chan_id < SND_MIXER_SCHN_LAST; chan_id += 1) {
		if (snd_mixer_selem_has_playback_channel (elem, chan_id)) {
			long vol;

			nchannels += 1;
			rc = snd_mixer_selem_get_playback_volume (elem, chan_id, &vol);
			if (rc < 0) {
				error ("Can't read mixer: %s", snd_strerror (rc));
				return -1;
			}

			assert (RANGE(0, vol, 100));

#if 0
			{
				static int prev_vol[SND_MIXER_SCHN_LAST] = {0};

				if (vol != prev_vol[chan_id]) {
					prev_vol[chan_id] = vol;
					debug ("Vol %d: %ld", chan_id, vol);
				}
			}
#endif

			volume += vol;
		}

		if (joined)
			break;
	}

	if (nchannels == 0) {
		logit ("Mixer has no channels");
		return -1;
	}

	volume /= nchannels;

	return volume;
}

static snd_mixer_elem_t *alsa_init_mixer_channel (const char *name)
{
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t *elem = NULL;

	snd_mixer_selem_id_malloc (&sid);
	snd_mixer_selem_id_set_index (sid, 0);
	snd_mixer_selem_id_set_name (sid, name);

	if (!(elem = snd_mixer_find_selem(mixer_handle, sid)))
		error ("Can't find mixer %s", name);
	else if (!snd_mixer_selem_has_playback_volume(elem)) {
		error ("Mixer device has no playback volume (%s).", name);
		elem = NULL;
	}
	else if (snd_mixer_selem_set_playback_volume_range (elem, 0, 100) < 0) {
		error ("Cannot set playback volume range (%s).", name);
		elem = NULL;
	}
	else
		logit ("Opened mixer (%s)", name);

	snd_mixer_selem_id_free (sid);

	return elem;
}

static int alsa_init (struct output_driver_caps *caps)
{
	int err;
	const char *device;

	device = options_get_str ("ALSADevice");
	logit ("Initialising ALSA device: %s", device);

#ifndef NDEBUG
	snd_lib_error_set_handler (alsa_log_cb);
#endif

	if ((err = snd_mixer_open(&mixer_handle, 0)) < 0) {
		error ("Can't open ALSA mixer: %s", snd_strerror(err));
		mixer_handle = NULL;
	}
	else if ((err = snd_mixer_attach(mixer_handle, device)) < 0) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Can't attach mixer: %s", snd_strerror(err));
	}
	else if ((err = snd_mixer_selem_register(mixer_handle, NULL, NULL))
			< 0) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Can't register mixer: %s", snd_strerror(err));
	}
	else if ((err = snd_mixer_load(mixer_handle)) < 0) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Can't load mixer: %s", snd_strerror(err));
	}

	if (mixer_handle) {
		mixer_elem1 = alsa_init_mixer_channel (options_get_str ("ALSAMixer1"));
		mixer_elem2 = alsa_init_mixer_channel (options_get_str ("ALSAMixer2"));
	}

	mixer_elem_curr = mixer_elem1 ? mixer_elem1 : mixer_elem2;

	if (mixer_elem_curr) {
		int vol;

		if (mixer_elem1 && (vol = alsa_read_mixer_raw (mixer_elem1)) != -1) {
			assert (RANGE(0, vol, 100));
			volume1 = vol;
		}
		else {
			mixer_elem1 = NULL;
			mixer_elem_curr = mixer_elem2;
		}

		if (mixer_elem2 && (vol = alsa_read_mixer_raw (mixer_elem2)) != -1) {
			assert (RANGE(0, vol, 100));
			volume2 = vol;
		}
		else {
			mixer_elem2 = NULL;
			mixer_elem_curr = mixer_elem1;
		}

		if (!mixer_elem_curr) {
			snd_mixer_close (mixer_handle);
			mixer_handle = NULL;
		}
	}
	else if (mixer_handle) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
	}

	return fill_capabilities (caps);
}

static int alsa_open (struct sound_params *sound_params)
{
	snd_pcm_hw_params_t *hw_params;
	int err;
	unsigned int period_time;
	unsigned int buffer_time;
	char fmt_name[128];
	const char *device;

	params.format = format_to_mask (sound_params->fmt & SFMT_MASK_FORMAT);
	if (params.format == SND_PCM_FORMAT_UNKNOWN) {
		error ("Unknown sample format: %s",
		        sfmt_str (sound_params->fmt, fmt_name, sizeof (fmt_name)));
		return 0;
	}

	device = options_get_str ("ALSADevice");
	logit ("Opening ALSA device: %s", device);

	hw_params = alsa_open_device (device);
	if (!hw_params)
		return 0;

	if ((err = snd_pcm_hw_params_set_access (handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		error ("Can't set alsa access type: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params_set_format (handle, hw_params,
					params.format)) < 0) {
		error ("Can't set sample format: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	logit ("Set sample width: %d bytes", sfmt_Bps (sound_params->fmt));

	params.rate = sound_params->rate;
	if ((err = snd_pcm_hw_params_set_rate_near (handle, hw_params,
					&params.rate, 0)) < 0) {
		error ("Can't set sample rate: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	logit ("Set rate: %uHz", params.rate);

	if ((err = snd_pcm_hw_params_set_channels (handle, hw_params,
					sound_params->channels)) < 0) {
		error ("Can't set number of channels: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	logit ("Set channels: %d", sound_params->channels);

	if ((err = snd_pcm_hw_params_get_buffer_time_max(hw_params,
					&buffer_time, 0)) < 0) {
		error ("Can't get maximum buffer time: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if (buffer_time > BUFFER_MAX_USEC)
		buffer_time = BUFFER_MAX_USEC;
	period_time = buffer_time / 4;

	if ((err = snd_pcm_hw_params_set_period_time_near(handle, hw_params,
					&period_time, 0)) < 0) {
		error ("Can't set period time: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params_set_buffer_time_near(handle, hw_params,
					&buffer_time, 0)) < 0) {
		error ("Can't set buffer time: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params (handle, hw_params)) < 0) {
		error ("Can't set audio parameters: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	snd_pcm_hw_params_get_period_size (hw_params, &chunk_frames, 0);
	debug ("Chunk size: %lu frames", chunk_frames);

	snd_pcm_hw_params_get_buffer_size (hw_params, &buffer_frames);
	debug ("Buffer size: %lu frames", buffer_frames);
	debug ("Buffer time: %"PRIu64"us",
	        (uint64_t) buffer_frames * UINT64_C(1000000) / params.rate);

	bytes_per_frame = sound_params->channels * sfmt_Bps(sound_params->fmt);
	debug ("Frame size: %d bytes", bytes_per_frame);

	chunk_bytes = chunk_frames * bytes_per_frame;

	if (chunk_frames == buffer_frames) {
		error ("Can't use period equal to buffer size (%lu == %lu)",
				chunk_frames, buffer_frames);
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	snd_pcm_hw_params_free (hw_params);

	if ((err = snd_pcm_prepare(handle)) < 0) {
		error ("Can't prepare audio interface for use: %s",
				snd_strerror(err));
		return 0;
	}

	logit ("ALSA device opened");

	params.channels = sound_params->channels;
	alsa_buf_fill = 0;
	return 1;
}

/* Play from alsa_buf as many chunks as possible. Move the remaining data
 * to the beginning of the buffer. Return the number of bytes written
 * or -1 on error. */
static int play_buf_chunks ()
{
	int written = 0;
	bool zero_logged = false;

	while (alsa_buf_fill >= chunk_bytes) {
		int rc;

		rc = snd_pcm_writei (handle, alsa_buf + written,
				chunk_bytes / bytes_per_frame);

		if (rc == 0) {
			if (!zero_logged) {
				debug ("Played 0 bytes");
				zero_logged = true;
			}
			continue;
		}

		zero_logged = false;

		if (rc > 0) {
			int written_bytes = rc * bytes_per_frame;

			written += written_bytes;
			alsa_buf_fill -= written_bytes;

			debug ("Played %d bytes", written_bytes);
			continue;
		}

		rc = snd_pcm_recover (handle, rc, 0);

		switch (rc) {
		case 0:
			break;
		case -EAGAIN:
			if (snd_pcm_wait (handle, 500) < 0)
				logit ("snd_pcm_wait() failed");
			break;
		default:
			error ("Can't play: %s", snd_strerror (rc));
			return -1;
		}
	}

	debug ("%d bytes remain in alsa_buf", alsa_buf_fill);
	memmove (alsa_buf, alsa_buf + written, alsa_buf_fill);

	return written;
}

static void alsa_close ()
{
	snd_pcm_sframes_t delay;

	assert (handle != NULL);

	/* play what remained in the buffer */
	if (alsa_buf_fill) {
		assert (alsa_buf_fill < chunk_bytes);

		snd_pcm_format_set_silence (params.format,
				alsa_buf + alsa_buf_fill,
				(chunk_bytes - alsa_buf_fill) / bytes_per_frame
				* params.channels);
		alsa_buf_fill = chunk_bytes;
		play_buf_chunks ();
	}

	/* Wait for ALSA buffers to empty.
	 * Do not be tempted to use snd_pcm_nonblock() and snd_pcm_drain()
	 * here; there are two bugs in ALSA which make it a bad idea (see
	 * the SVN commit log for r2550).  Instead we sleep for the duration
	 * of the still unplayed samples. */
	if (snd_pcm_delay (handle, &delay) == 0)
		xsleep (delay, params.rate);
	snd_pcm_close (handle);
	logit ("ALSA device closed");

	params.format = 0;
	params.rate = 0;
	params.channels = 0;
	buffer_frames = 0;
	chunk_frames = 0;
	chunk_bytes = -1;
	handle = NULL;
}

static int alsa_play (const char *buff, const size_t size)
{
	int to_write = size;
	int buf_pos = 0;

	assert (chunk_bytes > 0);

	debug ("Got %zu bytes to play", size);

	while (to_write) {
		int to_copy;

		to_copy = MIN(to_write, ssizeof(alsa_buf) - alsa_buf_fill);
		memcpy (alsa_buf + alsa_buf_fill, buff + buf_pos, to_copy);
		to_write -= to_copy;
		buf_pos += to_copy;
		alsa_buf_fill += to_copy;

		debug ("Copied %d bytes to alsa_buf (now filled with %d bytes)",
				to_copy, alsa_buf_fill);

		if (play_buf_chunks() < 0)
			return -1;
	}

	debug ("Played everything");

	return size;
}

static int alsa_read_mixer ()
{
	int actual_vol, *vol;

	actual_vol = alsa_read_mixer_raw (mixer_elem_curr);

	assert (RANGE(0, actual_vol, 100));

	if (mixer_elem_curr == mixer_elem1)
		vol = &volume1;
	else
		vol = &volume2;

	if (*vol != actual_vol) {
		*vol = actual_vol;
		logit ("Mixer volume has changed since we last read it.");
	}

	return actual_vol;
}

static void alsa_set_mixer (int vol)
{
	assert (RANGE(0, vol, 100));

	if (mixer_handle) {
		int rc;

		if (mixer_elem_curr == mixer_elem1)
			volume1 = vol;
		else
			volume2 = vol;

		debug ("Setting vol to %d", vol);

		rc = snd_mixer_selem_set_playback_volume_all (mixer_elem_curr, vol);
		if (rc < 0)
			error ("Can't set mixer: %s", snd_strerror (rc));
	}
}

static int alsa_get_buff_fill ()
{
	if (handle) {
		int err;
		snd_pcm_sframes_t delay;

		if ((err = snd_pcm_delay(handle, &delay)) < 0) {
			logit ("snd_pcm_delay() failed: %s", snd_strerror(err));
			return 0;
		}

		/* delay can be negative when underrun occur */
		return MAX(delay, 0) * bytes_per_frame;
	}
	return 0;
}

static int alsa_reset ()
{
	if (handle) {
		int err;

		if ((err = snd_pcm_drop(handle)) < 0) {
			error ("Can't reset the device: %s", snd_strerror(err));
			return 0;
		}
		if ((err = snd_pcm_prepare(handle)) < 0) {
			error ("Can't prepare after reset: %s", snd_strerror(err));
			return 0;
		}

		alsa_buf_fill = 0;
	}
	else
		logit ("alsa_reset() when the device is not opened.");
	return 1;
}

static int alsa_get_rate ()
{
	return params.rate;
}

static void alsa_toggle_mixer_channel ()
{
	if (mixer_elem_curr == mixer_elem1 && mixer_elem2)
		mixer_elem_curr = mixer_elem2;
	else if (mixer_elem1)
		mixer_elem_curr = mixer_elem1;
}

static char *alsa_get_mixer_channel_name ()
{
	if (mixer_elem_curr == mixer_elem1)
		return xstrdup (options_get_str ("ALSAMixer1"));
	return xstrdup (options_get_str ("ALSAMixer2"));
}

void alsa_funcs (struct hw_funcs *funcs)
{
	funcs->init = alsa_init;
	funcs->shutdown = alsa_shutdown;
	funcs->open = alsa_open;
	funcs->close = alsa_close;
	funcs->play = alsa_play;
	funcs->read_mixer = alsa_read_mixer;
	funcs->set_mixer = alsa_set_mixer;
	funcs->get_buff_fill = alsa_get_buff_fill;
	funcs->reset = alsa_reset;
	funcs->get_rate = alsa_get_rate;
	funcs->toggle_mixer_channel = alsa_toggle_mixer_channel;
	funcs->get_mixer_channel_name = alsa_get_mixer_channel_name;
}
