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

#include <rsound.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

typedef struct snd_pcm_rsound
{
   rsound_t *rd;
   snd_pcm_uframes_t last_ptr;
   snd_pcm_ioplug_t io;
} snd_pcm_rsound_t;

int rsound_stop(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   rsd_stop(rsound->rd);
   rsound->last_ptr = 0;
   return 0;
}

static snd_pcm_sframes_t rsound_write( snd_pcm_ioplug_t *io,
                  const snd_pcm_channel_area_t *areas,
                  snd_pcm_uframes_t offset,
                  snd_pcm_uframes_t size)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   const char *buf = (char*)areas->addr + (areas->first + areas->step * offset) / 8;
   size *= io->channels * 2;

   ssize_t result = rsd_write(rsound->rd, buf, size);
   if ( result <= 0 )
   {
      rsd_stop(rsound->rd);
      return -1;
   }
   return result / (io->channels * 2);
}

static int rsound_start(snd_pcm_ioplug_t *io)
{
   int rc;
   snd_pcm_rsound_t *rsound = io->private_data;
   rc = rsd_start(rsound->rd);
   if ( rc < 0 )
      return rc;

   io->poll_fd = rsound->rd->conn.socket;
   io->poll_events = POLLOUT;
   snd_pcm_ioplug_reinit_status(io);
   return 0;
}

static snd_pcm_sframes_t rsound_pointer(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   int ptr;
   
   if ( io->appl_ptr < rsound->last_ptr )
   {
      rsd_stop(rsound->rd);
      rsd_start(rsound->rd);
   }

   ptr = rsd_pointer(rsound->rd);	
   ptr = snd_pcm_bytes_to_frames( io->pcm, ptr );

   // Warning, dirty as fuck hack!
   ptr = io->appl_ptr - ptr;
   rsound->last_ptr = io->appl_ptr;

   return ptr;
}

static int rsound_close(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   rsd_free(rsound->rd);
   free(rsound);
   return 0;
}

static int rsound_prepare(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   return ( rsd_start(rsound->rd));
}

static int rsound_hw_constraint(snd_pcm_rsound_t *rsound)
{
	snd_pcm_ioplug_t *io = &rsound->io;
	static const snd_pcm_access_t access_list[] = { SND_PCM_ACCESS_RW_INTERLEAVED };

	static const unsigned int formats[] = { SND_PCM_FORMAT_S16_LE };

	int err;
	
   if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, 1, formats)) < 0 )
		goto const_err;
	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, 1, access_list)) < 0)
		goto const_err;
   if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 8)) < 0)
      goto const_err;
	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE, 8000, 96000)) < 0 )
		goto const_err;
	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1 << 13, 1 << 26)) < 0)
		goto const_err;
   if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 1 << 8, 1 << 13)) < 0 )
		goto const_err;
	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 1, 1024)) < 0)
		goto const_err;

	return 0;
const_err:
   return err;
}

static int rsound_hw_params(snd_pcm_ioplug_t *io,
               snd_pcm_hw_params_t *params)
{
	snd_pcm_rsound_t *rsound = io->private_data;

	if ( io->format != SND_PCM_FORMAT_S16_LE )
	{
		return -EINVAL;
	}
	if ( io->stream != SND_PCM_STREAM_PLAYBACK )
	{
		return -EINVAL;
	}

	int rate = io->rate;
	int channels = io->channels;
   snd_pcm_uframes_t buffersize;
   rsd_set_param(rsound->rd, RSD_SAMPLERATE, &rate);
   rsd_set_param(rsound->rd, RSD_CHANNELS, &channels);
	
   int err;
   
   if ((err = snd_pcm_hw_params_get_buffer_size(params, &buffersize) < 0))
	{
      return err;
	}
   buffersize *= io->channels * 2;
   int bufsiz = (int)buffersize;
   rsd_set_param(rsound->rd, RSD_BUFSIZE, &bufsiz);

   return 0;
}

static int rsound_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
   snd_pcm_rsound_t *rsound = io->private_data;

   int ptr = rsd_delay(rsound->rd);
   if ( ptr < 0 )
      ptr = 0;
   
   *delayp = snd_pcm_bytes_to_frames(io->pcm, ptr);

   return 0;
}

static int rsound_pause(snd_pcm_ioplug_t *io, int enable)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   if ( enable )
      return rsd_stop(rsound->rd);
   else
      return rsd_start(rsound->rd);
}

static const snd_pcm_ioplug_callback_t rsound_playback_callback = {
	.start = rsound_start,
	.stop = rsound_stop,
	.transfer = rsound_write,
	.pointer = rsound_pointer,
	.close = rsound_close,
   .delay = rsound_delay,
	.hw_params = rsound_hw_params,
	.prepare = rsound_prepare,
   .pause = rsound_pause
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
   if ( rsd_init(&rsound->rd) < 0 )
   {
      SNDERR("Cannot allocate");
      free(rsound);
      return -ENOMEM;
   }


	rsd_set_param(rsound->rd, RSD_HOST, (void*)host);
	if ( !rsound->rd->host )
	{
		SNDERR("Cannot allocate");
		free(rsound);
		return -ENOMEM;
	}
   
   rsd_set_param(rsound->rd, RSD_PORT, (void*)port);
	if ( !rsound->rd->port )
	{
		SNDERR("Cannot allocate");
		free(rsound);
		return -ENOMEM;
	}

   rsound->last_ptr = 0;
	
	rsound->io.version = SND_PCM_IOPLUG_VERSION;
	rsound->io.name = "ALSA <-> RSound output plugin";
	rsound->io.mmap_rw = 0;
	rsound->io.callback = &rsound_playback_callback;
	rsound->io.private_data = rsound;

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
   rsd_free(rsound->rd);
	free(rsound);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(rsound);


