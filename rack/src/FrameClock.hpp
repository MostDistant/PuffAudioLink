#pragma once
#include <cmath>
#include <cstdint>


// FrameClock maps Rack engine frames onto Ableton Link's clock (µs).
//
// Rack computes audio in bursts: at each audio callback the engine steps a
// whole block of frames as fast as it can, so the wall clock read inside
// process() says almost nothing about *which* frame is being computed. The
// one honest observation is at the first frame of each engine block (Rack 2's
// Engine::getBlockFrame()): that frame is computed at callback time. Feed those
// observations here; micros(frame) then gives a smooth, linear mapping for
// every frame in between.
//
// The offset is an EMA over the block-start residuals, so OS scheduling jitter
// is averaged out while the audio-vs-Link clock drift (tens of ppm) is followed
// with a lag of a few tens of µs. A residual beyond kStallUs is a real timeline
// jump (engine stall, device restart), not jitter, and re-anchors at once; so
// does a sample-rate change.
//
// micros() is the time a frame is *computed*; the time it reaches the DAC is
// one output pipeline later (see kOutputLatencyUs in LinkTime.hpp).
struct FrameClock {
	static constexpr double kAlpha = 0.01;
	static constexpr double kStallUs = 50000.0;

	void observe(int64_t frame, int64_t nowMicros, double sampleRate) {
		if (sampleRate <= 0.0)
			return;
		double residual = (double) nowMicros - (double) frame * 1e6 / sampleRate;
		if (!have || sampleRate != sr || std::fabs(residual - offsetUs) > kStallUs) {
			offsetUs = residual;
			sr = sampleRate;
			have = true;
			return;
		}
		offsetUs += kAlpha * (residual - offsetUs);
	}

	bool ready() const {
		return have;
	}

	void reset() {
		have = false;
	}

	double micros(int64_t frame) const {
		return (double) frame * 1e6 / sr + offsetUs;
	}

private:
	bool have = false;
	double sr = 0.0;
	double offsetUs = 0.0;
};
