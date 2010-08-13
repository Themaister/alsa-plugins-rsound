
#include "roar.h"
#define CHUNK_SIZE 256


// Writes to the FIFO buffer. Waits until there is room to write.
size_t roar_write( struct roar_alsa_pcm *self, const char *buf, size_t size )
{
   /* Wait until we have a ready buffer */
   for (;;)
   {
      /* Should the thread be shut down while we're running, return with error */
      if ( !self->thread_active )
         return 0;

      //fprintf(stderr, "%d + %d : %d\n", (int)self->bufptr, (int)size, (int)self->bufsize);
      pthread_mutex_lock(&self->lock);
      if ( self->bufptr + size <= self->bufsize  )
      {
         pthread_mutex_unlock(&self->lock);
         break;
      }
      pthread_mutex_unlock(&self->lock);
      
      pthread_cond_signal(&self->cond);
      /* Sleeps until we can write to the FIFO. */
      pthread_mutex_lock(&self->cond_lock);
      pthread_cond_wait(&self->cond, &self->cond_lock);
      pthread_mutex_unlock(&self->cond_lock);
   }
   
   pthread_mutex_lock(&self->lock);
   memcpy(self->buffer + self->bufptr, buf, size);
   self->bufptr += (int)size;
   pthread_mutex_unlock(&self->lock);

   /* Send signal to thread that buffer has been updated */
   pthread_cond_signal(&self->cond);

   return size;
}

#define TEST_CANCEL do { \
   if ( !self->thread_active ) \
    goto test_quit; \
} while(0)

// Attemps to drain the buffer at all times and write to libroar.
// If there is no data, it will wait for roar_write() to fill up more data.
void* roar_thread ( void * thread_data )
{
   /* We share data between thread and callable functions */
   struct roar_alsa_pcm *self = thread_data;
   int rc;

   /* Plays back data as long as there is data in the buffer. Else, sleep until it can. */
   /* Two (;;) for loops! :3 Beware! */
   for (;;)
   {
      
      for(;;)
      {
         
         // We ask the server to send its latest backend data. Do not really care about errors atm.
         // We only bother to check after 1 sec of audio has been played, as it might be quite inaccurate in the start of the stream.
         
         /* If the buffer is empty or we've stopped the stream. Jump out of this for loop */
         pthread_mutex_lock(&self->lock);
         if ( self->bufptr < CHUNK_SIZE || !self->thread_active )
         {
            pthread_mutex_unlock(&self->lock);
            break;
         }
         pthread_mutex_unlock(&self->lock);

         TEST_CANCEL;
         rc = roar_vio_write(&(self->stream_vio), self->buffer, CHUNK_SIZE);

         /* If this happens, we should make sure that subsequent and current calls to rsd_write() will fail. */
         if ( rc < 0 )
         {
            TEST_CANCEL;
            roar_reset(self);

            /* Wakes up a potentially sleeping fill_buffer() */
            pthread_cond_signal(&self->cond);

            /* This thread will not be joined, so detach. */
            pthread_detach(pthread_self());
            pthread_exit(NULL);
         }

         if ( !self->has_written )
         {
            pthread_mutex_lock(&self->lock);
            clock_gettime(CLOCK_MONOTONIC, &self->start_tv);
            self->has_written = 1;
            pthread_mutex_unlock(&self->lock);
         }

         pthread_mutex_lock(&self->lock);
         self->total_written += rc;
         pthread_mutex_unlock(&self->lock);

         /* "Drains" the buffer. This operation looks kinda expensive with large buffers, but hey. D: */
         pthread_mutex_lock(&self->lock);
         memmove(self->buffer, self->buffer + rc, self->bufsize - rc);
         self->bufptr -= rc;
         pthread_mutex_unlock(&self->lock);

         /* Buffer has decreased, signal fill_buffer() */
         pthread_cond_signal(&self->cond);
                          
      }

      /* If we're still good to go, sleep. We are waiting for fill_buffer() to fill up some data. */
test_quit:
      if ( self->thread_active )
      {
         pthread_cond_signal(&self->cond);
         pthread_mutex_lock(&self->cond_lock);
         pthread_cond_wait(&self->cond, &self->cond_lock);
         pthread_mutex_unlock(&self->cond_lock);
      }
      /* Abort request, chap. */
      else
      {
         pthread_cond_signal(&self->cond);
         pthread_exit(NULL);
      }

   }
}

void roar_drain(struct roar_alsa_pcm *self)
{
   /* If the audio playback has started on the server we need to use timers. */
   if ( self->has_written )
   {
      int64_t temp, temp2;

/* Falls back to gettimeofday() when CLOCK_MONOTONIC is not supported */

/* Calculates the amount of bytes that the server has consumed. */
      struct timespec now_tv;
      clock_gettime(CLOCK_MONOTONIC, &now_tv);
      
      temp = (int64_t)now_tv.tv_sec - (int64_t)self->start_tv.tv_sec;

      temp *= self->info.rate * self->info.channels * self->info.bits / 8;

      temp2 = (int64_t)now_tv.tv_nsec - (int64_t)self->start_tv.tv_nsec;
      temp2 *= self->info.rate * self->info.channels * self->info.bits / 8;
      temp2 /= 1000000000;
      temp += temp2;
      /* Calculates the amount of data we have in our virtual buffer. Only used to calculate delay. */
      self->bytes_in_buffer = (int)((int64_t)self->total_written + (int64_t)self->bufptr - temp);
   }
   else
      self->bytes_in_buffer = self->bufptr;
}



