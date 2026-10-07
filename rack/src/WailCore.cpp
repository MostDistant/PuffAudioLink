#include "plugin.hpp"
#include "wailcore.h"

#include <atomic>
#include <mutex>


// WAIL — the WAIL client running inside Rack (embedded Go core, wailcore.h).
//
// Packaging spike: the module starts the embedded core and shows it running —
// version and a live goroutine tick count. The session (room join, relay,
// codec) arrives when the engine is extracted from the app.
//
// The core is process-wide (one Go runtime per process), so instances share it
// through a refcount: the first constructed starts it, the last destroyed stops
// it. Constructors, destructors and widget drawing all run on Rack's UI thread,
// which is the only thread that calls into Go — process() never does.
static std::mutex coreMu;
static int coreRefs = 0;

struct WailCore : Module {
	enum ParamId {
		PARAMS_LEN
	};
	enum InputId {
		INPUTS_LEN
	};
	enum OutputId {
		OUTPUTS_LEN
	};
	enum LightId {
		RUNNING_LIGHT,
		LIGHTS_LEN
	};

	std::atomic<bool> running{false};

	WailCore() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configLight(RUNNING_LIGHT, "WAIL core running");
		std::lock_guard<std::mutex> lock(coreMu);
		if (coreRefs++ == 0) {
			WailCoreStart();
			INFO("WAIL: started %s", WailCoreVersion());
		}
		running = true;
	}

	~WailCore() override {
		std::lock_guard<std::mutex> lock(coreMu);
		if (--coreRefs == 0) {
			WailCoreStop();
			INFO("WAIL: core stopped after %llu ticks", (unsigned long long) WailCoreTicks());
		}
	}

	void process(const ProcessArgs& args) override {
		lights[RUNNING_LIGHT].setBrightness(running ? 1.f : 0.f);
	}
};


struct WailCoreWidget : ModuleWidget {
	uint64_t lastTicks = 0;

	WailCoreWidget(WailCore* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WailCore.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addChild(createLabel(Vec(20.32, 9.0), "WAIL", 12.f));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(20.32, 15.0)), module, WailCore::RUNNING_LIGHT));
		addChild(createChannelDisplay(Vec(20.32, 24.0), Vec(34.0, 7.0), [module]() {
			return module ? std::string(WailCoreVersion()) : std::string("wailcore");
		}));
		addChild(createChannelDisplay(Vec(20.32, 33.0), Vec(34.0, 7.0), [module]() {
			return module ? string::f("%llu ticks", (unsigned long long) WailCoreTicks()) : std::string("—");
		}));
	}
};


Model* modelWailCore = createModel<WailCore, WailCoreWidget>("WAIL");
