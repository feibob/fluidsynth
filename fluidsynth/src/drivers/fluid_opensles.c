/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

/* fluid_opensles.c
 *
 * Audio driver for OpenSLES.
 *
 */

#include "fluid_synth.h"
#include "fluid_adriver.h"
#include "fluid_settings.h"

#include "config.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

/** fluid_opensles_audio_driver_t
 *
 * This structure should not be accessed directly. Use audio port
 * functions instead.
 */
typedef struct {
  fluid_audio_driver_t driver;
  SLObjectItf engine;
  SLObjectItf output_mix_object;
  SLObjectItf audio_player;
  SLPlayItf audio_player_interface;
  SLAndroidSimpleBufferQueueItf player_buffer_queue_interface;
  
  fluid_audio_func_t callback;
  void* data;
  int buffer_size;
  fluid_thread_t *thread;
  int cont;
} fluid_opensles_audio_driver_t;


fluid_audio_driver_t* new_fluid_opensles_audio_driver(fluid_settings_t* settings,
						   fluid_synth_t* synth);
fluid_audio_driver_t* new_fluid_opensles_audio_driver2(fluid_settings_t* settings,
						    fluid_audio_func_t func, void* data);
int delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p);
void fluid_opensles_audio_driver_settings(fluid_settings_t* settings);
static void fluid_opensles_audio_run(void* d);
static void fluid_opensles_audio_run2(void* d);


void fluid_opensles_audio_driver_settings(fluid_settings_t* settings)
{
  fluid_settings_register_str(settings, "audio.opensles.server", "default", 0, NULL, NULL);
  fluid_settings_register_str(settings, "audio.opensles.device", "default", 0, NULL, NULL);
  fluid_settings_register_str(settings, "audio.opensles.media-role", "music", 0, NULL, NULL);
  fluid_settings_register_int(settings, "audio.opensles.adjust-latency", 1, 0, 1,
                              FLUID_HINT_TOGGLED, NULL, NULL);
}


fluid_audio_driver_t*
new_fluid_opensles_audio_driver(fluid_settings_t* settings,
			    fluid_synth_t* synth)
{
  return new_fluid_opensles_audio_driver2(settings, NULL, synth);
}

fluid_audio_driver_t*
new_fluid_opensles_audio_driver2(fluid_settings_t* settings,
			     fluid_audio_func_t func, void* data)
{
  SLresult result;
  fluid_opensles_audio_driver_t* dev;
  ////pa_buffer_attr bufattr;
  double sample_rate;
  int period_size, period_bytes, adjust_latency;
  char *server = NULL;
  char *device = NULL;
  char *media_role = NULL;
  int realtime_prio = 0;
  int err;

  dev = FLUID_NEW(fluid_opensles_audio_driver_t);
  if (dev == NULL) {
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }

  FLUID_MEMSET(dev, 0, sizeof(fluid_opensles_audio_driver_t));

  fluid_settings_getint(settings, "audio.period-size", &period_size);
  fluid_settings_getnum(settings, "synth.sample-rate", &sample_rate);
  fluid_settings_dupstr(settings, "audio.opensles.server", &server);  /* ++ alloc server string */
  fluid_settings_dupstr(settings, "audio.opensles.device", &device);  /* ++ alloc device string */
  fluid_settings_dupstr(settings, "audio.opensles.media-role", &media_role);  /* ++ alloc media-role string */
  fluid_settings_getint(settings, "audio.realtime-prio", &realtime_prio);
  fluid_settings_getint(settings, "audio.opensles.adjust-latency", &adjust_latency);

  if (server && strcmp (server, "default") == 0)
  {
    FLUID_FREE (server);        /* -- free server string */
    server = NULL;
  }

  if (device && strcmp (device, "default") == 0)
  {
    FLUID_FREE (device);        /* -- free device string */
    device = NULL;
  }

  dev->data = data;
  dev->callback = func;
  dev->cont = 1;
  dev->buffer_size = period_size;

  period_bytes = period_size * sizeof (float) * 2;
  ////bufattr.maxlength = adjust_latency ? -1 : period_bytes;
  ////bufattr.tlength = period_bytes;
  ////bufattr.minreq = -1;
  ////bufattr.prebuf = -1;    /* Just initialize to same value as tlength */
  ////bufattr.fragsize = -1;  /* Not used */

  result = slCreateEngine (&(dev->engine), 0, NULL, 0, NULL, NULL);

  if (!dev->engine)
  {
    FLUID_LOG(FLUID_ERR, "Failed to create OpenSLES connection");
    goto error_recovery;
  }
  result = (*dev->engine)->Realize (dev->engine, SL_BOOLEAN_FALSE);
  if (result != 0) goto error_recovery;
  
  SLEngineItf engine_interface;
  result = (*dev->engine)->GetInterface (dev->engine, SL_IID_ENGINE, &engine_interface);
  if (result != 0) goto error_recovery;

  result = (*engine_interface)->CreateOutputMix (engine_interface, &dev->output_mix_object, 0, 0, 0);
  if (result != 0) goto error_recovery;
  
  result = (*dev->output_mix_object)->Realize (dev->output_mix_object, SL_BOOLEAN_FALSE);
  if (result != 0) goto error_recovery;

  SLDataLocator_AndroidSimpleBufferQueue loc_buffer_queue = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
    2
    };
    /* TODO: verify sampling rate. Since it accepts only some values as valid ones, I ignored sample_rate and specified this directly. */
  SLDataFormat_PCM format_pcm = {
    SL_DATAFORMAT_PCM,
    2, /* numChannels */
    SL_SAMPLINGRATE_44_1,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    0, /* channelMask */
    SL_BYTEORDER_LITTLEENDIAN
    };
  SLDataSource audio_src = {
    &loc_buffer_queue,
    &format_pcm
    };

  SLDataLocator_OutputMix loc_outmix = {
    SL_DATALOCATOR_OUTPUTMIX,
    dev->output_mix_object
    };
  SLDataSink audio_sink = {&loc_outmix, NULL};

  const SLInterfaceID ids1[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
  const SLboolean req1[] = {SL_BOOLEAN_TRUE};
  result = (*engine_interface)->CreateAudioPlayer (engine_interface,
    &(dev->audio_player), &audio_src, &audio_sink, 1, ids1, req1);
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player)->Realize (dev->audio_player,SL_BOOLEAN_FALSE);
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player)->GetInterface (dev->audio_player, 
    SL_IID_PLAY, &(dev->audio_player_interface));
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player)->GetInterface(dev->audio_player,
    SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &(dev->player_buffer_queue_interface));
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player_interface)->SetPlayState(dev->audio_player_interface, SL_PLAYSTATE_PLAYING);
  if (result != 0) goto error_recovery;

  FLUID_LOG(FLUID_INFO, "Using OpenSLES driver");

  /* Create the audio thread */
  dev->thread = new_fluid_thread ("opensles-audio", func ? fluid_opensles_audio_run2 : fluid_opensles_audio_run,
                                  dev, realtime_prio, FALSE);
  if (!dev->thread)
    goto error_recovery;

  if (server) FLUID_FREE (server);      /* -- free server string */
  if (device) FLUID_FREE (device);      /* -- free device string */

  return (fluid_audio_driver_t*) dev;

 error_recovery:
  if (server) FLUID_FREE (server);      /* -- free server string */
  if (device) FLUID_FREE (device);      /* -- free device string */
  delete_fluid_opensles_audio_driver((fluid_audio_driver_t*) dev);
  return NULL;
}

int delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p)
{
  fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) p;

  if (dev == NULL) {
    return FLUID_OK;
  }

  dev->cont = 0;

  if (dev->thread)
    fluid_thread_join (dev->thread);

  if (dev->audio_player)
    (*dev->audio_player)->Destroy (dev->audio_player);
  if (dev->output_mix_object)
    (*dev->output_mix_object)->Destroy (dev->output_mix_object);
  if (dev->engine)
    (*dev->engine)->Destroy (dev->engine);

  FLUID_FREE(dev);

  return FLUID_OK;
}

/* Thread without audio callback, more efficient */
static void
fluid_opensles_audio_run(void* d)
{
  fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) d;
  float *buf;
  int buffer_size;
  int err;

  buffer_size = dev->buffer_size;

  /* FIXME - Probably shouldn't alloc in run() */
  buf = FLUID_ARRAY(float, buffer_size * 2);

  if (buf == NULL)
  {
    FLUID_LOG(FLUID_ERR, "Out of memory.");
    return;
  }

  while (dev->cont)
  {
    fluid_synth_write_float(dev->data, buffer_size, buf, 0, 2, buf, 1, 2);

    SLresult result = (*dev->player_buffer_queue_interface)->Enqueue (
      dev->player_buffer_queue_interface, buf, buffer_size * sizeof (float) * 2);
    if (result != 0) {
      FLUID_LOG(FLUID_ERR, "Error writing to OpenSLES connection.");
      break;
    }
  }	/* while (dev->cont) */

  FLUID_FREE(buf);
}

static void
fluid_opensles_audio_run2(void* d)
{
  fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) d;
  fluid_synth_t *synth = (fluid_synth_t *)(dev->data);
  float *left, *right, *buf;
  float* handle[2];
  int buffer_size;
  int err;
  int i;

  buffer_size = dev->buffer_size;

  /* FIXME - Probably shouldn't alloc in run() */
  left = FLUID_ARRAY(float, buffer_size);
  right = FLUID_ARRAY(float, buffer_size);
  buf = FLUID_ARRAY(float, buffer_size * 2);

  if (left == NULL || right == NULL || buf == NULL)
  {
    FLUID_LOG(FLUID_ERR, "Out of memory.");
    return;
  }

  handle[0] = left;
  handle[1] = right;

  while (dev->cont)
  {
    (*dev->callback)(synth, buffer_size, 0, NULL, 2, handle);

    /* Interleave the floating point data */
    for (i = 0; i < buffer_size; i++)
    {
      buf[i * 2] = left[i];
      buf[i * 2 + 1] = right[i];
    }

    SLresult result = (*dev->player_buffer_queue_interface)->Enqueue (
      dev->player_buffer_queue_interface, buf, buffer_size * sizeof (float) * 2);
    if (result != 0) {
      FLUID_LOG(FLUID_ERR, "Error writing to OpenSLES connection.");
      break;
    }
  }	/* while (dev->cont) */

  FLUID_FREE(left);
  FLUID_FREE(right);
  FLUID_FREE(buf);
}
