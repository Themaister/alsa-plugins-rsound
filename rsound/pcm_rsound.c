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

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

typedef struct snd_pcm_rsound {
   snd_pcm_ioplug_t io;
   int socket;
   char *host;
   char *port;
   char *buffer;
	int buffer_pointer;
   size_t chunk_size;
   size_t buffer_size;
   unsigned int bytes_per_frame;

   uint64_t total_written;
   struct timespec start_tv;
   int has_written;
   int bytes_in_buffer;

   int ready_for_data;

	uint32_t rate;
	uint16_t channels;
   snd_pcm_uframes_t alsa_buffer_size;
   snd_pcm_uframes_t alsa_fragsize;
} snd_pcm_rsound_t;

static inline int is_little_endian(void)
{
	uint16_t i = 1;
	return *((uint8_t*)&i);
}

static inline void swap_endian_16 ( uint16_t * x )
{
	*x = (*x>>8) | (*x<<8);
}

static inline void swap_endian_32 ( uint32_t * x )
{
	*x = 	(*x >> 24 ) |
			((*x<<8) & 0x00FF0000) |
			((*x>>8) & 0x0000FF00) |
			(*x << 24);
}

static int connect_server( snd_pcm_rsound_t *rd )
{
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof( hints ));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
   
   getaddrinfo(rd->host, rd->port, &hints, &res);

	rd->socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

	if ( connect(rd->socket, res->ai_addr, res->ai_addrlen) != 0 )
	{
		return 0;
	}

	freeaddrinfo(res);
	return 1;
}

static int send_header_info(snd_pcm_rsound_t *rd)
{
#define HEADER_SIZE 44
	char buffer[HEADER_SIZE] = {0};
	int rc = 0;

#define RATE 24
#define CHANNEL 22
#define FRAMESIZE 34

	uint32_t sample_rate_temp = rd->rate;
	uint16_t channels_temp = rd->channels;
	uint16_t framesize_temp = 16;

	if ( !is_little_endian() )
	{
		swap_endian_32(&sample_rate_temp);
		swap_endian_16(&channels_temp);
		swap_endian_16(&framesize_temp);
	}

	*((uint32_t*)(buffer+RATE)) = sample_rate_temp;
	*((uint16_t*)(buffer+CHANNEL)) = channels_temp;
	*((uint16_t*)(buffer+FRAMESIZE)) = framesize_temp;
	rc = send ( rd->socket, buffer, HEADER_SIZE, 0);
	if ( rc != HEADER_SIZE )
	{
		close(rd->socket);
		return 0;
	}

	return 1;
}

static int get_backend_info ( snd_pcm_rsound_t *rd )
{
	uint32_t chunk_size_temp, buffer_size_temp;
	int rc;

	rc = recv(rd->socket, &chunk_size_temp, sizeof(uint32_t), 0);
	if ( rc != sizeof(uint32_t))
	{
		close(rd->socket);
		return 0;
	}
	rc = recv(rd->socket, &buffer_size_temp, sizeof(uint32_t), 0);
	if ( rc != sizeof(uint32_t))
   {
		close(rd->socket);
		return 0;
	}

	chunk_size_temp = ntohl(chunk_size_temp);
	buffer_size_temp = ntohl(buffer_size_temp);

	int socket_buffer_size = (int)chunk_size_temp * 4;
	if ( setsockopt(rd->socket, SOL_SOCKET, SO_SNDBUF, &socket_buffer_size, sizeof(int)) == -1 )
	{
		return 0;
	}

	rd->chunk_size = chunk_size_temp;
	rd->buffer_size = buffer_size_temp;

	rd->buffer = malloc ( rd->buffer_size );
	rd->buffer_pointer = 0;

	return 1;
}

static int rsound_stop(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rd = io->private_data;
   
   
   close(rd->socket);
   rd->socket = -1;
   rd->has_written = 0;
   rd->ready_for_data = 0;
   rd->buffer_pointer = 0;
   rd->has_written = 0;
   rd->total_written = 0;

   return 0;
}

static int create_connection(snd_pcm_rsound_t *rd)
{
	int rc;

   if ( rd->socket < 0 )
   {
      rc = connect_server(rd);
      rd->io.poll_fd = rd->socket;
      snd_pcm_ioplug_reinit_status(&rd->io);
      if (!rc)
      {
         close(rd->socket);
         rd->socket = -1;
         return 0;
      }
   }
   if ( !rd->ready_for_data )
   {
      rc = send_header_info(rd);
      if (!rc)
      {
         rsound_stop(&rd->io);
         return 0;
      }

      rc = get_backend_info(rd);
      if (!rc)
      {
         rsound_stop(&rd->io);
         return 0;
      }

      rd->ready_for_data = 1;
   }
	
   return 1;
}

static int send_chunk(snd_pcm_rsound_t *rd)
{
	int rc;
	rc = send(rd->socket, rd->buffer, rd->chunk_size, 0);
	if ( rc <= 0 )
		return 0;

   memmove(rd->buffer, rd->buffer + rd->chunk_size, rd->buffer_size - rd->chunk_size);
   rd->buffer_pointer -= (int)rd->chunk_size;
   
	
	if ( !rd->has_written )
	{
		clock_gettime(CLOCK_MONOTONIC, &rd->start_tv);
		rd->has_written = 1;
	}
	rd->total_written += rc;
	return rc;
}

static void drain(snd_pcm_rsound_t *rd)
{
	if ( rd->has_written )
	{
		int64_t temp, temp2;

		struct timespec now_tv;
		clock_gettime(CLOCK_MONOTONIC, &now_tv);
		
		temp = (int64_t)now_tv.tv_sec - (int64_t)rd->start_tv.tv_sec;
		temp *= rd->rate * rd->bytes_per_frame;

		temp2 = (int64_t)now_tv.tv_nsec - (int64_t)rd->start_tv.tv_nsec;
		temp2 *= rd->rate * rd->bytes_per_frame;
		temp2 /= 1000000000;
		temp += temp2;

		rd->bytes_in_buffer = (int)((int64_t)rd->total_written + (int64_t)rd->buffer_pointer - temp);
   }
	else
		rd->bytes_in_buffer = rd->buffer_pointer;
}

static int fill_buffer(snd_pcm_rsound_t *rd, const char *buf, size_t size)
{
	int rc;
   int wrote = 0;

// Makes sure we have enough space
   while ( rd->buffer_pointer + (int)size > (int)rd->buffer_size )
	{
      wrote = 1;
		rc = send_chunk(rd);
		if ( rc <= 0 )
			return 0;
	}

   if ( !wrote && (rd->buffer_pointer >= (int)rd->chunk_size) )
   {
      rc = send_chunk(rd);
      if ( rc <= 0 )
         return 0;
   }

   memcpy(rd->buffer + rd->buffer_pointer, buf, size);
   rd->buffer_pointer += (int)size;

   return size;
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

#if 0

   int count;
   short *temp;
   int64_t sum = 0;
   float db;
   for ( count = 0; count < (int)size; count+=rsound->bytes_per_frame )
   {
      temp = (short*)&buf[count];
      sum += (int)(*temp) * (int)(*temp);
   }
   sum /= size/rsound->bytes_per_frame;
   db = sqrtf((float)sum);
   db = 20.0*log10(db / ((float)0xFFFF / 2.0));
   
   int distance = (int)db + 40;
   if (distance < 0)
      distance = 0;

   fputc('\r', stderr);
   fputc('[', stderr);
   for ( count = 0; count < distance; count++ )
      fputc('*', stderr);
   for ( count = 0; count < 40 - distance ; count++ )
      fputc(' ', stderr);
   fputc(']', stderr);
   fprintf(stderr, " [[ %7.2f dB ]]", db);

#endif

   ssize_t result;
   result = fill_buffer(rsound, buf, size);
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
   /* This might be very wrong! */
   
   ptr = (int)(rsound->total_written + rsound->buffer_pointer);
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
   {
      free(rsound);
   }
	return 0;
}

static int rsound_drain(snd_pcm_ioplug_t *io)
{
   (void) io;
   return 0;
}

static int rsound_prepare(snd_pcm_ioplug_t *io)
{
	snd_pcm_rsound_t *rsound = io->private_data;
   int rc;
   if ( (rc = create_connection(rsound)) == 1 )
   {
      return 0;
   }
   else
   {
      return -1;
   }
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

   int err;
   if ((err = snd_pcm_hw_params_get_buffer_size(params, &rsound->alsa_buffer_size)) < 0)
      return err;
   if ((err = snd_pcm_hw_params_get_period_size(params, &rsound->alsa_fragsize, NULL) < 0))
      return err;

   rsound->alsa_fragsize *= rsound->bytes_per_frame;
   rsound->alsa_buffer_size *= rsound->bytes_per_frame;

	rsound->rate = io->rate;
	rsound->channels = io->channels;
   return 0;
}

#define ARRAY_SIZE(ary)	(sizeof(ary)/sizeof(ary[0]))

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
   
   // Appearantly, if alsa tries to play something that's larger than chunk_size, there's no sound :S Weird bug. 	
	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 64, 256*32)) < 0)
		goto const_err;
	
   if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 64, 256 )) < 0 )
		goto const_err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 1, 32)) < 0)
		goto const_err;

	return 0;
const_err:
   return err;
}

static int rsound_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
   snd_pcm_rsound_t *rd = io->private_data;
   drain(rd);

   if ( rd->bytes_in_buffer < 0 )
      rd->bytes_in_buffer = 0;
   
   *delayp = snd_pcm_bytes_to_frames(io->pcm, rd->bytes_in_buffer);

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
	.prepare = rsound_prepare,
	.drain = rsound_drain
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
//   rsound->io.poll_fd = 1;
   rsound->io.poll_events = POLLOUT;
//   rsound->io.poll_events = 0;
	rsound->io.callback = &rsound_playback_callback;
	rsound->io.private_data = rsound;

	rsound->has_written = 0;
	rsound->buffer_pointer = 0;
   rsound->ready_for_data = 0;

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


