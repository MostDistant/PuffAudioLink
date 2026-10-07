#pragma once
#include "plugin.hpp"
#include "FrameClock.hpp"
#include "wail_link.h"


// Shared Link timing for the Most Distant modules.

// Phase lens for Link Audio buffer stamps (beats; beat values are absolute).
// Matches WAIL Send / WAIL Receive (plugins/wail_send.c LB_QUANTUM).
constexpr double kStampQuantum = 4.0;

// Link Audio is 48 kHz-only on the wire; Rack runs at whatever the user picks.
constexpr int kLinkAudioRate = 48000;

// Device output latency beyond the engine block itself. A frame computed at
// block start reaches the DAC one engine block later plus the device's own
// buffering, which Rack does not report. One constant, field-measurable per
// setup — the same class as WAIL Send's LBS_STAMP_AHEAD_US.
constexpr double kDeviceLatencyUs = 5000.0;

// converterLatency is a SampleRateConverter's group delay in its input frames
// (0 when the rates match and it simply copies). A frame fed in emerges this
// many input frames late, so stamps and alignment both account for it.
inline int converterLatency(dsp::SampleRateConverter<2>& c) {
	return c.st ? speex_resampler_get_input_latency(c.st) : 0;
}

// observeEngine feeds the frame clock at the first frame of each engine block,
// the one frame whose compute time the wall clock actually tells us (see
// FrameClock). Call once per process().
inline void observeEngine(FrameClock& clock, lb_link* link, const Module::ProcessArgs& args) {
	if (args.frame == APP->engine->getBlockFrame() || !clock.ready())
		clock.observe(args.frame, lb_clock_micros(link), args.sampleRate);
}

// playMicros is the Link time at which an engine frame is heard: Link's
// convention for both stamping published audio and placing clock edges.
inline int64_t playMicros(const FrameClock& clock, int64_t frame) {
	return (int64_t) (clock.micros(frame) + APP->engine->getBlockDuration() * 1e6 + kDeviceLatencyUs);
}
