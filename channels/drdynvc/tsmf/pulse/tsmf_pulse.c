/**
 * FreeRDP: A Remote Desktop Protocol client.
 * Video Redirection Virtual Channel - PulseAudio Device
 *
 * Copyright 2010-2011 Vic Lee
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pulse/pulseaudio.h>
#include <freerdp/utils/memory.h>

#include "tsmf_audio.h"

typedef struct _TSMFPulseAudioDevice
{
	ITSMFAudioDevice iface;

	char device[32];
	pa_threaded_mainloop* mainloop;
	pa_context* context;
	pa_sample_spec sample_spec;
	pa_stream* stream;
} TSMFPulseAudioDevice;

static void tsmf_pulse_context_state_callback(pa_context* context, void* userdata)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) userdata;
	pa_context_state_t state;

	state = pa_context_get_state(context);
	switch (state)
	{
		case PA_CONTEXT_READY:
			DEBUG_DVC("PA_CONTEXT_READY");
			pa_threaded_mainloop_signal(pulse->mainloop, 0);
			break;

		case PA_CONTEXT_FAILED:
		case PA_CONTEXT_TERMINATED:
			DEBUG_DVC("state %d", (int)state);
			pa_threaded_mainloop_signal(pulse->mainloop, 0);
			break;

		default:
			DEBUG_DVC("state %d", (int)state);
			break;
	}
}

static boolean tsmf_pulse_connect(TSMFPulseAudioDevice* pulse)
{
	pa_context_state_t state;

	if (!pulse->context)
		return False;

	if (pa_context_connect(pulse->context, NULL, 0, NULL))
	{
		DEBUG_WARN("pa_context_connect failed (%d)",
			pa_context_errno(pulse->context));
		return False;
	}
	pa_threaded_mainloop_lock(pulse->mainloop);
	if (pa_threaded_mainloop_start(pulse->mainloop) < 0)
	{
		pa_threaded_mainloop_unlock(pulse->mainloop);
		DEBUG_WARN("pa_threaded_mainloop_start failed (%d)",
			pa_context_errno(pulse->context));
		return False;
	}
	for (;;)
	{
		state = pa_context_get_state(pulse->context);
		if (state == PA_CONTEXT_READY)
			break;
		if (!PA_CONTEXT_IS_GOOD(state))
		{
			DEBUG_DVC("bad context state (%d)",
				pa_context_errno(pulse->context));
			break;
		}
		pa_threaded_mainloop_wait(pulse->mainloop);
	}
	pa_threaded_mainloop_unlock(pulse->mainloop);
	if (state == PA_CONTEXT_READY)
	{
		DEBUG_DVC("connected");
		return True;
	}
	else
	{
		pa_context_disconnect(pulse->context);
		return False;
	}
}

static boolean tsmf_pulse_open(ITSMFAudioDevice* audio, const char* device)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) audio;

	if (device)
	{
		strcpy(pulse->device, device);
	}

	pulse->mainloop = pa_threaded_mainloop_new();
	if (!pulse->mainloop)
	{
		DEBUG_WARN("pa_threaded_mainloop_new failed");
		return False;
	}
	pulse->context = pa_context_new(pa_threaded_mainloop_get_api(pulse->mainloop), "freerdp");
	if (!pulse->context)
	{
		DEBUG_WARN("pa_context_new failed");
		return False;
	}
	pa_context_set_state_callback(pulse->context, tsmf_pulse_context_state_callback, pulse);
	if (tsmf_pulse_connect(pulse))
	{
		DEBUG_WARN("tsmf_pulse_connect failed");
		return False;
	}

	DEBUG_DVC("open device %s", pulse->device);
	return True;
}

static void tsmf_pulse_stream_success_callback(pa_stream* stream, int success, void* userdata)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) userdata;

	pa_threaded_mainloop_signal(pulse->mainloop, 0);
}

static void tsmf_pulse_wait_for_operation(TSMFPulseAudioDevice* pulse, pa_operation* operation)
{
	if (operation == NULL)
		return;
	while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
	{
		pa_threaded_mainloop_wait(pulse->mainloop);
	}
	pa_operation_unref(operation);
}

static void tsmf_pulse_stream_state_callback(pa_stream* stream, void* userdata)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) userdata;
	pa_stream_state_t state;

	state = pa_stream_get_state(stream);
	switch (state)
	{
		case PA_STREAM_READY:
			DEBUG_DVC("PA_STREAM_READY");
			pa_threaded_mainloop_signal (pulse->mainloop, 0);
			break;

		case PA_STREAM_FAILED:
		case PA_STREAM_TERMINATED:
			DEBUG_DVC("state %d", (int)state);
			pa_threaded_mainloop_signal (pulse->mainloop, 0);
			break;

		default:
			DEBUG_DVC("state %d", (int)state);
			break;
	}
}

static void tsmf_pulse_stream_request_callback(pa_stream* stream, size_t length, void* userdata)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) userdata;

	DEBUG_DVC("%d", (int) length);

	pa_threaded_mainloop_signal(pulse->mainloop, 0);
}

static boolean tsmf_pulse_close_stream(TSMFPulseAudioDevice* pulse)
{
	if (!pulse->context || !pulse->stream)
		return False;

	DEBUG_DVC("");

	pa_threaded_mainloop_lock(pulse->mainloop);
	pa_stream_set_write_callback(pulse->stream, NULL, NULL);
	tsmf_pulse_wait_for_operation(pulse,
		pa_stream_drain(pulse->stream, tsmf_pulse_stream_success_callback, pulse));
	pa_stream_disconnect(pulse->stream);
	pa_stream_unref(pulse->stream);
	pulse->stream = NULL;
	pa_threaded_mainloop_unlock(pulse->mainloop);

	return True;
}

static boolean tsmf_pulse_open_stream(TSMFPulseAudioDevice* pulse)
{
	pa_stream_state_t state;
	pa_buffer_attr buffer_attr = { 0 };

	if (!pulse->context)
		return False;

	DEBUG_DVC("");

	pa_threaded_mainloop_lock(pulse->mainloop);
	pulse->stream = pa_stream_new(pulse->context, "freerdp",
		&pulse->sample_spec, NULL);
	if (!pulse->stream)
	{
		pa_threaded_mainloop_unlock(pulse->mainloop);
		DEBUG_WARN("pa_stream_new failed (%d)",
			pa_context_errno(pulse->context));
		return False;
	}
	pa_stream_set_state_callback(pulse->stream,
		tsmf_pulse_stream_state_callback, pulse);
	pa_stream_set_write_callback(pulse->stream,
		tsmf_pulse_stream_request_callback, pulse);
	buffer_attr.maxlength = pa_usec_to_bytes(500000, &pulse->sample_spec);
	buffer_attr.tlength = pa_usec_to_bytes(250000, &pulse->sample_spec);
	buffer_attr.prebuf = (uint32_t) -1;
	buffer_attr.minreq = (uint32_t) -1;
	buffer_attr.fragsize = (uint32_t) -1;
	if (pa_stream_connect_playback(pulse->stream,
		pulse->device[0] ? pulse->device : NULL, &buffer_attr,
		PA_STREAM_ADJUST_LATENCY | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE,
		NULL, NULL) < 0)
	{
		pa_threaded_mainloop_unlock(pulse->mainloop);
		DEBUG_WARN("pa_stream_connect_playback failed (%d)",
			pa_context_errno(pulse->context));
		return False;
	}

	for (;;)
	{
		state = pa_stream_get_state(pulse->stream);
		if (state == PA_STREAM_READY)
			break;
		if (!PA_STREAM_IS_GOOD(state))
		{
			DEBUG_WARN("bad stream state (%d)",
				pa_context_errno(pulse->context));
			break;
		}
		pa_threaded_mainloop_wait(pulse->mainloop);
	}
	pa_threaded_mainloop_unlock(pulse->mainloop);
	if (state == PA_STREAM_READY)
	{
		DEBUG_DVC("connected");
		return True;
	}
	else
	{
		tsmf_pulse_close_stream(pulse);
		return False;
	}
}

static boolean tsmf_pulse_set_format(ITSMFAudioDevice* audio,
	uint32 sample_rate, uint32 channels, uint32 bits_per_sample)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) audio;

	DEBUG_DVC("sample_rate %d channels %d bits_per_sample %d",
		sample_rate, channels, bits_per_sample);

	pulse->sample_spec.rate = sample_rate;
	pulse->sample_spec.channels = channels;
	pulse->sample_spec.format = PA_SAMPLE_S16LE;

	return tsmf_pulse_open_stream(pulse);
}

static boolean tsmf_pulse_play(ITSMFAudioDevice* audio, uint8* data, uint32 data_size)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) audio;
	uint8* src;
	int len;
	int ret;

	DEBUG_DVC("data_size %d", data_size);

	if (pulse->stream)
	{
		pa_threaded_mainloop_lock(pulse->mainloop);

		src = data;
		while (data_size > 0)
		{
			while ((len = pa_stream_writable_size(pulse->stream)) == 0)
			{
				DEBUG_DVC("waiting");
				pa_threaded_mainloop_wait(pulse->mainloop);
			}
			if (len < 0)
				break;
			if (len > data_size)
				len = data_size;
			ret = pa_stream_write(pulse->stream, src, len, NULL, 0LL, PA_SEEK_RELATIVE);
			if (ret < 0)
			{
				DEBUG_DVC("pa_stream_write failed (%d)",
					pa_context_errno(pulse->context));
				break;
			}
			src += len;
			data_size -= len;
		}

		pa_threaded_mainloop_unlock(pulse->mainloop);
	}
	xfree(data);

	return True;
}

static uint64 tsmf_pulse_get_latency(ITSMFAudioDevice* audio)
{
	pa_usec_t usec;
	uint64 latency = 0;
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) audio;

	if (pulse->stream && pa_stream_get_latency(pulse->stream, &usec, NULL) == 0)
	{
		latency = ((uint64)usec) * 10LL;
	}
	return latency;
}

static void tsmf_pulse_flush(ITSMFAudioDevice* audio)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) audio;

	pa_threaded_mainloop_lock(pulse->mainloop);
	tsmf_pulse_wait_for_operation(pulse,
		pa_stream_flush(pulse->stream, tsmf_pulse_stream_success_callback, pulse));
	pa_threaded_mainloop_unlock(pulse->mainloop);
}

static void tsmf_pulse_free(ITSMFAudioDevice* audio)
{
	TSMFPulseAudioDevice* pulse = (TSMFPulseAudioDevice*) audio;

	DEBUG_DVC("");

	tsmf_pulse_close_stream(pulse);
	if (pulse->mainloop)
	{
		pa_threaded_mainloop_stop(pulse->mainloop);
	}
	if (pulse->context)
	{
		pa_context_disconnect(pulse->context);
		pa_context_unref(pulse->context);
		pulse->context = NULL;
	}
	if (pulse->mainloop)
	{
		pa_threaded_mainloop_free(pulse->mainloop);
		pulse->mainloop = NULL;
	}
	xfree(pulse);
}

ITSMFAudioDevice* TSMFAudioDeviceEntry(void)
{
	TSMFPulseAudioDevice* pulse;

	pulse = xnew(TSMFPulseAudioDevice);

	pulse->iface.Open = tsmf_pulse_open;
	pulse->iface.SetFormat = tsmf_pulse_set_format;
	pulse->iface.Play = tsmf_pulse_play;
	pulse->iface.GetLatency = tsmf_pulse_get_latency;
	pulse->iface.Flush = tsmf_pulse_flush;
	pulse->iface.Free = tsmf_pulse_free;

	return (ITSMFAudioDevice*) pulse;
}

