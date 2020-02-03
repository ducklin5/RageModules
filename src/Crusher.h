#include "plugin.hpp"


struct Crusher : Module {
	enum ParamIds {
		GAIN_PARAM,
		DRIVE_PARAM,
		MIX_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		GAINCV_INPUT,
		DRIVECV_INPUT,
		MIXCV_INPUT,
		IN1_INPUT,
		IN2_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT2_OUTPUT,
		OUT1_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	Crusher() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(GAIN_PARAM, 0.f, 1.f, 0.f, "");
		configParam(DRIVE_PARAM, 0.f, 1.f, 0.f, "");
		configParam(MIX_PARAM, 0.f, 1.f, 0.f, "");
	}

	void process(const ProcessArgs& args) override;
};


struct CrusherWidget : ModuleWidget {
	CrusherWidget(Crusher* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Crusher.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22.5, 59.75)), module, Crusher::GAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(7.5, 76.0)), module, Crusher::DRIVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22.5, 76.0)), module, Crusher::MIX_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.5, 59.75)), module, Crusher::GAINCV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.0, 91.0)), module, Crusher::DRIVECV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.0, 91.0)), module, Crusher::MIXCV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.0, 103.5)), module, Crusher::IN1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.0, 103.5)), module, Crusher::IN2_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.0, 91.0)), module, Crusher::OUT2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.0, 103.5)), module, Crusher::OUT1_OUTPUT));
	}
};


