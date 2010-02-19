#include "rsound.h"

int is_little_endian(void)
{
	uint16_t i = 1;
	return *((uint8_t*)&i);
}

void swap_endian_16 ( uint16_t * x )
{
	*x = (*x>>8) | (*x<<8);
}

void swap_endian_32 ( uint32_t * x )
{
	*x = 	(*x >> 24 ) |
			((*x<<8) & 0x00FF0000) |
			((*x>>8) & 0x0000FF00) |
			(*x << 24);
}

int connect_server( snd_pcm_rsound_t *rd )
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

int send_header_info(snd_pcm_rsound_t *rd)
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

int get_backend_info ( snd_pcm_rsound_t *rd )
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

	rd->buffer = malloc ( rd->buffer_size );
	rd->buffer_pointer = 0;

	return 1;
}
int create_connection(snd_pcm_rsound_t *rd)
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
      rc = start_thread(rd);
      if ( !rc )
      {
         rsound_stop(&rd->io);
         return 0;
      }
   }
	
   return 1;
}

int send_chunk(int socket, char* buf, size_t size)
{
	int rc;
	rc = send(socket, buf, size, 0);
	return rc;
}

void drain(snd_pcm_rsound_t *rd)
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

int fill_buffer(snd_pcm_rsound_t *rd, const char *buf, size_t size)
{
   if ( !rd->thread_active )
      return -1;

   pthread_mutex_lock(&rd->thread.mutex);
   memcpy(rd->buffer + rd->buffer_pointer, buf, size);
   rd->buffer_pointer += (int)size;
   pthread_mutex_unlock(&rd->thread.mutex);

   return size;
}

int start_thread(snd_pcm_rsound_t *rd)
{
   int rc;
   if ( !rd->thread_active )
   {
      rc = pthread_create(&rd->thread.threadId, NULL, rsound_thread, rd);
      if ( rc != 0 )
         return 0;
      rd->thread_active = 1;
      return 1;
   }
   else
      return 1;
}

int stop_thread(snd_pcm_rsound_t *rd)
{
   int rc;
   if ( rd->thread_active )
   {
      rc = pthread_cancel(rd->thread.threadId);
      pthread_mutex_unlock(&rd->thread.mutex);
      if ( rc != 0 )
         return 0;
      rd->thread_active = 0;
      return 1;
   }
   else
      return 1;
}

int get_delay(snd_pcm_rsound_t *rd)
{
   drain(rd);
   return rd->bytes_in_buffer;
}

int get_ptr(snd_pcm_rsound_t *rd)
{
   return rd->buffer_pointer;
}

void* rsound_thread ( void * thread_data )
{
   snd_pcm_rsound_t *rd = thread_data;
   int rc;

// Plays back data as long as there is data in the buffer
   for (;;)
   {
      while ( rd->buffer_pointer >= (int)rd->chunk_size )
      {
         rc = send_chunk(rd->socket, rd->buffer, rd->chunk_size);
         if ( rc <= 0 )
         {
            rsound_stop(&rd->io);
            pthread_exit(NULL);
         }

         pthread_mutex_lock(&rd->thread.mutex);
         memmove(rd->buffer, rd->buffer + rd->chunk_size, rd->buffer_size - rd->chunk_size);
         pthread_mutex_unlock(&rd->thread.mutex);

         if ( !rd->has_written )
         {
            clock_gettime(CLOCK_MONOTONIC, &rd->start_tv);
            pthread_mutex_lock(&rd->thread.mutex);
            rd->has_written = 1;
            pthread_mutex_unlock(&rd->thread.mutex);
         }

         pthread_mutex_lock(&rd->thread.mutex);
         rd->total_written += rc;
         pthread_mutex_unlock(&rd->thread.mutex);
      }
      usleep(1000);
   }
}

   
