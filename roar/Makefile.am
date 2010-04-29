asound_module_pcm_roar_LTLIBRARIES = libasound_module_pcm_roar.la

asound_module_pcm_roardir = @ALSA_PLUGIN_DIR@

AM_CFLAGS = -Wall -g -pthread @ALSA_CFLAGS@
AM_LDFLAGS = -module -avoid-version -export-dynamic -no-undefined $(LDFLAGS_NOUNDEFINED)

libasound_module_pcm_roar_la_SOURCES = pcm_roar.c thread.c
libasound_module_pcm_roar_la_LIBADD = @ALSA_LIBS@ -lroar -lpthread -lrt


