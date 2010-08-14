//pcm_roar.c:

/*
 *      Copyright (C) Philipp 'ph3-der-loewe' Schafft - 2010
 *      Copyright (C) Hans-Kristian 'maister' Arntzen - 2010
 *
 *  This file is part of libroar a part of RoarAudio,
 *  a cross-platform sound system for both, home and professional use.
 *  See README for details.
 *
 *  This file is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3
 *  as published by the Free Software Foundation.
 *
 *  libroar is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 *  NOTE for everyone want's to change something and send patches:
 *  read README and HACKING! There a addition information on
 *  the license of this document you need to read before you send
 *  any patches.
 *
 *  NOTE for uses of non-GPL (LGPL,...) software using libesd, libartsc
 *  or libpulse*:
 *  The libs libroaresd, libroararts and libroarpulse link this lib
 *  and are therefore GPL. Because of this it may be illigal to use
 *  them with any software that uses libesd, libartsc or libpulse*.
 */

#include "roar.h"

// Equvivalent to prepare(). Starts a stream. Might/will be called several times during a program!
/////////////////////////////////////////////////
// Status: Should be mostly complete. 
// Condition for invalid fh is not solved here.
////////////////////////////////////////////////
static int roar_pcm_start (snd_pcm_ioplug_t * io) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_start(*) = ?");

 // If start is called several times in a row, just ignore it.
 if (self->stream_opened)
  return 0;

 if ( roar_vio_simple_new_stream_obj(&(self->stream_vio), &(self->roar.con), &(self->stream),
    self->info.rate, self->info.channels, self->info.bits, self->info.codec,
    io->stream == SND_PCM_STREAM_PLAYBACK ? ROAR_DIR_PLAY : ROAR_DIR_MONITOR
    ) == -1 ) {
  return -EINVAL;
 }

 int fh;
 if ( roar_vio_ctl(&(self->stream_vio), 
    io->stream == SND_PCM_STREAM_PLAYBACK ? ROAR_VIO_CTL_GET_SELECT_WRITE_FH :
    ROAR_VIO_CTL_GET_SELECT_READ_FH, &fh) != 1 ) {
  io->poll_fd = fh;
  io->poll_events = io->stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
 }

 // Oh, no, what should we do here if roar_vio_ctl() fails to grab a valid fh to poll?
 // In any case, ALSA will error out the next time it tries to poll() if we don't give it a valid fh.

 snd_pcm_ioplug_reinit_status(io);

 // Stream is now active, yay.
 self->stream_opened = 1;

 self->bufptr = 0;
 self->thread_active = 1; // We have to activate the thread before starting it, because the thread lives on that thread_active is 1.
 if ( pthread_create(&(self->thread), NULL, roar_thread, self) < 0 ) {
  self->thread_active = 0;
  return -1;
 }

 return 0;
}

void roar_reset(struct roar_alsa_pcm *self) {
 if ( !self->stream_opened )
  return;

 roar_vio_close(&(self->stream_vio));
 self->stream_opened = 0;
 self->thread_active = 0;
 self->bufptr = 0;
 self->last_ptr = 0;
 self->total_written = 0;
 self->has_written = 0;
}



// Simply stopping the stream. Will need to be restarted to play more.
// Will be called several times together with roar_pcm_start()
///////////////////////////////////////////////////
// Status: Still needs some error checking for the pthread calls, but
// should work.
//////////////////////////////////////////////////
static int roar_pcm_stop (snd_pcm_ioplug_t *io) {
 struct roar_alsa_pcm * self = io->private_data;

 // If this is called several times in a row, just ignore.

 ROAR_DBG("roar_pcm_stop(*) = 0");


 if ( self->thread_active ) {
  self->thread_active = 0;
  pthread_cond_signal(&(self->cond));
  pthread_join(self->thread, NULL);
 }

 roar_reset(self);

 return 0;
}

///////////////////////////////
// Status: Should be complete.
///////////////////////////////
static int roar_hw_constraint(struct roar_alsa_pcm * self) {
 snd_pcm_ioplug_t *io = &(self->io);
 static const snd_pcm_access_t access_list[] = {
  SND_PCM_ACCESS_RW_INTERLEAVED
 };
 static const unsigned int formats[] = {
  SND_PCM_FORMAT_S8,
  SND_PCM_FORMAT_U8,
  SND_PCM_FORMAT_A_LAW,
  SND_PCM_FORMAT_MU_LAW,
  SND_PCM_FORMAT_S16_LE,
  SND_PCM_FORMAT_S16_BE,
  SND_PCM_FORMAT_U16_LE,
  SND_PCM_FORMAT_U16_BE,
  SND_PCM_FORMAT_S32_LE,
  SND_PCM_FORMAT_S32_BE,
  SND_PCM_FORMAT_U32_LE,
  SND_PCM_FORMAT_U32_BE,
  SND_PCM_FORMAT_S24_3LE,
  SND_PCM_FORMAT_S24_3BE,
  SND_PCM_FORMAT_U24_3LE,
  SND_PCM_FORMAT_U24_3BE,
 };
 int ret;

 ROAR_DBG("roar_hw_constraint(*) = ?");

 if ( (ret = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
     _as(access_list), access_list)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
     _as(formats), formats)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
     1, ROAR_MAX_CHANNELS)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE, 8000, 192000)) < 0 )
  return ret;

 // We shouldn't let ALSA use extremely low or high values, it will kill a kitty most likely. :v
 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 1 << 6, 1 << 18)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 1, 1024)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1 << 13, 1 << 24)) < 0 )
  return ret;

 ROAR_DBG("roar_hw_constraint(*) = 0");

 return 0;
}

// Referring to alsa-lib/src/pcm/pcm_ioplug.c : snd_pcm_ioplug_hw_ptr_update
///////////////////////////////////////////////////////////
// Status: Mostly complete, but uses a really nasty hack!
///////////////////////////////////////////////////////////
static snd_pcm_sframes_t roar_pcm_pointer(snd_pcm_ioplug_t *io) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_pointer(*) = ?");

 int ptr;
 // Did ALSA just call snd_pcm_reset() or something like that without calling the plugin? 
 // We should restart our stream as well.
 if ( io->appl_ptr < self->last_ptr ) {
  roar_pcm_stop(io);
  roar_pcm_start(io);
 }

 // ALSA has a weird way of calculating how much data can be written to the audio buffer.
 // It uses the formula:
 // avail = bufsize + ptr - io->appl_ptr; (??!?)
 // We really want this:
 // avail = bufsize - ptr;
 // This is the obvious way, so we have to manipulate ptr like this:
 // ptr = io->appl_ptr - ptr;

 pthread_mutex_lock(&(self->lock));
 ptr = snd_pcm_bytes_to_frames(io->pcm, self->bufptr);
 pthread_mutex_unlock(&(self->lock));


 ptr = io->appl_ptr - ptr;
 self->last_ptr = io->appl_ptr;

 ROAR_DBG("roar_pcm_pointer(*) appl_ptr: %d, ptr: %d, calculated avail frames: %d", (int)io->appl_ptr, (int)ptr, (int)(io->appl_ptr - ptr));
 return ptr;
}

// TODO: FIXME: add support for reading data!
//////////////////////////////////////////////////
// Status: For writing, this should be complete.
//////////////////////////////////////////////////
static snd_pcm_sframes_t roar_pcm_transfer(snd_pcm_ioplug_t *io,
  const snd_pcm_channel_area_t *areas,
  snd_pcm_uframes_t offset,
  snd_pcm_uframes_t size) {
 struct roar_alsa_pcm * self = io->private_data;
 char * buf;
 size_t len = size * self->info.channels * self->info.bits / 8;
 ssize_t ret;

 ROAR_DBG("roar_pcm_transfer(*) = ?");
 ROAR_DBG("roar_pcm_transfer(*): len=%lu", (long unsigned int) len);

 // Weird ALSA stuff.
 buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;

 ret = roar_write(self, buf, len);

 if ( ret == -1 )
  return -EIO;

 ROAR_DBG("roar_pcm_transfer(*) = %lli", (long long int)size);
 return size;
}

///////////////////////////////////////////////////////////////////
// Status: Still missing proper delay measurements from the roar server. 
// Only uses a blind timer now. In ideal conditions, this will work well.
///////////////////////////////////////////////////////////////////
static int roar_pcm_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_delay(*) = ?");

 // TODO: We need to set *delayp the latency in frames.
 pthread_mutex_lock(&(self->lock));
 roar_drain(self);
 *delayp = snd_pcm_bytes_to_frames(io->pcm, self->bytes_in_buffer);
 pthread_mutex_unlock(&(self->lock));

 return 0;
}

////////////////////
// Status: Complete
////////////////////
static int roar_pcm_prepare(snd_pcm_ioplug_t *io) {
 ROAR_DBG("roar_pcm_prepare(*) = ?");

 return roar_pcm_start(io);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Status: This should be mostly complete.
// I'm not sure if hw_params can be called several times during one stream without stop() start() in between. 
// This will mean a memory leak, and possibly breakage should the buffer size change itself.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
static int roar_pcm_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_hw_params(*) = ?");

 self->info.channels = io->channels;
 self->info.rate     = io->rate;

 switch (io->format) {
  case SND_PCM_FORMAT_S8:
   self->info.codec = ROAR_CODEC_PCM_U_LE;
   self->info.bits  = 8;
   break;
  case SND_PCM_FORMAT_U8:
   self->info.codec = ROAR_CODEC_PCM_U_LE;
   self->info.bits  = 8;
   break;
  case SND_PCM_FORMAT_A_LAW:
   self->info.codec = ROAR_CODEC_ALAW;
   self->info.bits  = 8;
   break;
  case SND_PCM_FORMAT_MU_LAW:
   self->info.codec = ROAR_CODEC_MULAW;
   self->info.bits  = 8;
   break;
  case SND_PCM_FORMAT_S16_LE:
   self->info.codec = ROAR_CODEC_PCM_S_LE;
   self->info.bits  = 16;
   break;
  case SND_PCM_FORMAT_S16_BE:
   self->info.codec = ROAR_CODEC_PCM_S_BE;
   self->info.bits  = 16;
   break;
  case SND_PCM_FORMAT_U16_LE:
   self->info.codec = ROAR_CODEC_PCM_U_LE;
   self->info.bits  = 16;
   break;
  case SND_PCM_FORMAT_U16_BE:
   self->info.codec = ROAR_CODEC_PCM_U_BE;
   self->info.bits  = 16;
   break;
  case SND_PCM_FORMAT_S32_LE:
   self->info.codec = ROAR_CODEC_PCM_S_LE;
   self->info.bits  = 32;
   break;
  case SND_PCM_FORMAT_S32_BE:
   self->info.codec = ROAR_CODEC_PCM_S_BE;
   self->info.bits  = 32;
   break;
  case SND_PCM_FORMAT_U32_LE:
   self->info.codec = ROAR_CODEC_PCM_U_LE;
   self->info.bits  = 32;
   break;
  case SND_PCM_FORMAT_U32_BE:
   self->info.codec = ROAR_CODEC_PCM_U_BE;
   self->info.bits  = 32;
   break;
  case SND_PCM_FORMAT_S24_3LE:
   self->info.codec = ROAR_CODEC_PCM_S_LE;
   self->info.bits  = 24;
   break;
  case SND_PCM_FORMAT_S24_3BE:
   self->info.codec = ROAR_CODEC_PCM_S_BE;
   self->info.bits  = 24;
   break;
  case SND_PCM_FORMAT_U24_3LE:
   self->info.codec = ROAR_CODEC_PCM_U_LE;
   self->info.bits  = 24;
   break;
  case SND_PCM_FORMAT_U24_3BE:
   self->info.codec = ROAR_CODEC_PCM_U_BE;
   self->info.bits  = 24;
   break;
  default:
   return -EINVAL;
 }

 snd_pcm_uframes_t buffersize;
 int err;

 if ((err = snd_pcm_hw_params_get_buffer_size(params, &buffersize) < 0))
  return err;

 //self->bufsize = snd_pcm_frames_to_bytes(io->pcm, buffersize);
 self->bufsize = self->info.bits * self->info.channels * buffersize / 8;
 self->buffer = malloc(self->bufsize);
 if (self->buffer == NULL)
  return -1;
 self->bufptr = 0;

 ROAR_DBG("roar_pcm_hw_params(*) Setting buffersize (bytes): %d", (int)self->bufsize);
 ROAR_DBG("roar_pcm_hw_params(*) = 0");
 return 0;
}

///////////////////////////////////
// Status: This should be complete. 
// This is the last cleanup function to be called by ALSA.
///////////////////////////////////
static int roar_pcm_close (snd_pcm_ioplug_t * io) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_close(*) = ?");

 roar_disconnect(&(self->roar.con));

 pthread_mutex_destroy(&(self->lock));
 pthread_mutex_destroy(&(self->cond_lock));
 pthread_cond_destroy(&(self->cond));

 free(self->buffer);
 free(self);

 return 0;
}

static snd_pcm_ioplug_callback_t roar_pcm_callback = {
 .start                  = roar_pcm_start,
 .stop                   = roar_pcm_stop,
 .pointer                = roar_pcm_pointer,
 .transfer               = roar_pcm_transfer,
 .delay                  = roar_pcm_delay,
 .prepare                = roar_pcm_prepare,
 .hw_params              = roar_pcm_hw_params,
 .close                  = roar_pcm_close,
};

SND_PCM_PLUGIN_DEFINE_FUNC(roar) {
 struct roar_alsa_pcm * self;
 snd_config_iterator_t i, next;
 snd_config_t * n;
 const char   * para;
 const char   * server = NULL;
 int            ret;

 (void)root;

 ROAR_DBG("SND_PCM_PLUGIN_DEFINE_FUNC(roar) = ?");

 snd_config_for_each(i, next, conf) {
  n = snd_config_iterator_entry(i);
  if ( snd_config_get_id(n, &para) < 0 )
   continue;

  if ( !strcmp(para, "type") || !strcmp(para, "comment") )
   continue;

  if ( !strcmp(para, "server") ) {
   if (snd_config_get_string(n, &server) < 0) {
    return -EINVAL;
   }
  } else {
   return -EINVAL;
  }
 }

 errno = ENOSYS;

 if ( (self = malloc(sizeof(struct roar_alsa_pcm))) == NULL )
  return -errno;

 memset(self, 0, sizeof(struct roar_alsa_pcm));

 errno = ENOSYS;
 if ( roar_simple_connect(&(self->roar.con), (char*)server, "ALSA Plugin") == -1 ) {
  free(self);
  return -errno;
 }

 self->io.version      = SND_PCM_IOPLUG_VERSION;
 self->io.name         = "RoarAudio Plugin";
 self->io.poll_fd      =  -1;
 self->io.poll_events  =  POLLOUT;
 self->io.mmap_rw      =  0;
 self->io.callback     = &roar_pcm_callback;
 self->io.private_data =  self;

 if ( (ret = snd_pcm_ioplug_create(&(self->io), name, stream, mode)) < 0 ) {
  roar_disconnect(&(self->roar.con));
  free(self);
  return ret;
 }

 pthread_mutex_init(&(self->lock), NULL);
 pthread_mutex_init(&(self->cond_lock), NULL);
 pthread_cond_init(&(self->cond), NULL);


 if ( (ret = roar_hw_constraint(self)) < 0 ) {
  snd_pcm_ioplug_delete(&(self->io));
  roar_disconnect(&(self->roar.con));
  free(self);
  return ret;
 }

 *pcmp = self->io.pcm;

 ROAR_DBG("SND_PCM_PLUGIN_DEFINE_FUNC(roar) = 0");

 return 0;
}

SND_PCM_PLUGIN_SYMBOL(roar);


//ll
