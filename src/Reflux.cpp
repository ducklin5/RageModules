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
        char* pathDup = strdup(this->file_display.c_str());
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

    std::string get_last_directory()  {
        return this->directory_;
    };

    bool load_file(std::string filepath)  {
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

typedef unsigned char uchar;
struct RGBA {
    const uchar R;
    const uchar G;
    const uchar B;
    const uchar A;

    RGBA(uchar r, uchar g, uchar b, uchar a = 0xff) : R(r), G(g), B(b), A(a) {};
};

struct RefluxSampleDisplay: TransparentWidget {
    Reflux* module;
    int frame = 0;

    RefluxSampleDisplay() {}

    void drawZeroLine(const DrawArgs& args, RGBA color) {
        nvgStrokeColor(args.vg, nvgRGBA(color.R, color.G, color.B, color.A));
        {
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, 10, 210);
            nvgLineTo(args.vg, 130, 210);
            nvgClosePath(args.vg);
        }
        nvgStroke(args.vg);
    }

    void drawMarkerLine(const DrawArgs& args, RGBA color, double pos_ratio) {
        nvgStrokeColor(args.vg, nvgRGBA(color.R, color.G, color.B, color.A));
        int xLine;
        nvgStrokeWidth(args.vg, 0.8);
        {
            nvgBeginPath(args.vg);
            xLine = 7 + floor(pos_ratio * 235);
            nvgMoveTo(args.vg, xLine, 21);
            nvgLineTo(args.vg, xLine, 96);
            nvgClosePath(args.vg);
        }
        nvgStroke(args.vg);
    }

    void drawSample(const DrawArgs& args, AudioSample& sample) {
        auto& display_buf = sample.display_buf;
        nvgStrokeColor(args.vg, nvgRGBA(0x22, 0x44, 0xc9, 0xc0));
        nvgSave(args.vg);
        Rect b = Rect(Vec(10, 190), Vec(120, 40));
        nvgScissor(args.vg, b.pos.x, b.pos.y, b.size.x, b.size.y);
        nvgBeginPath(args.vg);
        for (unsigned int i = 0; i < display_buf.size(); i++) {
            float x, y;
            x = (float)i / (display_buf.size() - 1);
            y = display_buf[i] / 2.0 + 0.5;
            Vec p;
            p.x = b.pos.x + b.size.x * x;
            p.y = b.pos.y + b.size.y * (1.0 - y);
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

    void drawLayer(const DrawArgs& args, int layer) override {

        if (module) {
            AudioSample& currentSample = module->current_sample();
            if (layer == 1) {
                // std::shared_ptr<Font> font =
                // APP->window->loadFont(asset::system("res/fonts/DSEG7ClassicMini-BoldItalic.ttf"));
                nvgFontSize(args.vg, 10);
                // nvgFontFaceId(args.vg, font->handle);
                nvgTextLetterSpacing(args.vg, 0);
                nvgFillColor(args.vg, nvgRGBA(0xdd, 0x33, 0x33, 0xff));
                nvgTextBox(args.vg, 7, 16, 247, currentSample.file_display.c_str(), NULL);
                nvgTextBox(args.vg, 167, 16, 97, currentSample.file_info_display.c_str(), NULL);
                // nvgTextBox(args.vg, 9, 26,120, module->debugDisplay.c_str(), NULL);
                // nvgTextBox(args.vg, 109, 26,120, module->debugDisplay2.c_str(), NULL);

                // Zero line
                this->drawZeroLine(args, RGBA(0xff, 0xff, 0xff));

                if (!currentSample.is_empty()) {
                    // Write head line
                    drawMarkerLine(args, RGBA(0xff, 0x00, 0x00), currentSample.write_head / currentSample.num_frames);

                    // Read head line
                    drawMarkerLine(args, RGBA(0x00, 0xff, 0x00), currentSample.read_head / currentSample.num_frames);

                    // Start head line
                    drawMarkerLine(args, RGBA(0x00, 0x00, 0xff), currentSample.start_head / currentSample.num_frames);

                    // Stop head line
                    drawMarkerLine(args, RGBA(0x7f, 0x00, 0x7f), currentSample.stop_head / currentSample.num_frames);

                    // Waveform
                    drawSample(args, currentSample);
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
            display->box.pos = Vec(3, 24);
            display->box.size = Vec(247, 100);
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
