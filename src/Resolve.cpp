#include <algorithm>
#include "plugin.hpp"

struct Resolve : Module {
	static const int NUM_TRIGIN = 6;
	int selected = 0;

	enum ParamIds {
		NUM_PARAMS
	};
	enum InputIds {
		INPUT_TRIG0, INPUT_IN0A, INPUT_IN0B,
		INPUT_TRIG1, INPUT_IN1A, INPUT_IN1B,
		INPUT_TRIG2, INPUT_IN2A, INPUT_IN2B,
		INPUT_TRIG3, INPUT_IN3A, INPUT_IN3B,
		INPUT_TRIG4, INPUT_IN4A, INPUT_IN4B,
		INPUT_TRIG5, INPUT_IN5A, INPUT_IN5B,
		NUM_INPUTS
	};
	enum OutputIds {
		OUTPUT_TRIG,
		OUTPUT_OUTA,
		OUTPUT_OUTB,
		NUM_OUTPUTS
	};
	enum LightIds {
		LIGHT_LED0,
		LIGHT_LED1,
		LIGHT_LED2,
		LIGHT_LED3,
		LIGHT_LED4,
		LIGHT_LED5,
		NUM_LIGHTS
	};

	Resolve() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
	}

	float getInputPolyMaxVoltage(InputIds id) {
		const int input_channels = inputs[id].getChannels();
		const float* input_voltages = inputs[id].getVoltages();
		const float input_max_voltage = *std::max_element(input_voltages, input_voltages+input_channels);
		return input_max_voltage;
	}

	void process(const ProcessArgs& args) override {

		float max_trig_voltage = 0;
		float* output_voltages[3] = {nullptr, nullptr, nullptr};
		int selected_channels[3] = {0, 0, 0};

		for(int tid=0; tid<NUM_TRIGIN; tid++) {
			const float trig_max_poly_voltage = this->getInputPolyMaxVoltage((InputIds)(tid*3));
			if ( trig_max_poly_voltage && (trig_max_poly_voltage >= max_trig_voltage)) {
				max_trig_voltage = trig_max_poly_voltage;
				selected = tid;
			} 
		}
		
		for (int i = 0; i < 3; i++) {
			selected_channels[i] = inputs[selected*3+i].getChannels();
		};
		
		outputs[OUTPUT_TRIG].setChannels(selected_channels[0]);
		outputs[OUTPUT_OUTA].setChannels(selected_channels[1]);
		outputs[OUTPUT_OUTB].setChannels(selected_channels[2]);
		
		for (int i = 0; i < 3; i++) {
			output_voltages[i] = inputs[selected*3+i].getVoltages();
		}

		outputs[OUTPUT_TRIG].writeVoltages(output_voltages[0]);
		outputs[OUTPUT_OUTA].writeVoltages(output_voltages[1]);
		outputs[OUTPUT_OUTB].writeVoltages(output_voltages[2]);


		for (int tid=0; tid < NUM_TRIGIN; tid++) {
			lights[tid].setBrightness((float)(tid == selected));
		}
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

		for (int i = 0; i < Resolve::NUM_TRIGIN; i++) {
			const int y = 40.00 + 11.0 * i;
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.25, y)), module, i*3));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.25, y)), module, i*3 + 1));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.0, y)), module, i*3 +2));
			addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(11.25, y+3.0)), module, i));
		}
		
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(6.25, 107.25)), module, Resolve::OUTPUT_TRIG));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(16.25, 107.25)), module, Resolve::OUTPUT_OUTA));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.0, 107.25)), module, Resolve::OUTPUT_OUTB));
	}
};


Model* modelResolve = createModel<Resolve, ResolveWidget>("Resolve");

