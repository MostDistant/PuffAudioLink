#pragma once
#include <rack.hpp>

#include <functional>
#include <string>


using namespace rack;

// Declare the Plugin, defined in plugin.cpp
extern Plugin* pluginInstance;

// Declare each Model, defined in each module source file
extern Model* modelLinkClock;
extern Model* modelLinkAudioReceive;
extern Model* modelLinkAudioReceive16;
extern Model* modelLinkAudioSend;
extern Model* modelWailCore;


// Panel text. Rack's SVG renderer (nanosvg) ignores <text>, so labels are
// drawn here with the UI font instead of living in the panel SVGs.
struct PanelLabel : widget::Widget {
	std::string text;
	float fontSize = 9.f;
	NVGcolor color = nvgRGB(0xc8, 0xc8, 0xd4);

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgFillColor(args.vg, color);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, box.size.x / 2, box.size.y / 2, text.c_str(), NULL);
	}
};

// createLabel places a PanelLabel centered on a point given in mm.
inline PanelLabel* createLabel(Vec centerMm, const std::string& text, float fontSize = 9.f,
                               NVGcolor color = nvgRGB(0xc8, 0xc8, 0xd4)) {
	PanelLabel* l = new PanelLabel;
	l->text = text;
	l->fontSize = fontSize;
	l->color = color;
	l->box.size = mm2px(Vec(40.f, 5.f));
	l->box.pos = mm2px(centerMm).minus(l->box.size.div(2));
	return l;
}

// ChannelDisplay shows a Link Audio channel name in a small screen; the text
// comes from a getter so it can follow renames. Drawn on the UI thread — the
// getter must be safe to call there.
struct ChannelDisplay : widget::Widget {
	std::function<std::string()> getText;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(args.vg, nvgRGB(0x08, 0x08, 0x10));
		nvgFill(args.vg);
		Widget::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			std::string text = getText ? getText() : "Link Audio";
			std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
			if (font) {
				nvgScissor(args.vg, 2, 0, box.size.x - 4, box.size.y);
				nvgFontFaceId(args.vg, font->handle);
				nvgFontSize(args.vg, 10.f);
				nvgFillColor(args.vg, nvgRGB(0x6e, 0xe7, 0xb7));
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgText(args.vg, box.size.x / 2, box.size.y / 2, text.c_str(), NULL);
				nvgResetScissor(args.vg);
			}
		}
		Widget::drawLayer(args, layer);
	}
};

inline ChannelDisplay* createChannelDisplay(Vec centerMm, Vec sizeMm, std::function<std::string()> getText) {
	ChannelDisplay* d = new ChannelDisplay;
	d->getText = getText;
	d->box.size = mm2px(sizeMm);
	d->box.pos = mm2px(centerMm).minus(d->box.size.div(2));
	return d;
}
