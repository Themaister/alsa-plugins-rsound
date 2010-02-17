asound_module_pcm_rsound_LTLIBRARIES = libasound_module_pcm_rsound.la

asound_module_pcm_rsounddir = @ALSA_PLUGIN_DIR@

AM_CFLAGS = -Wall -g @ALSA_CFLAGS@
AM_LDFLAGS = -module -avoid-version -export-dynamic -no-undefined $(LDFLAGS_NOUNDEFINED)

libasound_module_pcm_rsound_la_SOURCES = pcm_rsound.c
libasound_module_pcm_rsound_la_LIBADD = @ALSA_LIBS@ -lrt


