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

typedef struct snd_pcm_oss {
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

static int create_connection(snd_pcm_rsound_t *rd)
{
	int rc;

   if ( rd->socket < 0 )
   {
      rc = connect_server(rd);
      if (!rc)
         return 0;
   }
   if ( !rd->ready_for_data )
   {
      rc = send_header_info(rd);
      if (!rc)
      {
         return 0;
      }

      rc = get_backend_info(rd);
      if (!rc)
      {
         return 0;
      }

      rd->io.poll_fd = rd->socket;
      snd_pcm_ioplug_reinit_status(&rd->io);
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

   memmove(rd->buffer, rd->buffer + rd->chunk_size, rd->chunk_size);
   rd->buffer_pointer -= rd->chunk_size;
   
	
	if ( !rd->has_written )
	{
		clock_gettime(CLOCK_MONOTONIC, &rd->start_tv);
		rd->has_written = 1;
	}
	rd->total_written += (uint64_t)rc;
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

		rd->bytes_in_buffer = (int)(rd->total_written - temp);
      /*if ( rd->bytes_in_buffer <= 0 )
      {
        // rd->total_written -= rd->bytes_in_buffer;
			rd->bytes_in_buffer = 0;
      }*/
	}
	else
		rd->bytes_in_buffer = 0;
}

static int fill_buffer(snd_pcm_rsound_t *rd, char *buf, size_t size)
{
	int rc;
   char *temp_buffer = NULL;

// Fills up buffer if it's big enough.
   if ( rd->buffer_pointer + size <= rd->buffer_size )
	{
		memcpy(rd->buffer + rd->buffer_pointer, buf, size);
		rd->buffer_pointer += size;
	}

// Oh, shit, we need to somehow hack around this unfortunate situation :\ Damn you apps that try to play so much data :D.
// Might be really buggy :<
	else
	{


      temp_buffer = malloc(size);
      if (!temp_buffer)
		   return 0;

      memcpy(temp_buffer, buf, size);
      int size_left = rd->buffer_size - rd->buffer_pointer;
      memcpy(rd->buffer + rd->buffer_pointer, temp_buffer, size_left);
      rd->buffer_pointer += size_left;
      int temp_buf_ptr = size_left;

      while ( temp_buf_ptr != (int)size  )
      {
         while ( rd->buffer_pointer >= (int)rd->chunk_size )
         {
            rc = send_chunk(rd);
            if ( rc <= 0 )
            {
               free(temp_buffer);
               return 0;
            }
         }
         
         size_left = rd->buffer_size - rd->buffer_pointer;
         if ( temp_buf_ptr + size_left >= (int)size ) // We've managed to clear our buffer
         {
            memcpy(rd->buffer + rd->buffer_pointer, temp_buffer + temp_buf_ptr, size - temp_buf_ptr);
            rd->buffer_pointer += size - temp_buf_ptr;
            temp_buf_ptr = (int)size;
         }
         else
         {
            memcpy(rd->buffer + rd->buffer_pointer, temp_buffer + temp_buf_ptr, size_left);
            rd->buffer_pointer += size_left;
            temp_buf_ptr += size_left;
         }

      }


	}

// Empties the buffer as much as possible.
   while ( rd->buffer_pointer >= (int)rd->chunk_size )
	{
		rc = send_chunk(rd);
		if ( rc <= 0 )
			break;
	}
	
   if ( temp_buffer != NULL )
      free ( temp_buffer );
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
   ssize_t result;

   buf = (char*)areas->addr + (areas->first + areas->step * offset) / 8;
   result = fill_buffer(rsound, (char*)buf, size);
   if ( result <= 0 )
   {
      return -1;
   }
   return result / rsound->bytes_per_frame;
}

static snd_pcm_sframes_t rsound_pointer(snd_pcm_ioplug_t *io)
{
   snd_pcm_rsound_t *rsound = io->private_data;
   int ptr;
   /* This might be very wrong! */
   
   
//   ptr = (int)rsound->buffer_pointer;
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
               snd_pcm_hw_params_t *params ATTRIBUTE_UNUSED)
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
   return 0;
}

#define ARRAY_SIZE(ary)	(sizeof(ary)/sizeof(ary[0]))

static int rsound_hw_constraint(snd_pcm_rsound_t *rsound)
{
	snd_pcm_ioplug_t *io = &rsound->io;
	static const snd_pcm_access_t access_list[] = { SND_PCM_ACCESS_RW_INTERLEAVED };

	static const unsigned int formats[] = { SND_PCM_FORMAT_S16_LE };
/*   static const unsigned int bytes_list[] = {
      1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5, 1 << 6, 1 << 7, 1 << 8 };
  */ 

	int err;
	
   if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, ARRAY_SIZE(formats), formats)) < 0 )
		goto const_err;

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, ARRAY_SIZE(access_list), access_list)) < 0)
		goto const_err;


   if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 8)) < 0)
      goto const_err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE, 8000, 96000)) < 0 )
		goto const_err;
	
	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 32*2, 64*8)) < 0)
		goto const_err;
	
   if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 32, 64 )) < 0 )
		goto const_err;

   /*if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, ARRAY_SIZE(bytes_list), bytes_list) < 0))
      goto const_err;

   if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, ARRAY_SIZE(bytes_list), bytes_list) < 0))
      goto const_err;*/

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 2, 8)) < 0)
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
   
   *delayp = snd_pcm_bytes_to_frames(io->pcm, rd->bytes_in_buffer + rd->buffer_pointer);

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
   rsound->io.poll_events = POLLOUT;
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


