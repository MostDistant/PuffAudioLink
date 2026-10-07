#include "plugin.hpp"
#include "RecvSlot.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>


// Link Audio Receive — subscribes to one Link Audio channel and plays it into
// Rack (stereo out, ±5 V).
//
// Channel choice lives in the context menu: a specific channel by name, or
// Auto (the default), which takes the first WAIL room channel ("WAIL · …") —
// so with WAIL running, dropping the module in a patch is enough to hear the
// room. The selection is by name (ids do not survive a publisher restart) and
// follows a channel the publisher renames in place, as the WAIL app does once
// a sender's stream name arrives.
//
// Threading: a manager thread does discovery and subscribe/unsubscribe (none of
// it realtime-safe); the engine thread renders the slot (RecvSlot.hpp).
struct LinkAudioReceive : Module {
	enum ParamId {
		PARAMS_LEN
	};
	enum InputId {
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SUBSCRIBED_LIGHT,
		AUDIO_LIGHT,
		LIGHTS_LEN
	};

	static constexpr const char* kRoomPrefix = "WAIL · ";
	static constexpr int kGonePolls = 6; // ~3 s of discovery misses → unsubscribe

	lb_link* link = nullptr;

	// --- engine thread ---
	lb_state* state = nullptr;
	FrameClock clock;
	RecvSlot slot;
	int outPos = RecvSlot::kOutBlock;
	dsp::ClockDivider lightDivider;
	std::atomic<uint64_t> processEpoch{0};

	// --- manager thread ---
	std::thread manager;
	std::atomic<bool> running{false};
	uint64_t currentId = 0; // manager-only
	int misses = 0;         // manager-only

	// Shared between the manager and the UI thread.
	std::mutex mu;
	std::string wanted;  // "" = Auto
	std::string current; // name of the subscribed channel, "" = none
	std::vector<std::string> discovered;

	LinkAudioReceive() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");
		configLight(SUBSCRIBED_LIGHT, "Subscribed");
		configLight(AUDIO_LIGHT, "Receiving audio");
		lightDivider.setDivision(512);

		link = lb_create(120.0);
		lb_enable(link, true);
		lb_enable_audio(link, true);
		state = lb_state_create();

		running = true;
		manager = std::thread([this]() { manage(); });
	}

	~LinkAudioReceive() override {
		running = false;
		if (manager.joinable())
			manager.join();
		// The engine has already removed this module, so nothing renders now.
		lb_source_destroy(slot.active.exchange(nullptr));
		if (link) {
			lb_enable(link, false);
			lb_destroy(link);
		}
		lb_release(state);
	}

	// --- manager thread ---

	static bool isRoomChannel(const char* name) {
		return std::strncmp(name, kRoomPrefix, std::strlen(kRoomPrefix)) == 0;
	}

	void retire(lb_source* next) {
		retireSource(slot, processEpoch, next);
	}

	void manage() {
		while (running) {
			lb_channel_info chans[LB_MAX_CHANNELS];
			size_t n = lb_channels(link, chans, LB_MAX_CHANNELS);

			std::string want;
			{
				std::lock_guard<std::mutex> lock(mu);
				want = wanted;
				discovered.clear();
				for (size_t i = 0; i < n; i++)
					discovered.push_back(chans[i].name);
			}

			// Still subscribed to a live channel? Follow an in-place rename
			// (the id survives it), and keep it unless the selection moved.
			int at = -1;
			for (size_t i = 0; i < n; i++)
				if (currentId && chans[i].id_u64 == currentId)
					at = (int) i;
			if (currentId) {
				std::string cur;
				{
					std::lock_guard<std::mutex> lock(mu);
					if (at >= 0 && current != chans[at].name) {
						if (wanted == current)
							wanted = want = chans[at].name;
						current = chans[at].name;
					}
					cur = current;
				}
				misses = at >= 0 ? 0 : misses + 1;
				bool stillWanted = want.empty() ? isRoomChannel(cur.c_str()) : cur == want;
				if (misses >= kGonePolls || !stillWanted)
					unsubscribe();
			}

			if (!currentId) {
				for (size_t i = 0; i < n; i++) {
					bool match = want.empty() ? isRoomChannel(chans[i].name) : want == chans[i].name;
					if (!match)
						continue;
					lb_source* s = lb_source_create(link, chans[i].id_u64);
					if (!s)
						break;
					currentId = chans[i].id_u64;
					misses = 0;
					{
						std::lock_guard<std::mutex> lock(mu);
						current = chans[i].name;
					}
					retire(s);
					break;
				}
			}

			for (int i = 0; i < 10 && running; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	void unsubscribe() {
		currentId = 0;
		misses = 0;
		{
			std::lock_guard<std::mutex> lock(mu);
			current.clear();
		}
		retire(nullptr);
	}

	// --- UI thread ---

	void select(const std::string& name) {
		std::lock_guard<std::mutex> lock(mu);
		wanted = name;
	}

	std::string displayName() {
		std::lock_guard<std::mutex> lock(mu);
		if (current.empty())
			return wanted.empty() ? "Auto: waiting" : wanted + " (gone)";
		if (isRoomChannel(current.c_str()))
			return current.substr(std::strlen(kRoomPrefix));
		return current;
	}

	// --- engine thread ---

	void process(const ProcessArgs& args) override {
		observeEngine(clock, link, args);

		if (outPos >= RecvSlot::kOutBlock) {
			lb_capture_into(link, state);
			slot.render(args, state, playMicros(clock, args.frame));
			outPos = 0;
			processEpoch.fetch_add(1, std::memory_order_acq_rel);
		}
		outputs[OUT_L_OUTPUT].setVoltage(slot.left(outPos));
		outputs[OUT_R_OUTPUT].setVoltage(slot.right(outPos));
		outPos++;

		if (lightDivider.process()) {
			lights[SUBSCRIBED_LIGHT].setBrightness(slot.subscribed() ? 1.f : 0.f);
			lights[AUDIO_LIGHT].setBrightness(slot.hearing(args.frame, args.sampleRate) ? 1.f : 0.f);
		}
	}

	// --- persistence ---

	json_t* dataToJson() override {
		json_t* root = json_object();
		std::lock_guard<std::mutex> lock(mu);
		json_object_set_new(root, "channel", json_string(wanted.c_str()));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "channel"))
			select(json_string_value(j));
	}
};


struct LinkAudioReceiveWidget : ModuleWidget {
	LinkAudioReceiveWidget(LinkAudioReceive* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LinkAudioReceive.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addChild(createLabel(Vec(20.32, 9.0), "LINK AUDIO", 10.f));
		addChild(createLabel(Vec(20.32, 13.5), "RECEIVE", 8.f));
		addChild(createChannelDisplay(Vec(20.32, 22.0), Vec(32.0, 7.0), [module]() {
			return module ? module->displayName() : std::string("Auto");
		}));
		addChild(createLabel(Vec(13.0, 31.0), "SUB", 7.f));
		addChild(createLabel(Vec(27.64, 31.0), "AUDIO", 7.f));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(13.0, 34.5)), module, LinkAudioReceive::SUBSCRIBED_LIGHT));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(27.64, 34.5)), module, LinkAudioReceive::AUDIO_LIGHT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(13.0, 108.0)), module, LinkAudioReceive::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.64, 108.0)), module, LinkAudioReceive::OUT_R_OUTPUT));
		addChild(createLabel(Vec(13.0, 114.5), "L", 7.f));
		addChild(createLabel(Vec(27.64, 114.5), "R", 7.f));
	}

	void appendContextMenu(Menu* menu) override {
		LinkAudioReceive* module = getModule<LinkAudioReceive>();
		std::vector<std::string> names;
		std::string wanted;
		{
			std::lock_guard<std::mutex> lock(module->mu);
			names = module->discovered;
			wanted = module->wanted;
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Link Audio channel"));
		menu->addChild(createCheckMenuItem("Auto (first WAIL room channel)", "",
		                                   [=]() { return wanted.empty(); },
		                                   [=]() { module->select(""); }));
		if (names.empty())
			menu->addChild(createMenuLabel("No channels on the network"));
		for (const std::string& name : names)
			menu->addChild(createCheckMenuItem(name, "",
			                                   [=]() { return wanted == name; },
			                                   [=]() { module->select(name); }));
	}
};


Model* modelLinkAudioReceive = createModel<LinkAudioReceive, LinkAudioReceiveWidget>("LinkAudioReceive");
