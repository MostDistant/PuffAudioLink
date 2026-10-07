// Unit tests for the Rack modules' pure timing logic (no Rack SDK, no Link).
// Run with `make test` from rack/.
#include "../src/FrameClock.hpp"
#include "../src/StampAligner.hpp"
#include "../src/RoomSlots.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                                \
	do {                                                                           \
		if (!(cond)) {                                                             \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
			failures++;                                                            \
		}                                                                          \
	} while (0)

#define CHECK_NEAR(a, b, tol)                                                      \
	do {                                                                           \
		double a_ = (double) (a), b_ = (double) (b);                               \
		if (std::fabs(a_ - b_) > (tol)) {                                          \
			std::printf("  FAIL %s:%d: %s = %.3f, want %.3f ± %.3f\n", __FILE__,    \
			            __LINE__, #a, a_, b_, (double) (tol));                     \
			failures++;                                                            \
		}                                                                          \
	} while (0)

// --- FrameClock ---

static void testFrameClockFirstObservationAnchors() {
	FrameClock c;
	CHECK(!c.ready());
	c.observe(1000, 5000000, 48000.0);
	CHECK(c.ready());
	CHECK_NEAR(c.micros(1000), 5000000, 0.5);
	CHECK_NEAR(c.micros(1000 + 48000), 6000000, 0.5);
}

// Engine blocks are computed in bursts at audio-callback time; observations at
// each block start carry OS scheduling jitter. The smoothed mapping must sit
// close to the true timeline and must not chase the jitter.
static void testFrameClockSmoothsJitter() {
	FrameClock c;
	std::mt19937 rng(42);
	std::uniform_real_distribution<double> jitter(-500.0, 500.0);
	const double sr = 48000.0;
	const int block = 256;
	const double t0 = 1e9;
	double worst = 0;
	for (int b = 0; b < 4000; b++) {
		int64_t frame = (int64_t) b * block;
		double truth = t0 + frame * 1e6 / sr;
		c.observe(frame, (int64_t) (truth + jitter(rng)), sr);
		if (b > 500)
			worst = std::max(worst, std::fabs(c.micros(frame) - truth));
	}
	CHECK(worst < 150.0);
}

// The audio clock and Link's clock drift apart by tens of ppm; the mapping must
// follow without accumulating error.
static void testFrameClockFollowsDrift() {
	FrameClock c;
	const double sr = 48000.0;
	const int block = 256;
	const double ppm = 100.0;
	double worst = 0;
	for (int b = 0; b < 20000; b++) { // ~107 s
		int64_t frame = (int64_t) b * block;
		double truth = 2e9 + frame * 1e6 / sr * (1.0 + ppm * 1e-6);
		c.observe(frame, (int64_t) truth, sr);
		if (b > 1000)
			worst = std::max(worst, std::fabs(c.micros(frame) - truth));
	}
	CHECK(worst < 100.0);
}

// An engine stall (patch load, device restart) is a real timeline jump, not
// jitter: re-anchor immediately instead of slewing for seconds.
static void testFrameClockReanchorsOnStall() {
	FrameClock c;
	c.observe(0, 1000000, 48000.0);
	c.observe(256, 1000000 + 5333, 48000.0);
	c.observe(512, 1000000 + 10667 + 200000, 48000.0); // 200 ms stall
	CHECK_NEAR(c.micros(512), 1000000 + 10667 + 200000, 1.0);
}

static void testFrameClockReanchorsOnRateChange() {
	FrameClock c;
	c.observe(0, 1000000, 48000.0);
	c.observe(4410, 3000000, 44100.0);
	CHECK_NEAR(c.micros(4410), 3000000, 0.5);
	CHECK_NEAR(c.micros(4410 + 44100), 4000000, 0.5);
}

// --- StampAligner ---

static std::vector<int16_t> ramp(uint32_t frames, int16_t start) {
	std::vector<int16_t> v(frames * 2);
	for (uint32_t i = 0; i < frames; i++)
		v[2 * i] = v[2 * i + 1] = (int16_t) (start + (int16_t) i);
	return v;
}

// A buffer whose stamp says "play at T", read at exactly T: no pad, no skip.
static void testAlignerOnTime() {
	StampAligner a;
	auto pcm = ramp(480, 100);
	a.push(pcm.data(), 480, 1000000, 48000);
	CHECK(a.align(1000000) == 0);
	CHECK(a.available() == 480);
	CHECK(a.skipped() == 0);
}

// Read 10 ms early: pad with silence until the stamp comes due (minus the
// jitter deadband).
static void testAlignerPadsWhenEarly() {
	StampAligner a;
	auto pcm = ramp(960, 0);
	a.push(pcm.data(), 960, 1010000, 48000);
	uint32_t pad = a.align(1000000);
	CHECK_NEAR(pad, 480 - StampAligner::kDeadband, 1);
}

// Read 5 ms late: skip the stale frames so playback lands on the grid.
static void testAlignerSkipsWhenLate() {
	StampAligner a;
	auto pcm = ramp(960, 0);
	a.push(pcm.data(), 960, 1000000, 48000);
	CHECK(a.align(1005000) == 0);
	CHECK_NEAR(a.skipped(), 240 - StampAligner::kDeadband, 1);
	float l = 0, r = 0;
	a.peek(&l, &r, 1, 0);
	CHECK_NEAR(l * 32768.0f, 240 - StampAligner::kDeadband, 1.5);
}

// Small jitter inside the deadband is absorbed — no clicks from ±0.5 ms noise.
static void testAlignerDeadbandAbsorbsJitter() {
	StampAligner a;
	auto pcm = ramp(960, 0);
	a.push(pcm.data(), 960, 1000000, 48000);
	CHECK(a.align(1000000 - 500) == 0);
	CHECK(a.align(1000000 + 500) == 0);
	CHECK(a.skipped() == 0);
}

// Contiguous stream consumed in step with its stamps never pads or skips.
static void testAlignerSteadyStream() {
	StampAligner a;
	const uint32_t chunk = 128;
	int64_t t = 5000000;
	uint32_t pads = 0;
	for (int i = 0; i < 200; i++) {
		auto pcm = ramp(chunk, 0);
		a.push(pcm.data(), chunk, t + (int64_t) i * chunk * 1000000 / 48000, 48000);
	}
	for (int i = 0; i < 150; i++) {
		int64_t now = t + (int64_t) i * chunk * 1000000 / 48000;
		pads += a.align(now);
		a.consume(chunk);
	}
	CHECK(pads == 0);
	CHECK(a.skipped() == 0);
}

static void testAlignerPeekPastTailIsSilence() {
	StampAligner a;
	auto pcm = ramp(4, 1000);
	a.push(pcm.data(), 4, 0, 48000);
	float l[8], r[8];
	uint32_t got = a.peek(l, r, 8, 0);
	CHECK(got == 4);
	CHECK_NEAR(l[3] * 32768.0f, 1003, 0.5);
	CHECK(l[4] == 0.0f && r[7] == 0.0f);
}

static void testAlignerResetClears() {
	StampAligner a;
	auto pcm = ramp(100, 0);
	a.push(pcm.data(), 100, 0, 48000);
	a.reset();
	CHECK(a.available() == 0);
	CHECK(a.align(0) == 0);
}


// Early arrivals (WAIL room channels are stamped an interval ahead) play
// exactly on their stamp: no added delay.
static void testAlignerEarlyArrivalKeepsZeroDelay() {
	StampAligner a;
	a.noteArrival(1100000, 1000000);
	CHECK(a.delayUs() == 0);
}

// A live LAN publisher stamps buffers ~now, so they land late. Rather than
// skipping every one (chopped audio), the delay grows to cover the lateness.
static void testAlignerLateArrivalRaisesDelay() {
	StampAligner a;
	a.noteArrival(1000000, 1003000);
	CHECK_NEAR(a.delayUs(), 3000 + StampAligner::kLateMarginUs, 0.5);
	a.noteArrival(2000000, 2001000); // less late: delay holds
	CHECK_NEAR(a.delayUs(), 3000 + StampAligner::kLateMarginUs, 0.5);
}

static void testAlignerDelayIsCapped() {
	StampAligner a;
	a.noteArrival(0, 10000000);
	CHECK(a.delayUs() == StampAligner::kMaxDelayUs);
}

// With the delay covering the lateness, a steady late stream plays through
// without skipping.
static void testAlignerLateStreamPlaysWithoutSkips() {
	StampAligner a;
	const uint32_t chunk = 128;
	const int64_t chunkUs = chunk * 1000000 / 48000;
	const int64_t lateUs = 4000;
	int64_t t0 = 3000000;
	for (int i = 0; i < 100; i++) {
		int64_t stamp = t0 + i * chunkUs;
		int64_t now = stamp + lateUs; // arrives (and is drained) late
		auto pcm = ramp(chunk, 0);
		a.noteArrival(stamp, now);
		a.push(pcm.data(), chunk, stamp, 48000);
		uint32_t pad = a.align(now);
		a.consume(chunk > pad ? chunk - pad : 0);
	}
	CHECK(a.skipped() == 0);
}

static void testAlignerResetClearsDelay() {
	StampAligner a;
	a.noteArrival(0, 5000);
	a.reset();
	CHECK(a.delayUs() == 0);
}


// --- RoomSlots ---

static std::vector<RoomSlots::Seen> seen(std::initializer_list<RoomSlots::Seen> l) {
	return std::vector<RoomSlots::Seen>(l);
}

static int countKind(const std::vector<RoomSlots::Action>& acts, RoomSlots::Action::Kind k) {
	int n = 0;
	for (auto& a : acts)
		n += a.kind == k;
	return n;
}

// Room channels ("WAIL · …") take slots in discovery order; anything else on
// the LAN (a Live track, a Rack Send) is ignored.
static void testSlotsAssignRoomChannelsOnly() {
	RoomSlots r;
	auto acts = r.poll(seen({{1, "WAIL · alice · Bass"}, {2, "Live Track"}, {3, "WAIL · bob · Keys"}}));
	CHECK(countKind(acts, RoomSlots::Action::Subscribe) == 2);
	CHECK(r.id(0) == 1 && r.id(1) == 3);
	CHECK(r.name(0) == "WAIL · alice · Bass");
	CHECK(!r.used(2));
	CHECK(r.poll(seen({{1, "WAIL · alice · Bass"}, {2, "Live Track"}, {3, "WAIL · bob · Keys"}})).empty());
}

// The WAIL app renames a room channel in place once the sender's stream name
// arrives; the id survives, so the slot (and its patch cable) must too.
static void testSlotsFollowRenameById() {
	RoomSlots r;
	r.poll(seen({{7, "WAIL · alice · Stream 1"}}));
	auto acts = r.poll(seen({{7, "WAIL · alice · Bass"}}));
	CHECK(acts.size() == 1 && acts[0].kind == RoomSlots::Action::Rename && acts[0].slot == 0);
	CHECK(r.name(0) == "WAIL · alice · Bass");
}

// A channel gone for kGonePolls polls frees its slot — not one poll sooner
// (discovery blips must not drop a stream).
static void testSlotsFreeAfterGonePolls() {
	RoomSlots r;
	r.poll(seen({{1, "WAIL · a · x"}}));
	for (int i = 0; i < RoomSlots::kGonePolls - 1; i++)
		CHECK(r.poll(seen({})).empty());
	auto acts = r.poll(seen({}));
	CHECK(acts.size() == 1 && acts[0].kind == RoomSlots::Action::Unsubscribe && acts[0].slot == 0);
	CHECK(!r.used(0));
}

static void testSlotsReappearResetsMisses() {
	RoomSlots r;
	r.poll(seen({{1, "WAIL · a · x"}}));
	for (int i = 0; i < RoomSlots::kGonePolls - 1; i++)
		r.poll(seen({}));
	r.poll(seen({{1, "WAIL · a · x"}}));
	for (int i = 0; i < RoomSlots::kGonePolls - 1; i++)
		CHECK(r.poll(seen({})).empty());
}

// Freed slots are reused, and nobody else moves: a cable patched to slot 3
// keeps hearing the same player when slot 1 leaves.
static void testSlotsStablePositions() {
	RoomSlots r;
	r.poll(seen({{1, "WAIL · a · x"}, {2, "WAIL · b · x"}, {3, "WAIL · c · x"}}));
	for (int i = 0; i < RoomSlots::kGonePolls; i++)
		r.poll(seen({{1, "WAIL · a · x"}, {3, "WAIL · c · x"}}));
	CHECK(!r.used(1) && r.id(2) == 3);
	r.poll(seen({{1, "WAIL · a · x"}, {3, "WAIL · c · x"}, {4, "WAIL · d · x"}}));
	CHECK(r.id(1) == 4 && r.id(2) == 3);
}

// Two WAILs bridging one room publish the same stream under the same name:
// first wins, so it doesn't play twice.
static void testSlotsDuplicateNameFirstWins() {
	RoomSlots r;
	auto acts = r.poll(seen({{1, "WAIL · a · x"}, {2, "WAIL · a · x"}}));
	CHECK(countKind(acts, RoomSlots::Action::Subscribe) == 1);
	CHECK(r.id(0) == 1 && !r.used(1));
}

// Duplicates that only collide after a rename converge by freeing the later.
static void testSlotsDuplicateAfterRenameConverges() {
	RoomSlots r;
	r.poll(seen({{1, "WAIL · a · x"}, {2, "WAIL · a · Stream 1"}}));
	auto acts = r.poll(seen({{1, "WAIL · a · x"}, {2, "WAIL · a · x"}}));
	CHECK(countKind(acts, RoomSlots::Action::Unsubscribe) == 1);
	CHECK(r.used(0) && !r.used(1));
}

static void testSlotsCapacity() {
	RoomSlots r;
	std::vector<RoomSlots::Seen> many;
	for (int i = 0; i < RoomSlots::kSlots + 3; i++)
		many.push_back({(uint64_t) (100 + i), "WAIL · p" + std::to_string(i) + " · x"});
	auto acts = r.poll(many);
	CHECK(countKind(acts, RoomSlots::Action::Subscribe) == RoomSlots::kSlots);
	CHECK(r.highest() == RoomSlots::kSlots - 1);
}

// A failed subscribe hands the slot back so a later poll can retry.
static void testSlotsReleaseRetries() {
	RoomSlots r;
	r.poll(seen({{1, "WAIL · a · x"}}));
	r.release(0);
	CHECK(!r.used(0));
	CHECK(countKind(r.poll(seen({{1, "WAIL · a · x"}})), RoomSlots::Action::Subscribe) == 1);
}

static void testSlotsHighestEmpty() {
	RoomSlots r;
	CHECK(r.highest() == -1);
}

int main() {
	struct {
		const char* name;
		void (*fn)();
	} tests[] = {
	    {"FrameClockFirstObservationAnchors", testFrameClockFirstObservationAnchors},
	    {"FrameClockSmoothsJitter", testFrameClockSmoothsJitter},
	    {"FrameClockFollowsDrift", testFrameClockFollowsDrift},
	    {"FrameClockReanchorsOnStall", testFrameClockReanchorsOnStall},
	    {"FrameClockReanchorsOnRateChange", testFrameClockReanchorsOnRateChange},
	    {"AlignerOnTime", testAlignerOnTime},
	    {"AlignerPadsWhenEarly", testAlignerPadsWhenEarly},
	    {"AlignerSkipsWhenLate", testAlignerSkipsWhenLate},
	    {"AlignerDeadbandAbsorbsJitter", testAlignerDeadbandAbsorbsJitter},
	    {"AlignerSteadyStream", testAlignerSteadyStream},
	    {"AlignerPeekPastTailIsSilence", testAlignerPeekPastTailIsSilence},
	    {"AlignerResetClears", testAlignerResetClears},
	    {"AlignerEarlyArrivalKeepsZeroDelay", testAlignerEarlyArrivalKeepsZeroDelay},
	    {"AlignerLateArrivalRaisesDelay", testAlignerLateArrivalRaisesDelay},
	    {"AlignerDelayIsCapped", testAlignerDelayIsCapped},
	    {"AlignerLateStreamPlaysWithoutSkips", testAlignerLateStreamPlaysWithoutSkips},
	    {"AlignerResetClearsDelay", testAlignerResetClearsDelay},
	    {"SlotsAssignRoomChannelsOnly", testSlotsAssignRoomChannelsOnly},
	    {"SlotsFollowRenameById", testSlotsFollowRenameById},
	    {"SlotsFreeAfterGonePolls", testSlotsFreeAfterGonePolls},
	    {"SlotsReappearResetsMisses", testSlotsReappearResetsMisses},
	    {"SlotsStablePositions", testSlotsStablePositions},
	    {"SlotsDuplicateNameFirstWins", testSlotsDuplicateNameFirstWins},
	    {"SlotsDuplicateAfterRenameConverges", testSlotsDuplicateAfterRenameConverges},
	    {"SlotsCapacity", testSlotsCapacity},
	    {"SlotsReleaseRetries", testSlotsReleaseRetries},
	    {"SlotsHighestEmpty", testSlotsHighestEmpty},
	};
	for (auto& t : tests) {
		int before = failures;
		t.fn();
		std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
	}
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all passed\n");
	return 0;
}
