#pragma once
#include "LinkTime.hpp"
#include "StampAligner.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>


// RecvSlot is one subscribed Link Audio channel's playout, shared by Link Audio
// Receive (one slot) and Link Audio Receive 16 (sixteen).
//
// Threading (mirrors plugins/wail_recv.c): the module's manager thread creates
// and destroys sources; the engine thread owns everything else. The handoff is
// `active`: the manager swaps a source in or out with retireSource(), and
// render() picks the change up at its next block. The engine thread drains the
// source, stamps each buffer with its play time on our Link timeline, aligns
// playout to those stamps (StampAligner), and converts 48 kHz to the engine
// rate a kOutBlock-frame block at a time.
struct RecvSlot {
	static constexpr int kOutBlock = 32; // engine frames rendered per block
	static constexpr int kInMax = 256;   // 48k frames fed to the converter per block

	std::atomic<lb_source*> active{nullptr};

	RecvSlot() {
		src.setChannels(2);
	}

	// render produces the next kOutBlock engine frames. `state` was captured
	// for this block; `now` is the play time of its first frame. [engine]
	void render(const Module::ProcessArgs& args, lb_state* state, int64_t now) {
		lb_source* s = active.load(std::memory_order_acquire);
		if (s != engineSource) {
			engineSource = s;
			aligner.reset();
			lastAudioFrame = -1;
		}
		if (srcRate != args.sampleRate) {
			src.setRates(kLinkAudioRate, (int) args.sampleRate);
			srcRate = args.sampleRate;
		}
		if (!engineSource) {
			std::memset(out, 0, sizeof(out));
			return;
		}
		// What we feed the converter now is heard one converter delay later.
		const int64_t feedAt = now + (int64_t) converterLatency(src) * 1000000 / kLinkAudioRate;

		lb_source_buffer b;
		while (lb_source_pop_at(engineSource, &b, kStampQuantum, state)) {
			if (b.num_frames == 0 || b.begin_beat == 0)
				continue; // unmappable (cross-session) buffer
			const int64_t at = lb_time_at_beat(state, b.begin_beat, kStampQuantum);
			aligner.noteArrival(at, feedAt);
			aligner.push(b.samples, (uint32_t) b.num_frames, at, b.sample_rate);
			lastAudioFrame = args.frame;
		}

		// 48k frames this block needs, plus slack so the converter is never
		// short (it consumes only what it uses).
		int want = (int) std::ceil(kOutBlock * (double) kLinkAudioRate / args.sampleRate) + 8;
		if (want > kInMax)
			want = kInMax;
		uint32_t pad = aligner.align(feedAt);
		if (pad > (uint32_t) want)
			pad = want;
		for (uint32_t i = 0; i < pad; i++)
			inL[i] = inR[i] = 0.f;
		aligner.peek(inL + pad, inR + pad, want - pad, 0);
		for (int i = 0; i < want; i++) {
			inInter[2 * i] = inL[i];
			inInter[2 * i + 1] = inR[i];
		}

		int inFrames = want;
		int outFrames = kOutBlock;
		src.process(inInter, 2, &inFrames, out, 2, &outFrames);
		for (int i = outFrames; i < kOutBlock; i++)
			out[2 * i] = out[2 * i + 1] = 0.f;
		// Consumed frames past the pad came from the ring (or ran past its tail
		// as silence, which consume() clamps).
		if ((uint32_t) inFrames > pad)
			aligner.consume(inFrames - pad);
	}

	// Sample i of the last rendered block, in volts. [engine]
	float left(int i) const {
		return out[2 * i] * 5.f;
	}
	float right(int i) const {
		return out[2 * i + 1] * 5.f;
	}

	bool subscribed() const {
		return engineSource != nullptr;
	}

	// hearing: audio arrived within the last half second. [engine]
	bool hearing(int64_t frame, float sampleRate) const {
		return engineSource && lastAudioFrame >= 0 && frame - lastAudioFrame < (int64_t) sampleRate / 2;
	}

private:
	lb_source* engineSource = nullptr;
	StampAligner aligner;
	dsp::SampleRateConverter<2> src;
	float srcRate = 0.f;
	float inInter[kInMax * 2];
	float inL[kInMax], inR[kInMax];
	float out[kOutBlock * 2] = {};
	int64_t lastAudioFrame = -1;
};


// retireSource swaps `next` into a slot and destroys the previous source once
// the engine can no longer be using it: after the engine has started at least
// two more blocks (`epoch`, bumped per block), or after a timeout when the
// engine is not running at all. [manager thread]
inline void retireSource(RecvSlot& slot, const std::atomic<uint64_t>& epoch, lb_source* next) {
	uint64_t start = epoch.load(std::memory_order_acquire);
	lb_source* prev = slot.active.exchange(next, std::memory_order_acq_rel);
	if (!prev)
		return;
	for (int i = 0; i < 20 && epoch.load(std::memory_order_acquire) < start + 2; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	lb_source_destroy(prev);
}
