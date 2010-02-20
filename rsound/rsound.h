#ifndef __RSOUND_H
#define __RSOUND_H

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
#include <pthread.h>

typedef struct rsound_thread
{
   pthread_t threadId;
   pthread_mutex_t mutex;

} rsound_thread_t;

typedef struct snd_pcm_rsound {
   snd_pcm_ioplug_t io;
   int socket;
   char *host;
   char *port;
   char *buffer;
	int buffer_pointer;
   size_t chunk_size;
   size_t buffer_size;
   int bytes_per_frame;

   int thread_active;

   uint64_t total_written;
   struct timespec start_tv;
   int has_written;
   int bytes_in_buffer;

   int ready_for_data;

	uint32_t rate;
	uint16_t channels;

   snd_pcm_uframes_t alsa_buffer_size;
   snd_pcm_uframes_t alsa_fragsize;

   rsound_thread_t thread;

} snd_pcm_rsound_t;

// Thread
void* rsnd_thread (void *thread_data);

// Cool utils
int rsnd_is_little_endian(void);
void rsnd_swap_endian_16 ( uint16_t * x );
void rsnd_swap_endian_32 ( uint32_t * x );

// RSound related
int rsnd_connect_server ( snd_pcm_rsound_t *rd );
int rsnd_create_connection ( snd_pcm_rsound_t *rd );
int rsnd_send_header_info ( snd_pcm_rsound_t *rd );
int rsnd_get_backend_info ( snd_pcm_rsound_t *rd );

// Cool stuff
void rsnd_drain(snd_pcm_rsound_t *rd);
int rsnd_fill_buffer(snd_pcm_rsound_t *rd, const char *buf, size_t size);
int rsnd_send_chunk (int socket, char* buf, size_t size);

// ALSA callbacks
int rsnd_get_ptr( snd_pcm_rsound_t *rd );
int rsnd_get_delay ( snd_pcm_rsound_t *rd );

// I/O callback
int rsound_stop(snd_pcm_ioplug_t *io);

// Dirty work inc.
int rsnd_start_thread( snd_pcm_rsound_t *rd );
int rsnd_stop_thread( snd_pcm_rsound_t *rd );

#endif
