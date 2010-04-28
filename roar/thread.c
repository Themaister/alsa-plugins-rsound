
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
         pthread_mutex_lock(&rd->thread.mutex);
         if ( rd->buffer_pointer < (int)rd->backend_info.chunk_size || !rd->thread_active )
         {
            pthread_mutex_unlock(&rd->thread.mutex);
            break;
         }
         pthread_mutex_unlock(&rd->thread.mutex);

         pthread_testcancel();
         rc = rsnd_send_chunk(rd->conn.socket, rd->buffer, rd->backend_info.chunk_size);

         /* If this happens, we should make sure that subsequent and current calls to rsd_write() will fail. */
         if ( rc <= 0 )
         {
            pthread_testcancel();
            rsnd_reset(rd);

            /* Wakes up a potentially sleeping fill_buffer() */
            pthread_cond_signal(&rd->thread.cond);

            /* This thread will not be joined, so detach. */
            pthread_detach(pthread_self());
            pthread_exit(NULL);
         }
         
         /* If this was the first write, set the start point for the timer. */
         if ( !rd->has_written )
         {
            pthread_mutex_lock(&rd->thread.mutex);
#ifdef _POSIX_MONOTONIC_CLOCK
            clock_gettime(CLOCK_MONOTONIC, &rd->start_tv_nsec);
#else
            gettimeofday(&rd->start_tv_usec, NULL);
#endif
            rd->has_written = 1;
            pthread_mutex_unlock(&rd->thread.mutex);
         }

         /* Increase the total_written counter. Used in rsnd_drain() */
         pthread_mutex_lock(&rd->thread.mutex);
         rd->total_written += rc;
         pthread_mutex_unlock(&rd->thread.mutex);

         /* "Drains" the buffer. This operation looks kinda expensive with large buffers, but hey. D: */
         pthread_mutex_lock(&rd->thread.mutex);
         memmove(rd->buffer, rd->buffer + rd->backend_info.chunk_size, rd->buffer_size - rd->backend_info.chunk_size);
         rd->buffer_pointer -= (int)rd->backend_info.chunk_size;
         pthread_mutex_unlock(&rd->thread.mutex);

         /* Buffer has decreased, signal fill_buffer() */
         pthread_cond_signal(&rd->thread.cond);
                          
      }

      /* If we're still good to go, sleep. We are waiting for fill_buffer() to fill up some data. */
      pthread_testcancel();
      if ( rd->thread_active )
      {
         pthread_mutex_lock(&rd->thread.cond_mutex);
         pthread_cond_wait(&rd->thread.cond, &rd->thread.cond_mutex);
         pthread_mutex_unlock(&rd->thread.cond_mutex);
      }
      /* Abort request, chap. */
      else
      {
         pthread_cond_signal(&rd->thread.cond);
         pthread_exit(NULL);
      }

   }
}

