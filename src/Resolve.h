#include "plugin.hpp"


struct Resolve : Module {
	enum ParamIds {
		NUM_PARAMS
	};
	enum InputIds {
		TRIG1_INPUT,
		IN1A_INPUT,
		IN1B_INPUT,
		TRIG2_INPUT,
		IN2A_INPUT,
		IN2B_INPUT,
		TRIG3_INPUT,
		IN3A_INPUT,
		IN3B_INPUT,
		TRIG4_INPUT,
		IN4A_INPUT,
		IN4B_INPUT,
		TRIG5_INPUT,
		IN5A_INPUT,
		IN5B_INPUT,
		TRIG6_INPUT,
		IN6A_INPUT,
		IN6B_INPUT,
		TRIG7_INPUT,
		IN7A_INPUT,
		IN7B_INPUT,
		TRIG8_INPUT,
		IN8A_INPUT,
		IN8B_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		TRIG_OUTPUT,
		OUTA_OUTPUT,
		OUTB_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		LED1_LIGHT,
		LED2_LIGHT,
		LED3_LIGHT,
		LED4_LIGHT,
		LED5_LIGHT,
		LED6_LIGHT,
		LED7_LIGHT,
		LED8_LIGHT,
		NUM_LIGHTS
	};

	Resolve() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
	}

	void process(const ProcessArgs& args) override {
	}
};


struct ResolveWidget : ModuleWidget {
	ResolveWidget(Resolve* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Resolve.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 47.25)), module, Resolve::TRIG1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 47.25)), module, Resolve::IN1A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 47.25)), module, Resolve::IN1B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 54.75)), module, Resolve::TRIG2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 54.75)), module, Resolve::IN2A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 54.75)), module, Resolve::IN2B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 62.25)), module, Resolve::TRIG3_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 62.25)), module, Resolve::IN3A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 62.25)), module, Resolve::IN3B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 69.75)), module, Resolve::TRIG4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 69.75)), module, Resolve::IN4A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 69.75)), module, Resolve::IN4B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 77.25)), module, Resolve::TRIG5_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 77.25)), module, Resolve::IN5A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 77.25)), module, Resolve::IN5B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 84.75)), module, Resolve::TRIG6_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 84.75)), module, Resolve::IN6A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 84.75)), module, Resolve::IN6B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 92.25)), module, Resolve::TRIG7_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 92.25)), module, Resolve::IN7A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 92.25)), module, Resolve::IN7B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, 99.75)), module, Resolve::TRIG8_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, 99.75)), module, Resolve::IN8A_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, 99.75)), module, Resolve::IN8B_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(6.25, 107.25)), module, Resolve::TRIG_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(16.25, 107.25)), module, Resolve::OUTA_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.0, 107.25)), module, Resolve::OUTB_OUTPUT));

		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 48.5)), module, Resolve::LED1_LIGHT));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 56.0)), module, Resolve::LED2_LIGHT));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 63.5)), module, Resolve::LED3_LIGHT));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 71.0)), module, Resolve::LED4_LIGHT));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 78.5)), module, Resolve::LED5_LIGHT));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 86.0)), module, Resolve::LED6_LIGHT));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 93.5)), module, Resolve::LED7_LIGHT));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, 101.0)), module, Resolve::LED8_LIGHT));
	}
};


Model* modelResolve = createModel<Resolve, ResolveWidget>("Resolve");
