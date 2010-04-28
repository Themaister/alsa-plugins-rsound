
#include "roar.h"
#define CHUNK_SIZE 256


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

         pthread_testcancel();
         rc = roar_vio_write(&(self->stream_vio), self->buffer, CHUNK_SIZE);
       //  fprintf(stderr, "Thread wrote %d\n", rc);

         /* If this happens, we should make sure that subsequent and current calls to rsd_write() will fail. */
         if ( rc < 0 )
         {
            pthread_testcancel();
            roar_reset(self);

            /* Wakes up a potentially sleeping fill_buffer() */
            pthread_cond_signal(&self->cond);

            /* This thread will not be joined, so detach. */
            pthread_detach(pthread_self());
            pthread_exit(NULL);
         }
         
         /* "Drains" the buffer. This operation looks kinda expensive with large buffers, but hey. D: */
         pthread_mutex_lock(&self->lock);
         memmove(self->buffer, self->buffer + rc, self->bufsize - rc);
         self->bufptr -= rc;
         pthread_mutex_unlock(&self->lock);

         /* Buffer has decreased, signal fill_buffer() */
         pthread_cond_signal(&self->cond);
                          
      }

      /* If we're still good to go, sleep. We are waiting for fill_buffer() to fill up some data. */
      pthread_testcancel();
      if ( self->thread_active )
      {
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

