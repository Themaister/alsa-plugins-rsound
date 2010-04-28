
#define CHUNK_SIZE 256

static void* roar_thread ( void * thread_data )
{
   /* We share data between thread and callable functions */
   struct roar_alsa *self = thread_data;
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
         pthread_mutex_lock(&self->mutex);
         if ( self->bufptr < CHUNK_SIZE || !self->thread_active )
         {
            pthread_mutex_unlock(&self->mutex);
            break;
         }
         pthread_mutex_unlock(&self->mutex);

         pthread_testcancel();
         rc = roar_vio_write(&(self->stream_vio), self->buffer, CHUNK_SIZE);

         /* If this happens, we should make sure that subsequent and current calls to rsd_write() will fail. */
         if ( rc < 0 )
         {
            pthread_testcancel();
            roar_reset(rd);

            /* Wakes up a potentially sleeping fill_buffer() */
            pthread_cond_signal(&self->cond);

            /* This thread will not be joined, so detach. */
            pthread_detach(pthread_self());
            pthread_exit(NULL);
         }
         
         /* "Drains" the buffer. This operation looks kinda expensive with large buffers, but hey. D: */
         pthread_mutex_lock(&self->mutex);
         memmove(self->buffer, self->buffer + rc, rd->buffer_size - rc);
         rd->buffer_pointer -= rc;
         pthread_mutex_unlock(&self->mutex);

         /* Buffer has decreased, signal fill_buffer() */
         pthread_cond_signal(&self->cond);
                          
      }

      /* If we're still good to go, sleep. We are waiting for fill_buffer() to fill up some data. */
      pthread_testcancel();
      if ( self->thread_active )
      {
         pthread_mutex_lock(&rd->thread.cond_mutex);
         pthread_cond_wait(&rd->thread.cond, &rd->thread.cond_mutex);
         pthread_mutex_unlock(&rd->thread.cond_mutex);
      }
      /* Abort request, chap. */
      else
      {
         pthread_cond_signal(&self->cond);
         pthread_exit(NULL);
      }

   }
}

