#include <algorithm>

#include "../dep/babycat/babycat.h"
#include "plugin.hpp"

/*
struct AudioClip {
    data: Vec
}


*/

struct Reflux : Module {
  static const int NUM_TRIGIN = 6;
  int selected = 0;

  enum ParamIds { NUM_PARAMS };
  enum InputIds {
    INPUT_START_RECORD,
    INPUT_STOP_RECORD,
    INPUT_AUDIOL,
    INPUT_AUDIOR,
    INPUT_TRIGGER,
    INPUT_SLICE_CV,
    INPUT_SPEED_CV,
    INPUT_TUNE_CV,
    NUM_INPUTS
  };

  enum OutputIds { OUTPUT_EOS, OUTPUT_AUDIOL, OUTPUT_AUDIOR, NUM_OUTPUTS };

  enum LightIds { NUM_LIGHTS };

  Reflux() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    babycat_WaveformArgs waveform_args = babycat_waveform_args_init_default();
    babycat_WaveformResult waveform_result =
        babycat_waveform_from_file("audio-for-tests/circus-of-freaks/track.flac", waveform_args);
    if (waveform_result.error_num != 0) {
      printf("Decoding error: %u", waveform_result.error_num);
      return;
    }
    struct babycat_Waveform *waveform = waveform_result.result;
    uint32_t num_frames = babycat_waveform_get_num_frames(waveform);
    uint32_t num_channels = babycat_waveform_get_num_channels(waveform);
    uint32_t frame_rate_hz = babycat_waveform_get_frame_rate_hz(waveform);
    printf("Decoded %u frames with %u channels at %u hz\n", num_frames, num_channels, frame_rate_hz);

    return;
  }

  void process(const ProcessArgs &args) override {}
};

struct RefluxWidget : ModuleWidget {
  RefluxWidget(Reflux *module) {
    setModule(module);
    setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Reflux.svg")));

    addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
    addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
    addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
    addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0, 101.0)), module, Reflux::INPUT_START_RECORD));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.0, 101.0)), module, Reflux::INPUT_STOP_RECORD));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.0, 101.0)), module, Reflux::INPUT_AUDIOL));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.0, 101.0)), module, Reflux::INPUT_AUDIOR));

    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0, 112.5)), module, Reflux::INPUT_SLICE_CV));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.0, 112.5)), module, Reflux::INPUT_SPEED_CV));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.0, 112.5)), module, Reflux::INPUT_TUNE_CV));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.0, 112.5)), module, Reflux::INPUT_TRIGGER));

    addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(103, 10)), module, Reflux::OUTPUT_AUDIOL));
    addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(113, 10)), module, Reflux::OUTPUT_AUDIOR));
  }
};

Model *modelReflux = createModel<Reflux, RefluxWidget>("Reflux");
