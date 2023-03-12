#include <dirent.h>
#include <libgen.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../dep/babycat/babycat.h"
#include "./shared/Components.hpp"
#include "plugin.hpp"

#define AUDIO_SAMPLE_DISPLAY_RES 1024

using namespace rage;

struct AudioSample {
    bool is_loaded = false;
    bool is_modified = false;

    std::string file_path;
    std::string file_display;
    std::string file_info_display;

    int num_frames = 0;
    int num_channels = 0;
    int num_samples = 0;
    int frame_rate_hz = 0;
    babycat_Waveform* raw_waveform = nullptr;

    int read_head = 0;
    int write_head = 0;
    int start_head = 0;
    int stop_head = 0;

    std::vector<double> display_buf;

    void load_waveform(babycat_Waveform* waveform) {
        this->raw_waveform = waveform;
        this->num_samples = babycat_waveform_get_num_samples(waveform);
        this->num_channels = babycat_waveform_get_num_channels(waveform);
        this->num_frames = babycat_waveform_get_num_frames(waveform);
        this->frame_rate_hz = babycat_waveform_get_frame_rate_hz(waveform);
    }

    void update_display_data() {
        char* pathDup = strdup(this->file_path.c_str());
        std::string fileDescription = basename(pathDup);
        this->file_display = fileDescription.substr(0, fileDescription.size() - 4);
        this->file_display = file_display.substr(0, 20);
        this->file_info_display = std::to_string(this->frame_rate_hz) + "-" + std::to_string(this->num_channels) + "Ch";
        free(pathDup);
    }

    double get_sample(int frame_idx, int channel_idx) {
        if (frame_idx < 0 || frame_idx >= this->num_frames)
            return 0;
        if (channel_idx < 0 || channel_idx >= this->num_channels)
            return 0;

        return babycat_waveform_get_unchecked_sample(this->raw_waveform, frame_idx, channel_idx);
    }

    void build_display_buf() {
        this->display_buf.resize(AUDIO_SAMPLE_DISPLAY_RES, 0.0);
        const int skip_amount = this->num_frames / AUDIO_SAMPLE_DISPLAY_RES;
        for (int i = 0; i < AUDIO_SAMPLE_DISPLAY_RES && i < this->num_frames; i++) {
            this->display_buf[i] = this->get_sample(i * skip_amount, 0);
        }
    }

    bool load_file(std::string& path) {
        // Try load waveform using babycat

        babycat_WaveformArgs waveform_args = babycat_waveform_args_init_default();
        babycat_WaveformResult waveform_result = babycat_waveform_from_file(path.c_str(), waveform_args);
        if (waveform_result.error_num != 0) {
            printf("Failed to load audio clip [%s] with error: %u\n", path.c_str(), waveform_result.error_num);
            return false;
        }

        this->file_path = path;
        this->is_loaded = true;
        this->is_modified = false;

        {
            auto waveform = waveform_result.result;
            this->load_waveform(waveform);

            // make sure we are working with 1 sample per channel in each frame
            const int samples_perChannel_perFrame = this->num_samples / (this->num_channels * this->num_frames);
            babycat_waveform_resample(waveform, frame_rate_hz * samples_perChannel_perFrame);

            this->load_waveform(waveform);
        }

        this->update_display_data();
        this->build_display_buf();

        return true;
    }

    bool is_empty() {
        return !this->is_loaded && !is_modified;
    }
};

struct AudioSlice {
    int sample_index;
    int start;
    int stop;
};

struct Reflux: Module {
    enum ParamIds {
        SELECTED_SAMPLE,
        SELECTED_SLICE,
        SAMPLE_HEAD_VAL,
        SAMPLE_HEAD_MODE,
        SAMPLE_START,
        SAMPLE_STOP,
        SAMPLE_WRIE,
        SAMPLE_RECORDING,
        SAMPLE_LOAD,
        SAMPLE_TRIG,
        SAMPLE_AUTO_SLICE,
        SAMPLE_MAKE_SLICE,
        GLOBAL_DRY,
        GLOBAL_TRIG_MODE,
        GLOBAL_OVERWRITE,
        GLOBAL_SLICES,
        GLOBAL_SLICE_CV_MODE,
        GLOBAL_SLICE_CV_ATNV,
        NUM_PARAMS
    };

    enum InputIds {
        INPUT_AUDIOL,
        INPUT_AUDIOR,
        INPUT_SAMPLE_CV,
        INPUT_START_RECORD,
        INPUT_STOP_RECORD,
        INPUT_TRIGGER,
        INPUT_SLICE_CV,
        INPUT_SPEED_CV,
        INPUT_INERTIA_CV,
        NUM_INPUTS
    };

    enum OutputIds { OUTPUT_AUDIOL, OUTPUT_AUDIOR, OUTPUT_BOS, OUTPUT_EOS, NUM_OUTPUTS };

    enum LightIds { NUM_LIGHTS };

    static const int NUM_SAMPLES = 3;
    int selected_sample = 0;
    AudioSample samples[NUM_SAMPLES];
    std::string directory_ = "";

    Reflux() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    }

    std::string get_last_directory() {
        return this->directory_;
    };

    bool load_file(std::string filepath) {
        const bool loaded = current_sample().load_file(filepath);
        if (loaded)
            directory_ = system::getDirectory(filepath);
        return loaded;
        return false;
    };

    AudioSample& current_sample() {
        return samples[selected_sample];
    }

    void process(const ProcessArgs& args) override {}
};

struct RefluxSampleDisplay: TransparentWidget {
    Reflux* module;
    int frame = 0;

    RefluxSampleDisplay() {}

    void drawLine(const DrawArgs& args, NVGcolor color, rack::math::Vec start, rack::math::Vec stop) {
        nvgStrokeColor(args.vg, color);
        {
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, start.x, start.y);
            nvgLineTo(args.vg, stop.x, stop.y);
            nvgClosePath(args.vg);
        }
        nvgStroke(args.vg);
    }

    void drawRect(const DrawArgs& args, NVGcolor color, rack::math::Rect rect, bool fill = false) {
        auto pos = rect.pos;
        auto size = rect.size;
        if (fill) {
            nvgBeginPath(args.vg);
            nvgFillColor(args.vg, color);
            nvgRect(args.vg, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y);
            nvgFill(args.vg);
            nvgClosePath(args.vg);
        }
        drawLine(args, color, pos, pos + size * Vec(1, 0));
        drawLine(args, color, pos, pos + size * Vec(0, 1));
        drawLine(args, color, pos + size * Vec(1, 0), pos + size * Vec(1, 1));
        drawLine(args, color, pos + size * Vec(0, 1), pos + size * Vec(1, 1));
    }

    void drawHLine(const DrawArgs& args, NVGcolor color, Rect rect, double pos_ratio) {
        int yLine = floor(pos_ratio * rect.size.y);
        nvgStrokeWidth(args.vg, 0.8);
        this->drawLine(args, color, rect.pos + Vec(0, yLine), rect.pos + Vec(rect.size.x, yLine));
    }

    void drawVLine(const DrawArgs& args, NVGcolor color, Rect rect, double pos_ratio) {
        int xLine = floor(pos_ratio * rect.size.x);
        nvgStrokeWidth(args.vg, 0.8);
        this->drawLine(args, color, rect.pos + Vec(xLine, 0), rect.pos + Vec(xLine, rect.size.y));
    }

    void drawText(const DrawArgs& args, NVGcolor color, Rect rect, const char* text) {
        nvgFontSize(args.vg, 8);
        nvgTextLetterSpacing(args.vg, 0);
        nvgFillColor(args.vg, color);
        nvgTextBox(args.vg, rect.pos.x, rect.pos.y + rect.size.y, rect.size.x, text, NULL);
    }

    void drawSample(const DrawArgs& args, Rect rect, AudioSample& sample) {
        auto& display_buf = sample.display_buf;
        nvgStrokeColor(args.vg, nvgRGBA(0x22, 0x44, 0xc9, 0xc0));
        nvgSave(args.vg);
        nvgScissor(args.vg, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y);
        nvgBeginPath(args.vg);
        for (unsigned int i = 0; i < display_buf.size(); i++) {
            float x, y;
            x = (float)i / (display_buf.size() - 1);
            y = display_buf[i] / 2.0 + 0.5;
            Vec p;
            p.x = rect.pos.x + rect.size.x * x;
            p.y = rect.pos.y + rect.size.y * (1.0 - y);
            if (i == 0)
                nvgMoveTo(args.vg, p.x, p.y);
            else
                nvgLineTo(args.vg, p.x, p.y);
        }
        nvgLineCap(args.vg, NVG_ROUND);
        nvgMiterLimit(args.vg, 1.0);
        nvgStrokeWidth(args.vg, 0.6);
        nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
        nvgStroke(args.vg);
        nvgResetScissor(args.vg);
        nvgRestore(args.vg);
    }

    struct SplitResult {
        Rect A;
        Rect B;
    };

    SplitResult splitRectH(const Rect rect, float h) {
        return
        SplitResult {A: Rect(rect.pos, Vec(rect.size.x, h)), B: Rect(rect.pos + Vec(0, h), rect.size - Vec(0, h))};
    }

    SplitResult splitRectV(const Rect rect, float w) {
        return
        SplitResult {A: Rect(rect.pos, Vec(w, rect.size.y)), B: Rect(rect.pos + Vec(w, 0), rect.size - Vec(w, 0))};
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        const float titleHeight = 10;
        const auto rectBorderColor = nvgRGB(0xff, 0xff, 0x00);

        if (module) {
            AudioSample& sample = module->current_sample();
            if (layer == 1) {
                Rect localBox = Rect(Vec(0), box.size);
                Rect headerRect, bodyRect, fileNameRect, fileInfoRect;
                {
                    auto result = splitRectH(localBox, titleHeight);
                    headerRect = result.A;
                    bodyRect = result.B;
                    result = splitRectV(headerRect, headerRect.size.x * 0.7);
                    fileNameRect = result.A;
                    fileInfoRect = result.B;
                }

                this->drawRect(args, nvgRGB(0, 0, 0), localBox, true);
                this->drawRect(args, rectBorderColor, fileNameRect);
                this->drawRect(args, rectBorderColor, fileInfoRect);
                this->drawRect(args, rectBorderColor, bodyRect);

                this->drawText(args, nvgRGB(0, 255, 0), fileNameRect, sample.file_display.c_str());
                this->drawText(args, nvgRGB(0, 255, 0), fileInfoRect, sample.file_info_display.c_str());

                // Zero Line
                this->drawHLine(args, nvgRGB(255, 0, 0), bodyRect, 0.5);

                if (!sample.is_empty()) {
                    // Write head line
                    drawVLine(args, nvgRGB(255, 0, 0), bodyRect, sample.write_head / sample.num_frames);

                    // Read head line
                    drawVLine(args, nvgRGB(0, 255, 0), bodyRect, sample.read_head / sample.num_frames);

                    // Start head line
                    drawVLine(args, nvgRGB(0, 0, 255), bodyRect, sample.start_head / sample.num_frames);

                    // Stop head line
                    drawVLine(args, nvgRGB(127, 0, 127), bodyRect, sample.stop_head / sample.num_frames);

                    // Waveform
                    drawSample(args, bodyRect, sample);
                }
            }
        }
        Widget::drawLayer(args, layer);
    }
};

struct RefluxWidget: ModuleWidget {
    RefluxWidget(Reflux* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Reflux.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        {
            RefluxSampleDisplay* display = new RefluxSampleDisplay();
            display->box.pos = Vec(15, 210);
            display->box.size = Vec(150, 58);
            display->module = module;
            addChild(display);
        }

        addParam(createParamCentered<LoadButton<Reflux>>(mm2px(Vec(70, 90)), module, Reflux::SAMPLE_LOAD));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0, 101.0)), module, Reflux::INPUT_AUDIOL));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.0, 101.0)), module, Reflux::INPUT_AUDIOR));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.0, 101.0)), module, Reflux::INPUT_START_RECORD));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.0, 101.0)), module, Reflux::INPUT_STOP_RECORD));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0, 112.5)), module, Reflux::INPUT_SLICE_CV));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.0, 112.5)), module, Reflux::INPUT_SPEED_CV));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.0, 112.5)), module, Reflux::INPUT_INERTIA_CV));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.0, 112.5)), module, Reflux::INPUT_TRIGGER));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(103, 10)), module, Reflux::OUTPUT_AUDIOL));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(113, 10)), module, Reflux::OUTPUT_AUDIOR));
    }
};

Model* modelReflux = createModel<Reflux, RefluxWidget>("Reflux");
