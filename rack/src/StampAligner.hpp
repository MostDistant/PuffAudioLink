#pragma once
#include <cstdint>
#include <cstring>


// StampAligner is the Link Audio Receive module's playout buffer: a ring of
// received 48 kHz stereo PCM plus a parallel ring of anchors (ring frame ↔
// session-clock µs at which that frame should play). align() pads when the
// stream is early and skips when it is late, with a small deadband so network
// and scheduling jitter is absorbed instead of turned into clicks.
//
// This is WAIL Receive's slot render path (plugins/wail_recv.c: ring_push,
// slot_align) in its session-clock ("mono") mode — Rack has no host transport,
// so the module's own Link peer is the only clock. Single-threaded: owned by
// the engine thread.
//
// Playback latency is the receiver's choice in Link Audio. Stamps are honored
// exactly when buffers arrive early — WAIL room channels are stamped an
// interval ahead, so they land on the grid. A live LAN publisher stamps ~now,
// so its buffers arrive late; skipping each one would chop the audio, so the
// delay grows (max-hold, capped) to cover the observed lateness plus a margin,
// and is cleared by reset() when the subscription changes.
struct StampAligner {
	static constexpr uint32_t kRingFrames = 32768; // power of two; ~0.68 s @48k
	static constexpr uint32_t kAnchors = 512;
	static constexpr int32_t kDeadband = 32; // ~0.67 ms @48k
	static constexpr int64_t kLateMarginUs = 3000;
	static constexpr int64_t kMaxDelayUs = 250000;

	void reset() {
		head = tail = 0;
		arHead = arTail = 0;
		hasLast = false;
		rate = 48000;
		skippedFrames = 0;
		delay = 0;
	}

	// noteArrival records when a buffer stamped playMicros was drained,
	// relative to the output's play time nowMicros, and grows the delay if it
	// came in late.
	void noteArrival(int64_t playMicros, int64_t nowMicros) {
		int64_t want = nowMicros - playMicros + kLateMarginUs;
		if (nowMicros - playMicros <= 0 || want <= delay)
			return;
		delay = want < kMaxDelayUs ? want : kMaxDelayUs;
	}

	int64_t delayUs() const {
		return delay;
	}

	// push appends interleaved int16 stereo frames whose first frame should
	// play at playMicros (session clock). Overflow is dropped.
	void push(const int16_t* pcm, uint32_t frames, int64_t playMicros, uint32_t sampleRate) {
		uint32_t space = kRingFrames - (tail - head);
		uint32_t put = frames < space ? frames : space;
		uint32_t frameStart = tail;
		for (uint32_t i = 0; i < put; i++) {
			uint32_t idx = (tail + i) % kRingFrames;
			samples[2 * idx] = pcm[2 * i];
			samples[2 * idx + 1] = pcm[2 * i + 1];
		}
		tail += put;
		if (arTail - arHead == kAnchors)
			arHead++; // drop the oldest (stale anyway)
		Anchor& a = anchors[arTail % kAnchors];
		a.micros = playMicros;
		a.frame = frameStart;
		arTail++;
		rate = sampleRate ? sampleRate : 48000;
	}

	// align compares the ring head with where the stamps say playback should
	// be at nowMicros. Late → skips stale frames now. Early → returns how many
	// frames of silence to emit before the head (the caller caps it to its
	// block). Recomputed every block, so a partial pad simply continues.
	uint32_t align(int64_t nowMicros) {
		nowMicros -= delay;
		while (arTail - arHead > 1 &&
		       (int32_t) (anchors[(arHead + 1) % kAnchors].frame - head) <= 0) {
			last = anchors[arHead % kAnchors];
			hasLast = true;
			arHead++;
		}

		Anchor a;
		bool have = false;
		for (uint32_t i = arTail; i > arHead; i--) {
			const Anchor& c = anchors[(i - 1) % kAnchors];
			if (c.micros <= nowMicros) {
				a = c;
				have = true;
				break;
			}
		}
		if (!have && arHead != arTail) {
			a = anchors[arHead % kAnchors];
			have = true;
		}
		if (!have) {
			if (!hasLast)
				return 0;
			a = last;
		}

		int64_t target = (int64_t) a.frame + (nowMicros - a.micros) * (int64_t) rate / 1000000;
		int32_t err = (int32_t) (head - (uint32_t) target);
		if (err > kDeadband)
			return (uint32_t) (err - kDeadband);
		if (err < -kDeadband) {
			uint32_t skip = (uint32_t) (-err - kDeadband);
			uint32_t avail = available();
			if (skip > avail)
				skip = avail;
			head += skip;
			skippedFrames += skip;
		}
		return 0;
	}

	uint32_t available() const {
		return tail - head;
	}

	// peek copies up to n frames starting `offset` frames past the head into
	// l/r as floats in [-1, 1), filling past the tail with silence. Returns the
	// number of real frames copied.
	uint32_t peek(float* l, float* r, uint32_t n, uint32_t offset) const {
		uint32_t avail = available();
		uint32_t got = offset < avail ? avail - offset : 0;
		if (got > n)
			got = n;
		for (uint32_t i = 0; i < got; i++) {
			uint32_t idx = (head + offset + i) % kRingFrames;
			l[i] = (float) samples[2 * idx] / 32768.0f;
			r[i] = (float) samples[2 * idx + 1] / 32768.0f;
		}
		for (uint32_t i = got; i < n; i++)
			l[i] = r[i] = 0.0f;
		return got;
	}

	void consume(uint32_t n) {
		uint32_t avail = available();
		head += n < avail ? n : avail;
	}

	uint64_t skipped() const {
		return skippedFrames;
	}

	uint32_t sampleRate() const {
		return rate;
	}

private:
	struct Anchor {
		int64_t micros = 0;
		uint32_t frame = 0;
	};

	int16_t samples[kRingFrames * 2] = {};
	uint32_t head = 0, tail = 0;
	Anchor anchors[kAnchors];
	uint32_t arHead = 0, arTail = 0;
	Anchor last;
	bool hasLast = false;
	uint32_t rate = 48000;
	uint64_t skippedFrames = 0;
	int64_t delay = 0;
};
