#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>


// RoomSlots decides which WAIL room channel plays on which of Link Audio
// Receive 16's slots. Pure bookkeeping — the module turns its actions into
// Link Audio subscriptions — ported from WAIL Receive's manager loop
// (plugins/wail_recv.c mgr_main), and with the same rules:
//
//  - Only room-published channels ("WAIL · …") are carried.
//  - Slot identity is the channel id, not its name: the WAIL app renames a room
//    channel in place once the sender's stream name arrives, and the id
//    survives, so the slot (and whatever is patched to it) does too.
//  - A slot is freed only after kGonePolls polls without its channel, so a
//    discovery blip does not drop a stream; freed slots are reused, and no
//    other slot moves.
//  - Names are first-wins: two WAILs bridging one room publish the same stream
//    under the same name, and it must not play twice. Duplicates that only
//    collide after a rename converge by freeing the later slot.
struct RoomSlots {
	static constexpr int kSlots = 16;
	static constexpr int kGonePolls = 6; // ~3 s at the module's 500 ms poll
	static constexpr const char* kRoomPrefix = "WAIL · ";

	struct Seen {
		uint64_t id;
		std::string name;
	};

	struct Action {
		enum Kind { Subscribe, Unsubscribe, Rename } kind;
		int slot;
		uint64_t id;
		std::string name;
	};

	static bool isRoomChannel(const std::string& name) {
		return name.compare(0, std::strlen(kRoomPrefix), kRoomPrefix) == 0;
	}

	// poll takes one discovery snapshot and returns what to change.
	std::vector<Action> poll(const std::vector<Seen>& chans) {
		std::vector<Action> acts;

		for (int i = 0; i < kSlots; i++) {
			if (!slots[i].used)
				continue;
			bool present = false;
			for (const Seen& c : chans)
				present |= c.id == slots[i].id;
			slots[i].misses = present ? 0 : slots[i].misses + 1;
			if (slots[i].misses >= kGonePolls)
				free(i, acts);
		}

		for (const Seen& c : chans) {
			if (!isRoomChannel(c.name))
				continue;
			int owner = -1;
			for (int i = 0; i < kSlots && owner < 0; i++)
				if (slots[i].used && slots[i].id == c.id)
					owner = i;
			if (owner >= 0) {
				if (slots[owner].name != c.name) {
					slots[owner].name = c.name;
					acts.push_back({Action::Rename, owner, c.id, c.name});
				}
				continue;
			}
			bool dupName = false;
			for (int i = 0; i < kSlots; i++)
				dupName |= slots[i].used && slots[i].name == c.name;
			if (dupName)
				continue;
			for (int i = 0; i < kSlots; i++) {
				if (slots[i].used)
					continue;
				slots[i] = Slot{true, c.id, c.name, 0};
				acts.push_back({Action::Subscribe, i, c.id, c.name});
				break;
			}
		}

		for (int i = 0; i < kSlots; i++)
			for (int j = i + 1; j < kSlots; j++)
				if (slots[i].used && slots[j].used && slots[i].name == slots[j].name)
					free(j, acts);

		return acts;
	}

	// release hands back a slot whose subscribe failed, so a later poll retries.
	void release(int i) {
		slots[i] = Slot{};
	}

	bool used(int i) const {
		return slots[i].used;
	}
	uint64_t id(int i) const {
		return slots[i].id;
	}
	const std::string& name(int i) const {
		return slots[i].name;
	}

	// highest is the last used slot, or -1 — the poly outputs' channel count
	// is highest() + 1, so voices never shift when a lower slot frees.
	int highest() const {
		for (int i = kSlots - 1; i >= 0; i--)
			if (slots[i].used)
				return i;
		return -1;
	}

private:
	struct Slot {
		bool used = false;
		uint64_t id = 0;
		std::string name;
		int misses = 0;
	};

	void free(int i, std::vector<Action>& acts) {
		acts.push_back({Action::Unsubscribe, i, slots[i].id, slots[i].name});
		slots[i] = Slot{};
	}

	Slot slots[kSlots];
};
