#include "plugin.hpp"
#include "LinkTime.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>


// Link Clock — joins the local Ableton Link session as a peer and turns its
// shared timeline into CV: a beat clock, a bar reset, and a run gate driven by
// Link start/stop transport.
//
// Edges are sample-accurate: every frame is mapped to the Link time at which
// it will be heard (FrameClock + output latency, see LinkTime.hpp) and the beat
// is evaluated there, so a clock edge lands on the frame where the beat
// boundary falls. The session state is captured into a preallocated snapshot
// once per engine block — nothing allocates on the audio thread.
//
// The clock runs whenever Link's timeline does (always). "Only clock while Link
// is playing" gates it with Link start/stop instead, for setups where a peer
// (e.g. Live with Start Stop Sync) drives transport — off by default, because
// a session without a start/stop-aware peer never reports playing.
struct LinkClock : Module {
	enum ParamId {
		PPQN_PARAM,
		QUANTUM_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INPUTS_LEN
	};
	enum OutputId {
		CLOCK_OUTPUT,
		RESET_OUTPUT,
		RUN_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		PLAYING_LIGHT,
		PEER_LIGHT,
		LIGHTS_LEN
	};

	lb_link* link = nullptr;
	lb_state* state = nullptr; // preallocated; captured into per block
	FrameClock clock;
	dsp::PulseGenerator clockPulse;
	dsp::PulseGenerator resetPulse;
	dsp::ClockDivider lightDivider;
	double lastBeat = NAN;
	bool playing = false;
	bool gateWithTransport = false;
	std::atomic<double> tempo{120.0}; // engine writes, UI reads
	std::atomic<int> peers{0};

	LinkClock() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PPQN_PARAM, 1.f, 24.f, 4.f, "Pulses per beat")->snapEnabled = true;
		configParam(QUANTUM_PARAM, 1.f, 16.f, 4.f, "Quantum (beats per bar)")->snapEnabled = true;
		configOutput(CLOCK_OUTPUT, "Clock");
		configOutput(RESET_OUTPUT, "Bar reset");
		configOutput(RUN_OUTPUT, "Run (Link transport)");
		configLight(PLAYING_LIGHT, "Link transport playing");
		configLight(PEER_LIGHT, "Link peers connected");
		lightDivider.setDivision(512);

		// One abl_link handle = one LAN peer. Created here, on the UI thread
		// (it spawns Link's threads), per the lb_ API threading contract.
		link = lb_create(120.0);
		lb_enable(link, true);
		lb_enable_start_stop_sync(link, true);
		state = lb_state_create();
	}

	~LinkClock() override {
		if (link) {
			lb_enable(link, false);
			lb_destroy(link);
		}
		lb_release(state);
	}

	void onReset() override {
		gateWithTransport = false;
	}

	void process(const ProcessArgs& args) override {
		observeEngine(clock, link, args);
		if (args.frame == APP->engine->getBlockFrame() || std::isnan(lastBeat)) {
			lb_capture_into(link, state);
			playing = lb_state_is_playing(state);
		}

		const double quantum = std::max(1.0, std::round((double) params[QUANTUM_PARAM].getValue()));
		const double ppqn = std::max(1.0, std::round((double) params[PPQN_PARAM].getValue()));
		const double beat = lb_beat_at_time(state, playMicros(clock, args.frame), quantum);
		const bool running = playing || !gateWithTransport;

		if (running && !std::isnan(lastBeat) && beat > lastBeat) {
			if (std::floor(beat * ppqn) > std::floor(lastBeat * ppqn))
				clockPulse.trigger(1e-3f);
			if (std::floor(beat / quantum) > std::floor(lastBeat / quantum))
				resetPulse.trigger(1e-3f);
		}
		lastBeat = beat;

		outputs[CLOCK_OUTPUT].setVoltage(clockPulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[RESET_OUTPUT].setVoltage(resetPulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[RUN_OUTPUT].setVoltage(running ? 10.f : 0.f);

		if (lightDivider.process()) {
			peers = (int) lb_num_peers(link); // an atomic read in Link
			tempo = lb_state_tempo(state);
			lights[PLAYING_LIGHT].setBrightness(playing ? 1.f : 0.f);
			lights[PEER_LIGHT].setBrightness(peers > 0 ? 1.f : 0.f);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "gateWithTransport", json_boolean(gateWithTransport));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "gateWithTransport"))
			gateWithTransport = json_boolean_value(j);
	}
};


struct LinkClockWidget : ModuleWidget {
	LinkClockWidget(LinkClock* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LinkClock.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addChild(createLabel(Vec(20.32, 9.0), "LINK CLOCK", 10.f));
		addChild(createChannelDisplay(Vec(20.32, 22.0), Vec(32.0, 7.0), [module]() {
			if (!module)
				return std::string("120.0 BPM");
			return string::f("%.1f BPM · %d", module->tempo.load(), module->peers.load());
		}));

		addChild(createLabel(Vec(13.0, 31.0), "PLAY", 7.f));
		addChild(createLabel(Vec(27.64, 31.0), "PEERS", 7.f));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(13.0, 34.5)), module, LinkClock::PLAYING_LIGHT));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(27.64, 34.5)), module, LinkClock::PEER_LIGHT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(20.32, 46.0)), module, LinkClock::PPQN_PARAM));
		addChild(createLabel(Vec(20.32, 53.5), "PULSES / BEAT", 7.f));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(20.32, 64.0)), module, LinkClock::QUANTUM_PARAM));
		addChild(createLabel(Vec(20.32, 71.5), "BEATS / BAR", 7.f));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32, 82.0)), module, LinkClock::CLOCK_OUTPUT));
		addChild(createLabel(Vec(20.32, 88.0), "CLOCK", 7.f));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32, 97.0)), module, LinkClock::RESET_OUTPUT));
		addChild(createLabel(Vec(20.32, 103.0), "BAR", 7.f));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32, 112.0)), module, LinkClock::RUN_OUTPUT));
		addChild(createLabel(Vec(20.32, 118.0), "RUN", 7.f));
	}

	void appendContextMenu(Menu* menu) override {
		LinkClock* module = getModule<LinkClock>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Only clock while Link is playing", "", &module->gateWithTransport));
	}
};


Model* modelLinkClock = createModel<LinkClock, LinkClockWidget>("LinkClock");
