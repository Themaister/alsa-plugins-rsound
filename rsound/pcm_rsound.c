/*
 * ALSA <-> RSound PCM output plugin
 *
 * Copyright (c) 2010 by Hans-Kristian Arntzen <maister@archlinux.us>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include "rsound.h"

#define ARRAY_SIZE(ary)	(sizeof(ary)/sizeof(ary[0]))

int rsound_stop(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rd = io->private_data;
   
   stop_thread(rd);
   close(rd->socket);
   rd->socket = -1;
   rd->has_written = 0;
   rd->ready_for_data = 0;
   rd->buffer_pointer = 0;
   rd->has_written = 0;
   rd->total_written = 0;

   return 0;
}

static snd_pcm_sframes_t rsound_write( snd_pcm_ioplug_t *io,
                  const snd_pcm_channel_area_t *areas,
                  snd_pcm_uframes_t offset,
                  snd_pcm_uframes_t size)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   size *= rsound->bytes_per_frame;
   const char *buf;
   buf = (char*)areas->addr + (areas->first + areas->step * offset) / 8;

   ssize_t result;
   //fprintf(stderr, "doing fill_buffer: %d bytes\n", (int)size);
   result = fill_buffer(rsound, buf, size);
   //fprintf(stderr, "ended fill_buffer: %d bytes\n", (int)size);
   if ( result <= 0 )
   {
      rsound_stop(io);
      return -1;
   }
   return result / rsound->bytes_per_frame;
}

static snd_pcm_sframes_t rsound_pointer(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   int ptr;
   
   ptr = get_ptr(rsound);	
   if ( ptr > rsound->alsa_buffer_size )
      ptr = rsound->alsa_buffer_size;
   ptr = snd_pcm_bytes_to_frames( io->pcm, ptr );
   return ptr;
}

static int rsound_start(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rsound = io->private_data;
	rsound->channels = (uint32_t)io->channels;
	rsound->rate = (uint32_t)io->rate;
   if ( create_connection(rsound) )
      return 0;
   else
      return -1;
}

static int rsound_close(snd_pcm_ioplug_t *io)
{
	snd_pcm_rsound_t *rsound = io->private_data;

   if ( rsound )
      free(rsound);
	
   return 0;
}

static int rsound_prepare(snd_pcm_ioplug_t *io)
{
	snd_pcm_rsound_t *rsound = io->private_data;
   if ( create_connection(rsound)) 
      return 0;
   else
      return -1;
}

static int rsound_hw_constraint(snd_pcm_rsound_t *rsound)
{
	snd_pcm_ioplug_t *io = &rsound->io;
	static const snd_pcm_access_t access_list[] = { SND_PCM_ACCESS_RW_INTERLEAVED };

	static const unsigned int formats[] = { SND_PCM_FORMAT_S16_LE };

	int err;
	
   if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, ARRAY_SIZE(formats), formats)) < 0 )
		goto const_err;

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, ARRAY_SIZE(access_list), access_list)) < 0)
		goto const_err;

   if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 8)) < 0)
      goto const_err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE, 8000, 96000)) < 0 )
		goto const_err;
   
	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1 << 15, 1 << 22)) < 0)
		goto const_err;
	
   if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 1 << 6, 1 << 12)) < 0 )
		goto const_err;
	
	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 4, 1024)) < 0)
		goto const_err;

	return 0;
const_err:
   return err;
}

static int rsound_hw_params(snd_pcm_ioplug_t *io,
               snd_pcm_hw_params_t *params)
{
	snd_pcm_rsound_t *rsound = io->private_data;

	rsound->bytes_per_frame = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;
	if ( io->format != SND_PCM_FORMAT_S16_LE )
	{
		return -EINVAL;
	}
	if ( io->stream != SND_PCM_STREAM_PLAYBACK )
	{
		return -EINVAL;
	}

	rsound->rate = io->rate;
	rsound->channels = io->channels;
	
   int err;
   
   if ((err = snd_pcm_hw_params_get_buffer_size(params, &rsound->alsa_buffer_size) < 0))
	{
      return err;
	}
   if ((err = snd_pcm_hw_params_get_period_size(params, &rsound->alsa_fragsize, NULL) < 0))
	{
      return err;
	}

	rsound->alsa_buffer_size *= rsound->bytes_per_frame;
   rsound->buffer_size = rsound->alsa_buffer_size;
	rsound->alsa_fragsize *= rsound->bytes_per_frame;

   //fprintf(stderr, "Buffer %d frag %d\n", (int)rsound->alsa_buffer_size, (int)rsound->alsa_fragsize);

   return 0;
}

static int rsound_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
   snd_pcm_rsound_t *rd = io->private_data;

   int ptr = get_delay(rd);
   if ( ptr < 0 )
      ptr = 0;
   
   *delayp = snd_pcm_bytes_to_frames(io->pcm, ptr);

   return 0;
}

static const snd_pcm_ioplug_callback_t rsound_playback_callback = {
	.start = rsound_start,
	.stop = rsound_stop,
	.transfer = rsound_write,
	.pointer = rsound_pointer,
	.close = rsound_close,
   .delay = rsound_delay,
	.hw_params = rsound_hw_params,
	.prepare = rsound_prepare
};

SND_PCM_PLUGIN_DEFINE_FUNC(rsound)
{
   (void) root;
	snd_config_iterator_t i, next;
	const char *host = "localhost";
	const char *port = "12345";
	int err;
	snd_pcm_rsound_t *rsound;

	snd_config_for_each(i, next, conf)
	{
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if ( snd_config_get_id(n, &id) < 0 )
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
			continue;
		if (strcmp(id, "host") == 0)
		{
			if ( snd_config_get_string(n, &host) < 0 )
			{
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "port") == 0)
		{
			if ( snd_config_get_string(n, &port) < 0 )
			{
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	rsound = calloc(1, sizeof(*rsound));
	if ( ! rsound )
	{
		SNDERR("Cannot allocate");
		return -ENOMEM;
	}


	rsound->host = strdup(host);
	if ( !rsound->host )
	{
		SNDERR("Cannot allocate");
		free(rsound);
		return -ENOMEM;
	}
	
	rsound->port = strdup(port);
	if ( !rsound->port )
	{
		SNDERR("Cannot allocate");
		free(rsound);
		return -ENOMEM;
	}

   rsound->socket = -1;
   err = connect_server(rsound);
   if ( err != 1 )
   {
      err = -EINVAL;
      goto error;
   }

	rsound->io.version = SND_PCM_IOPLUG_VERSION;
	rsound->io.name = "ALSA <-> RSound output plugin";
	rsound->io.mmap_rw = 0;
   rsound->io.poll_fd = rsound->socket;
   rsound->io.poll_events = POLLOUT;
	rsound->io.callback = &rsound_playback_callback;
	rsound->io.private_data = rsound;

	rsound->has_written = 0;
	rsound->buffer_pointer = 0;
   rsound->ready_for_data = 0;
   rsound->thread_active = 0;
   pthread_mutex_init(&rsound->thread.mutex, NULL);

	err = snd_pcm_ioplug_create(&rsound->io, name, stream, mode);
	if ( err < 0 )
		goto error;
	
	err = rsound_hw_constraint(rsound);
	if ( err != 0 )
	{
		snd_pcm_ioplug_delete(&rsound->io);
		goto error;
	}

	*pcmp = rsound->io.pcm;
   return 0;

error:
	free(rsound->host);
	free(rsound->port);
	free(rsound);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(rsound);


