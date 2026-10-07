#include "plugin.hpp"
#include "LinkTime.hpp"

#include <atomic>
#include <cmath>
#include <cstring>


// Link Audio Send — publishes a Rack stereo input as a Link Audio channel that
// other Link Audio peers (WAIL, Live, another Most Distant Link Audio Receive,
// ...) can hear. The WAIL app captures it like any LAN channel and streams it
// to the room.
//
// Audio path (engine thread): inputs (±5 V → ±1) collect in kInBlock-frame
// blocks, are converted from the engine rate to Link Audio's 48 kHz, and are
// committed to the sink kCommitFrames at a time. Each commit is stamped with
// the session beat at which its first frame is heard (playMicros — WAIL Send's
// stamp-ahead, in Rack terms), tracked as a fractional engine-frame position
// so the stamps advance exactly with the audio. Nothing allocates here: the
// session snapshot is preallocated and the sink's retain/commit is realtime-safe.
//
// Like WAIL Send, Link Audio only flows while some peer subscribes to the
// channel; the light shows whether commits are being accepted.
struct LinkAudioSend : Module {
	enum ParamId {
		GAIN_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		IN_L_INPUT,
		IN_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTPUTS_LEN
	};
	enum LightId {
		ACTIVE_LIGHT,
		LIGHTS_LEN
	};

	static constexpr int kInBlock = 32;
	static constexpr int kCommitFrames = 128; // ~2.7 ms @48k
	// A 32-frame block at the lowest Rack rate (11.025k) is ~140 frames at
	// 48k; leave room for that on top of a not-yet-committed remainder.
	static constexpr int kOutCap = 1024;

	lb_link* link = nullptr;
	lb_sink* sink = nullptr;
	lb_state* state = nullptr;
	FrameClock clock;
	dsp::SampleRateConverter<2> src;
	float srcRate = 0.f;

	dsp::Frame<2> inBuf[kInBlock];
	int inLen = 0;
	int64_t nextInFrame = -1; // engine frame expected next (discontinuity check)
	float outInter[kOutCap * 2];
	float outL[kOutCap], outR[kOutCap];
	int outLen = 0;
	double outPos = 0.0; // engine frame (fractional) heard as outL[0]

	std::atomic<bool> accepted{false};
	dsp::ClockDivider lightDivider;

	std::string channelName = "VCV Rack"; // UI thread only

	LinkAudioSend() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Gain", "x");
		configInput(IN_L_INPUT, "Left (polyphonic inputs are summed)");
		configInput(IN_R_INPUT, "Right (normalled to left)");
		configLight(ACTIVE_LIGHT, "Publishing to a subscriber");
		lightDivider.setDivision(512);
		src.setChannels(2);

		link = lb_create(120.0);
		lb_enable(link, true);
		lb_enable_audio(link, true);
		state = lb_state_create();
		// Max samples per buffer: one commit, stereo, with headroom.
		sink = lb_sink_create(link, channelName.c_str(), kCommitFrames * 2 * 4);
	}

	~LinkAudioSend() override {
		lb_sink_destroy(sink);
		if (link) {
			lb_enable(link, false);
			lb_destroy(link);
		}
		lb_release(state);
	}

	// [UI thread] — lb_sink_set_name is a main-thread call.
	void setChannelName(const std::string& name) {
		if (name.empty() || name == channelName)
			return;
		channelName = name;
		lb_sink_set_name(sink, channelName.c_str());
	}

	void restart(const ProcessArgs& args) {
		inLen = 0;
		outLen = 0;
		if (srcRate != args.sampleRate) {
			src.setRates((int) args.sampleRate, kLinkAudioRate);
			srcRate = args.sampleRate;
		}
		// The converter's group delay: the first 48k frames out are its warm-up,
		// and engine frame f emerges converterLatency frames later.
		outPos = (double) (args.frame - converterLatency(src));
	}

	void process(const ProcessArgs& args) override {
		observeEngine(clock, link, args);
		// A rate change or a gap in the frame sequence (module bypassed, engine
		// paused) breaks the stamp ↔ audio correspondence: start over.
		if (args.frame != nextInFrame || srcRate != args.sampleRate)
			restart(args);
		nextInFrame = args.frame + 1;

		const float gain = params[GAIN_PARAM].getValue();
		const float l = inputs[IN_L_INPUT].getVoltageSum() / 5.f * gain;
		const float r = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltageSum() / 5.f * gain : l;
		inBuf[inLen].samples[0] = l;
		inBuf[inLen].samples[1] = r;
		if (++inLen == kInBlock)
			flush();

		if (lightDivider.process())
			lights[ACTIVE_LIGHT].setBrightness(accepted ? 1.f : 0.f);
	}

	void flush() {
		int inFrames = inLen;
		int outFrames = kOutCap - outLen;
		src.process((const float*) inBuf, 2, &inFrames, outInter, 2, &outFrames);
		for (int i = 0; i < outFrames; i++) {
			outL[outLen + i] = outInter[2 * i];
			outR[outLen + i] = outInter[2 * i + 1];
		}
		outLen += outFrames;
		// The converter consumes the whole block unless the output buffer is
		// full; keep any remainder for the next block.
		if (inFrames < inLen)
			std::memmove(inBuf, inBuf + inFrames, (inLen - inFrames) * sizeof(inBuf[0]));
		inLen -= inFrames;

		while (outLen >= kCommitFrames)
			commit();
	}

	void commit() {
		if (clock.ready()) {
			lb_capture_into(link, state);
			const double beat = lb_beat_at_time(state, playMicros(clock, (int64_t) outPos), kStampQuantum);
			accepted = lb_sink_commit(sink, state, beat, kStampQuantum, outL, outR, kCommitFrames, kLinkAudioRate);
		}
		outLen -= kCommitFrames;
		std::memmove(outL, outL + kCommitFrames, outLen * sizeof(float));
		std::memmove(outR, outR + kCommitFrames, outLen * sizeof(float));
		outPos += (double) kCommitFrames * srcRate / kLinkAudioRate;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "channelName", json_string(channelName.c_str()));
		return root;
	}

	// [UI thread] — patch load and undo run here, so renaming the sink is safe.
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "channelName"))
			setChannelName(json_string_value(j));
	}
};


// ChannelNameField renames the sink when Enter is pressed in the context menu.
struct ChannelNameField : ui::TextField {
	LinkAudioSend* module;

	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
			module->setChannelName(text);
			if (ui::MenuOverlay* overlay = getAncestorOfType<ui::MenuOverlay>())
				overlay->requestDelete();
			e.consume(this);
		}
		if (!e.getTarget())
			ui::TextField::onSelectKey(e);
	}
};


struct LinkAudioSendWidget : ModuleWidget {
	LinkAudioSendWidget(LinkAudioSend* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LinkAudioSend.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addChild(createLabel(Vec(20.32, 9.0), "LINK AUDIO", 10.f));
		addChild(createLabel(Vec(20.32, 13.5), "SEND", 8.f));
		addChild(createChannelDisplay(Vec(20.32, 22.0), Vec(32.0, 7.0), [module]() {
			return module ? module->channelName : std::string("VCV Rack");
		}));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(20.32, 30.0)), module, LinkAudioSend::ACTIVE_LIGHT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(20.32, 50.0)), module, LinkAudioSend::GAIN_PARAM));
		addChild(createLabel(Vec(20.32, 57.5), "GAIN", 7.f));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.0, 108.0)), module, LinkAudioSend::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(27.64, 108.0)), module, LinkAudioSend::IN_R_INPUT));
		addChild(createLabel(Vec(13.0, 114.5), "L", 7.f));
		addChild(createLabel(Vec(27.64, 114.5), "R", 7.f));
	}

	void appendContextMenu(Menu* menu) override {
		LinkAudioSend* module = getModule<LinkAudioSend>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Channel name (Enter to apply)"));
		ChannelNameField* field = new ChannelNameField;
		field->module = module;
		field->box.size.x = 200;
		field->text = module->channelName;
		menu->addChild(field);
	}
};


Model* modelLinkAudioSend = createModel<LinkAudioSend, LinkAudioSendWidget>("LinkAudioSend");
