//pcm.c:

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

// Equvivalent to prepare(). Starts a stream. Also needs to reset the writec since pointer() will do funny
// things without it. Will be called several times during a program!
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

 // Stream is now active, yay.
 self->stream_opened = 1;
 self->writec = 0;

 return 0;
}

// Simply stopping the stream. Will need to be restarted to play more.
// Will be called several times together with roar_pcm_start()
static int roar_pcm_stop (snd_pcm_ioplug_t *io) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_stop(*) = ?");

 // If this is called several times in a row, just ignore.
 if ( !self->stream_opened )
  return 0;

 roar_vio_close(&(self->stream_vio));
 self->stream_opened = 0;

 ROAR_DBG("roar_pcm_stop(*) = 0");

 return 0;
}

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

#if 0
 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 1, 4294967295U)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 1, 4294967295U)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1, 4294967295U)) < 0 )
  return ret;
#else
 // We shouldn't let ALSA use extremely low or high values, it will kill a kitty most likely. :v
 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 1 << 6, 1 << 18)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 1, 1024)) < 0 )
  return ret;

 if ( (ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1 << 13, 1 << 24)) < 0 )
  return ret;
#endif

 ROAR_DBG("roar_hw_constraint(*) = 0");

 return 0;
}


///////////////////////////////
/// TODO: Needs to be implemented
///////////////////////////////
// This hacky hack should not be used, since apparently, using this, ALSA will think the audio buffer is always empty (?),
// leading to completely broken blocking audio which will only work with audio players.
// Will need a method to determine how much buffer data is available to write to the roar buffer without blocking.
// We therefore also need to know the buffersize that ALSA uses.

// Referring to alsa-lib/src/pcm/pcm_ioplug.c : snd_pcm_ioplug_hw_ptr_update
static snd_pcm_sframes_t roar_pcm_pointer(snd_pcm_ioplug_t *io) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_pointer(*) = ?");

 return snd_pcm_bytes_to_frames(io->pcm, self->writec);
}

// TODO: FIXME: add support for reading data!
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

 ret = roar_vio_write(&(self->stream_vio), buf, len);

 if ( ret != -1 ) {
  // We increment the written counter so that subsequent calls to pointer() will not cause
  // the library to hang due to several quirks that ALSA uses to determine available size.
  // This approach is bad, but is needed until pointer() is implemented correctly.
  self->writec += ret;
 } else {
  return -EIO;
 }

 ROAR_DBG("roar_pcm_transfer(*) = %lli", (long long int)size);
 return size;
}


///////////////////////////////
/// TODO: Needs to be implemented
///////////////////////////////
static int roar_pcm_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
 (void)io;

 ROAR_DBG("roar_pcm_delay(*) = ?");

 // TODO: We need to set *delayp the latency in frames.
 *delayp = 0;

 return 0;
}

static int roar_pcm_prepare(snd_pcm_ioplug_t *io) {
 ROAR_DBG("roar_pcm_prepare(*) = ?");

 return roar_pcm_start(io);
}

static int roar_pcm_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
 struct roar_alsa_pcm * self = io->private_data;

 (void) params;

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
    return-EINVAL;
 }

 ROAR_DBG("roar_pcm_hw_params(*) = 0");
 return 0;
}

static int roar_pcm_close (snd_pcm_ioplug_t * io) {
 struct roar_alsa_pcm * self = io->private_data;

 ROAR_DBG("roar_pcm_close(*) = ?");

 roar_disconnect(&(self->roar.con));

 free(self);

 return 0;
}

static snd_pcm_ioplug_callback_t roar_pcm_callback = {
    .start                  = roar_pcm_start,
    .stop                   = roar_pcm_stop,
    .drain                  = NULL,
    .pointer                = roar_pcm_pointer,
    .transfer               = roar_pcm_transfer,
    .delay                  = roar_pcm_delay,
    .poll_descriptors_count = NULL,
    .poll_descriptors       = NULL,
    .poll_revents           = NULL,
    .prepare                = roar_pcm_prepare,
    .hw_params              = roar_pcm_hw_params,
    .hw_free                = NULL,
    .sw_params              = NULL,
    .pause                  = NULL,
    .resume                 = NULL,
    .dump                   = NULL,
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
 self->io.poll_fd      = -1;
 self->io.poll_events  =  0;
 self->io.mmap_rw      =  0;
 self->io.callback     = &roar_pcm_callback;
 self->io.private_data =  self;

 if ( (ret = snd_pcm_ioplug_create(&(self->io), name, stream, mode)) < 0 ) {
  roar_disconnect(&(self->roar.con));
  free(self);
  return ret;
 }

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
