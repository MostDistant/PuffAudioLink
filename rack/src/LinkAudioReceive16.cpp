#include "plugin.hpp"
#include "RecvSlot.hpp"
#include "RoomSlots.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>


// Link Audio Receive 16 — every WAIL room stream at once: sixteen named stereo
// slots, auto-populated from room-published Link Audio channels ("WAIL · …"),
// plus polyphonic L/R outputs carrying all of them (voice n = slot n).
//
// The Rack counterpart of WAIL Receive (plugins/wail_recv.c): a player joining
// the jam appears on the next free slot with no user action, keeps it across
// the WAIL app's in-place renames, and frees it ~3 s after leaving. Slot
// assignment is RoomSlots; each slot's playout is a RecvSlot, aligned to the
// buffers' beat stamps so the room lands on the grid.
struct LinkAudioReceive16 : Module {
	static constexpr int kSlots = RoomSlots::kSlots;

	enum ParamId {
		PARAMS_LEN
	};
	enum InputId {
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(OUT_L_OUTPUT, kSlots),
		ENUMS(OUT_R_OUTPUT, kSlots),
		POLY_L_OUTPUT,
		POLY_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(AUDIO_LIGHT, kSlots),
		LIGHTS_LEN
	};

	lb_link* link = nullptr;

	// --- engine thread ---
	lb_state* state = nullptr;
	FrameClock clock;
	std::array<RecvSlot, kSlots> slots;
	int outPos = RecvSlot::kOutBlock;
	dsp::ClockDivider lightDivider;
	std::atomic<uint64_t> processEpoch{0};

	// --- manager → engine / UI ---
	std::atomic<int> polyChannels{0};
	std::array<std::atomic<bool>, kSlots> hearing{};

	// --- manager thread ---
	std::thread manager;
	std::atomic<bool> running{false};
	RoomSlots room; // manager-only

	// Stream names per slot ("" = empty), manager writes, UI reads.
	std::mutex mu;
	std::array<std::string, kSlots> names;

	// Jack tooltips name the stream on that slot.
	struct SlotPortInfo : engine::PortInfo {
		LinkAudioReceive16* m = nullptr;
		int slot = 0;
		bool right = false;
		std::string getName() override {
			std::string n = m->label(slot);
			return string::f("%d: %s (%s)", slot + 1, n.empty() ? "empty" : n.c_str(), right ? "R" : "L");
		}
	};

	LinkAudioReceive16() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < kSlots; i++) {
			for (int side = 0; side < 2; side++) {
				SlotPortInfo* pi = configOutput<SlotPortInfo>((side ? OUT_R_OUTPUT : OUT_L_OUTPUT) + i);
				pi->m = this;
				pi->slot = i;
				pi->right = side;
			}
			configLight(AUDIO_LIGHT + i, string::f("Slot %d receiving audio", i + 1));
		}
		configOutput(POLY_L_OUTPUT, "All slots, left (voice n = slot n)");
		configOutput(POLY_R_OUTPUT, "All slots, right (voice n = slot n)");
		lightDivider.setDivision(512);

		link = lb_create(120.0);
		lb_enable(link, true);
		lb_enable_audio(link, true);
		state = lb_state_create();

		running = true;
		manager = std::thread([this]() { manage(); });
	}

	~LinkAudioReceive16() override {
		running = false;
		if (manager.joinable())
			manager.join();
		// The engine has already removed this module, so nothing renders now.
		for (RecvSlot& s : slots)
			lb_source_destroy(s.active.exchange(nullptr));
		if (link) {
			lb_enable(link, false);
			lb_destroy(link);
		}
		lb_release(state);
	}

	// --- manager thread ---

	void manage() {
		while (running) {
			lb_channel_info chans[LB_MAX_CHANNELS];
			size_t n = lb_channels(link, chans, LB_MAX_CHANNELS);
			std::vector<RoomSlots::Seen> seen;
			for (size_t i = 0; i < n; i++)
				seen.push_back({chans[i].id_u64, chans[i].name});

			for (const RoomSlots::Action& a : room.poll(seen)) {
				if (a.kind == RoomSlots::Action::Subscribe) {
					lb_source* s = lb_source_create(link, a.id);
					if (s)
						retireSource(slots[a.slot], processEpoch, s);
					else
						room.release(a.slot); // retried next poll
				}
				else if (a.kind == RoomSlots::Action::Unsubscribe) {
					retireSource(slots[a.slot], processEpoch, nullptr);
				}
			}

			{
				std::lock_guard<std::mutex> lock(mu);
				for (int i = 0; i < kSlots; i++)
					names[i] = room.used(i) ? room.name(i).substr(std::strlen(RoomSlots::kRoomPrefix)) : "";
			}
			polyChannels = room.highest() + 1;

			for (int i = 0; i < 10 && running; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	// --- UI thread ---

	// label is the slot's stream name without the room prefix ("alice · Bass").
	std::string label(int i) {
		std::lock_guard<std::mutex> lock(mu);
		return names[i];
	}

	// --- engine thread ---

	void process(const ProcessArgs& args) override {
		observeEngine(clock, link, args);

		if (outPos >= RecvSlot::kOutBlock) {
			lb_capture_into(link, state);
			const int64_t now = playMicros(clock, args.frame);
			for (RecvSlot& s : slots)
				s.render(args, state, now);
			outPos = 0;
			processEpoch.fetch_add(1, std::memory_order_acq_rel);
		}

		const int poly = std::max(1, polyChannels.load(std::memory_order_relaxed));
		outputs[POLY_L_OUTPUT].setChannels(poly);
		outputs[POLY_R_OUTPUT].setChannels(poly);
		for (int i = 0; i < kSlots; i++) {
			const float l = slots[i].left(outPos);
			const float r = slots[i].right(outPos);
			outputs[OUT_L_OUTPUT + i].setVoltage(l);
			outputs[OUT_R_OUTPUT + i].setVoltage(r);
			if (i < poly) {
				outputs[POLY_L_OUTPUT].setVoltage(l, i);
				outputs[POLY_R_OUTPUT].setVoltage(r, i);
			}
		}
		outPos++;

		if (lightDivider.process()) {
			for (int i = 0; i < kSlots; i++) {
				bool h = slots[i].hearing(args.frame, args.sampleRate);
				hearing[i].store(h, std::memory_order_relaxed);
				lights[AUDIO_LIGHT + i].setBrightness(h ? 1.f : 0.f);
			}
		}
	}
};


// SlotDisplay shows one slot's stream: peer on the first line, stream on the
// second (room names are "peer · stream"); a dim slot number when empty, and
// dimmed text while the stream is silent.
struct SlotDisplay : widget::Widget {
	LinkAudioReceive16* module = nullptr;
	int slot = 0;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.5f);
		nvgFillColor(args.vg, nvgRGB(0x08, 0x08, 0x10));
		nvgFill(args.vg);
		Widget::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			drawText(args);
		Widget::drawLayer(args, layer);
	}

	void drawText(const DrawArgs& args) {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		std::string name = module ? module->label(slot) : (slot == 0 ? "alice · Bass" : "");
		bool live = module ? module->hearing[slot].load(std::memory_order_relaxed) : slot == 0;

		nvgScissor(args.vg, 2, 0, box.size.x - 4, box.size.y);
		nvgFontFaceId(args.vg, font->handle);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		if (name.empty()) {
			nvgFontSize(args.vg, 9.f);
			nvgFillColor(args.vg, nvgRGB(0x3a, 0x3a, 0x50));
			nvgText(args.vg, 4, box.size.y / 2, string::f("%d", slot + 1).c_str(), NULL);
		}
		else {
			std::string peer = name, stream;
			size_t dot = name.find(" · ");
			if (dot != std::string::npos) {
				peer = name.substr(0, dot);
				stream = name.substr(dot + std::strlen(" · "));
			}
			NVGcolor c = live ? nvgRGB(0x6e, 0xe7, 0xb7) : nvgRGB(0x3f, 0x80, 0x68);
			nvgFillColor(args.vg, c);
			nvgFontSize(args.vg, 8.5f);
			if (stream.empty()) {
				nvgText(args.vg, 4, box.size.y / 2, peer.c_str(), NULL);
			}
			else {
				nvgText(args.vg, 4, box.size.y * 0.3f, peer.c_str(), NULL);
				nvgText(args.vg, 4, box.size.y * 0.72f, stream.c_str(), NULL);
			}
		}
		nvgResetScissor(args.vg);
	}
};


struct LinkAudioReceive16Widget : ModuleWidget {
	LinkAudioReceive16Widget(LinkAudioReceive16* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LinkAudioReceive16.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float mid = 45.72f;
		addChild(createLabel(Vec(mid, 8.0), "LINK AUDIO RECEIVE 16", 10.f));
		addChild(createLabel(Vec(mid, 12.5), "WAIL ROOM STREAMS", 7.f, nvgRGB(0x7a, 0x7a, 0x95)));

		// Two columns of eight: [light][name display] L R
		for (int i = 0; i < LinkAudioReceive16::kSlots; i++) {
			const float x0 = i < 8 ? 2.5f : 47.0f;
			const float y = 21.f + (i % 8) * 11.f;
			addChild(createLightCentered<TinyLight<GreenLight>>(mm2px(Vec(x0 + 1.2f, y)), module, LinkAudioReceive16::AUDIO_LIGHT + i));
			SlotDisplay* d = new SlotDisplay;
			d->module = module;
			d->slot = i;
			d->box.size = mm2px(Vec(21.5f, 8.f));
			d->box.pos = mm2px(Vec(x0 + 2.8f, y - 4.f));
			addChild(d);
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x0 + 30.0f, y)), module, LinkAudioReceive16::OUT_L_OUTPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x0 + 39.0f, y)), module, LinkAudioReceive16::OUT_R_OUTPUT + i));
		}
		addChild(createLabel(Vec(32.5f, 15.8f), "L", 7.f));
		addChild(createLabel(Vec(41.5f, 15.8f), "R", 7.f));
		addChild(createLabel(Vec(77.0f, 15.8f), "L", 7.f));
		addChild(createLabel(Vec(86.0f, 15.8f), "R", 7.f));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(mid - 6.f, 111.0)), module, LinkAudioReceive16::POLY_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(mid + 6.f, 111.0)), module, LinkAudioReceive16::POLY_R_OUTPUT));
		addChild(createLabel(Vec(mid, 105.0), "ALL (POLY)", 7.f));
		addChild(createLabel(Vec(mid - 6.f, 117.5), "L", 7.f));
		addChild(createLabel(Vec(mid + 6.f, 117.5), "R", 7.f));
	}
};


Model* modelLinkAudioReceive16 = createModel<LinkAudioReceive16, LinkAudioReceive16Widget>("LinkAudioReceive16");
