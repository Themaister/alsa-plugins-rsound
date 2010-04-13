//roar.h:

/*
 *      Copyright (C) Philipp 'ph3-der-loewe' Schafft - 2010
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

#ifndef _ROARAUDIO_PLUGINS_ALSA_ROAR_H_
#define _ROARAUDIO_PLUGINS_ALSA_ROAR_H_

#include <roaraudio.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <alsa/global.h>
#include <alsa/pcm_external.h>
#include <alsa/control_external.h>

#define _as(x) (sizeof((x))/sizeof(*(x)))

struct roar_alsa {
 struct roar_connection con;
};

struct roar_alsa_pcm {
 snd_pcm_ioplug_t       io;
 struct roar_alsa       roar;
 struct roar_audio_info info;
 struct roar_stream     stream;
 struct roar_vio_calls  stream_vio;
 int                    stream_opened;
 size_t                 writec;
};

#endif

//ll
