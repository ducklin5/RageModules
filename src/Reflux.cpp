#include <dirent.h>
#include <libgen.h>
#include <sndfile.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../dep/babycat/babycat.h"
#include "./shared/Components.hpp"
#include "plugin.hpp"

enum { AUDIO_SAMPLE_DISPLAY_RES = 1024 };

using namespace rage;
using IdxType = uintptr_t;

struct BabycatWaveformInfo {
    IdxType num_frames = 0;
    IdxType num_channels = 0;
    IdxType num_samples = 0;
    IdxType frame_rate_hz = 0;

    BabycatWaveformInfo(babycat_Waveform* waveform) {
        this->num_samples = babycat_waveform_get_num_samples(waveform);
        this->num_channels = babycat_waveform_get_num_channels(waveform);
        this->num_frames = babycat_waveform_get_num_frames(waveform);
        this->frame_rate_hz = babycat_waveform_get_frame_rate_hz(waveform);
    }
};

struct AudioClip {
    IdxType num_frames = 0;
    IdxType num_channels = 0;
    IdxType frame_rate_hz = 0;
    std::vector<std::vector<float>> raw_waveform;

    std::string file_path;
    std::string file_display;
    std::string file_info_display;
    std::vector<double> display_buf;

    bool is_loaded = false;
    bool is_modified = false;
    bool is_playing = false;
    bool is_recording = false;

    IdxType read_head = 0;
    IdxType write_head = 0;
    IdxType start_head = 0;
    IdxType stop_head = 0;

    void load_babycat_waveform(babycat_Waveform* waveform) {
        const auto info = BabycatWaveformInfo(waveform);
        this->num_frames = info.num_frames;
        this->num_channels = info.num_channels;
        this->frame_rate_hz = info.frame_rate_hz;
        this->raw_waveform.resize(this->num_channels);

        for (IdxType c = 0; c < this->num_channels; c++) {
            this->raw_waveform[c].resize(this->num_frames, 0.0);
            for (IdxType f = 0; f < this->num_frames; f++) {
                this->raw_waveform[c][f] = babycat_waveform_get_unchecked_sample(waveform, f, c);
            }
        }
    }

    bool load_babycat_path(std::string& path) {
        babycat_WaveformArgs waveform_args = babycat_waveform_args_init_default();
        babycat_WaveformResult waveform_result = babycat_waveform_from_file(path.c_str(), waveform_args);
        if (waveform_result.error_num != 0) {
            printf("Failed to load audio clip [%s] with error: %u\n", path.c_str(), waveform_result.error_num);
            return false;
        }

        auto waveform = waveform_result.result;
        {
            // make sure we are working with 1 sample per channel in each frame
            auto info = BabycatWaveformInfo(waveform);
            const int samples_per_channel_per_frame = info.num_samples / (info.num_channels * info.num_frames);
            babycat_waveform_resample(waveform, info.frame_rate_hz * samples_per_channel_per_frame);
        }

        this->load_babycat_waveform(waveform);
        return true;
    }

    void update_display_data() {
        char* path_dup = strdup(this->file_path.c_str());
        std::string file_description = basename(path_dup);
        this->file_display = file_description.substr(0, file_description.size() - 4);
        this->file_display = file_display.substr(0, 20);
        this->file_info_display = std::to_string(this->frame_rate_hz) + "-" + std::to_string(this->num_channels) + "Ch";
        free(path_dup);
    }

    void build_display_buf() {
        this->display_buf.resize(AUDIO_SAMPLE_DISPLAY_RES, 0.0);
        const IdxType skip_amount = this->num_frames / AUDIO_SAMPLE_DISPLAY_RES;
        for (IdxType i = 0; i < AUDIO_SAMPLE_DISPLAY_RES && i < this->num_frames; i++) {
            this->display_buf[i] = this->get_sample(0, i * skip_amount);
        }
    }

    bool load_file(std::string& path) {
        // Try load waveform using babycat
        if (!this->load_babycat_path(path))
            return false;

        this->stop_head = this->num_frames;
        this->file_path = path;
        this->is_loaded = true;
        this->is_modified = false;
        this->update_display_data();
        this->build_display_buf();

        return true;
    }

    bool save_file(std::string& path) {
        SF_INFO info = {
            frames: (int) this->num_frames,
            samplerate: (int) this->frame_rate_hz,
            channels: (int) this->num_channels,
            format: SF_FORMAT_WAV | SF_FORMAT_PCM_24,
            sections: 0,
            seekable: true,
        };

        std::cout << "Writting to file: " << path << std::endl;
        SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
        if (!file) {
            std::cout << "Failed to open output file: " << sf_strerror(file) << std::endl;
            return false;
        }

        const int num_samples = num_channels * num_frames;
        float* save_buffer = new float[num_samples];

        for (IdxType c = 0; c < this->num_channels; c++) {
            for (IdxType f = 0; f < this->num_frames; f++) {
                save_buffer[num_channels * f + c] = get_sample(c, f);
            }
        }

        // Write the stereo sine wave samples to the output file.
        bool result = true;
        const sf_count_t num_written = sf_write_float(file, save_buffer, num_samples);
        if (num_written != num_samples) {
            std::cerr << "Failed to write samples to output file" << std::endl;
            result = false;
        }

        // Close the output file.
        sf_close(file);
        return result;
    }

    double get_sample(IdxType channel_idx, IdxType frame_idx) {
        if (channel_idx >= this->raw_waveform.size() || frame_idx >= this->raw_waveform[channel_idx].size())
            return 0;

        return this->raw_waveform[channel_idx][frame_idx];
    }

    bool set_sample(IdxType channel_idx, IdxType frame_idx, int value, bool overwrite = true) {
        if (channel_idx >= this->num_channels) {
            this->num_channels = channel_idx + 1;
            this->raw_waveform.resize(this->num_channels);
        }
        if (frame_idx >= this->num_frames) {
            this->num_frames = frame_idx + 1;
        }
        if (frame_idx >= this->raw_waveform[channel_idx].size() && overwrite) {
            this->raw_waveform[channel_idx].resize(frame_idx + 1);
        }

        if (overwrite) {
            this->raw_waveform[channel_idx][frame_idx] = value;
        } else {
            auto& channel = this->raw_waveform[channel_idx];
            channel.insert(channel.begin() + frame_idx, value);
        }
        return true;
    }

    bool is_empty() {
        return !this->is_loaded && !is_modified;
    }

    bool has_data() {
        return this->is_loaded || is_modified;
    };

    void start_playing() {
        if (has_data()) {
            this->read_head = start_head;
            this->is_playing = true;
        }
    }

    std::vector<double> read_frame() {
        auto data = std::vector<double>(num_channels);

        if (!is_playing || read_head >= stop_head) {
            read_head = start_head;
            is_playing = false;
            return data;
        }

        for (IdxType channel_idx = 0; channel_idx < num_channels; channel_idx++) {
            data[channel_idx] = get_sample(channel_idx, read_head);
        }

        read_head++;

        return data;
    }

    void write_frame(std::vector<double> channels, bool overwrite = true) {
        for (IdxType cidx = 0; cidx < channels.size(); cidx++) {
            set_sample(cidx, write_head, channels[cidx], overwrite);
        }

        write_head++;
    }
};

struct AudioSlice {
    int clip_index;
    int start;
    int stop;
};

struct Reflux: Module {
    enum ParamIds {
        SELECTED_SAMPLE,
        SELECTED_SLICE,
        SAMPLE_START,
        SAMPLE_STOP,
        SAMPLE_WRITE,
        SAMPLE_READ,
        SAMPLE_RECORD,
        SAMPLE_LOAD,
        SAMPLE_SAVE,
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

    static const int NUM_SAMPLES = 8;
    int selected_sample = 0;
    float prev_sample_trig_value = 0;
    AudioClip clips[NUM_SAMPLES];
    std::string directory_ = "";

    Reflux() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(SELECTED_SAMPLE, 0.0, NUM_SAMPLES - 1, 0.0, "Selected Sample");
        configParam(SAMPLE_START, -INFINITY, INFINITY, 0.0);
    }

    std::string get_last_directory() {
        return this->directory_;
    };

    bool load_file(std::string filepath) {
        const bool loaded = current_sample().load_file(filepath);
        if (loaded)
            directory_ = system::getDirectory(filepath);
        return loaded;
    };

    bool save_file(std::string filepath) {
        const bool saved = current_sample().save_file(filepath);
        if (saved)
            directory_ = system::getDirectory(filepath);
        return saved;
    }

    AudioClip& current_sample() {
        return clips[selected_sample];
    }

    //returns the target frame given
    int lerp_current_sample_frames(int current_frame, float delta) {
        const auto num_frames = current_sample().num_frames;
        float ratio = ((float)current_frame) / num_frames;
        ratio = clamp(ratio + delta, 0.0, 1.0);
        return ratio * num_frames;
    }

    void on_omni_knob_changed(int id, float delta) {
        IdxType* frame = nullptr;
        switch (id) {
            case SAMPLE_START:
                frame = &(current_sample().start_head);
                break;
            case SAMPLE_STOP:
                frame = &(current_sample().stop_head);
                break;
            case SAMPLE_READ:
                frame = &(current_sample().read_head);
                break;
            case SAMPLE_WRITE:
                frame = &(current_sample().write_head);
                break;
            default:
                return;
        }

        *frame = lerp_current_sample_frames(*frame, delta);
    }

    void process(const ProcessArgs& args) override {
        // update the select sample
        selected_sample = getParam(SELECTED_SAMPLE).getValue();

        // listen for sample trigger param event
        if (getParam(SAMPLE_TRIG).getValue() > 0 && prev_sample_trig_value <= 0) {
            current_sample().start_playing();
        }
        prev_sample_trig_value = getParam(SAMPLE_TRIG).getValue();

        // listen for toggle recording param event

        // read audio input to current sample

        // write all playing clips to output
        int clips_playing = 0;
        double audio_out_l = 0;
        double audio_out_r = 0;

        for (int i = 0; i < NUM_SAMPLES; i++) {
            if (clips[i].is_playing) {
                clips_playing += 1;
                auto frame = clips[i].read_frame();
                audio_out_l += frame[0];
                audio_out_r += frame[1];
            }
        }

        clips_playing = std::max(1, clips_playing);
        audio_out_l /= clips_playing;
        audio_out_r /= clips_playing;

        getOutput(OUTPUT_AUDIOL).setVoltage(audio_out_l);
        getOutput(OUTPUT_AUDIOR).setVoltage(audio_out_r);
    }
};

struct RefluxSampleDisplay: TransparentWidget {
    Reflux* module;
    int frame = 0;

    RefluxSampleDisplay() {}

    void draw_line(const DrawArgs& args, NVGcolor color, rack::math::Vec start, rack::math::Vec stop) {
        nvgStrokeColor(args.vg, color);
        {
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, start.x, start.y);
            nvgLineTo(args.vg, stop.x, stop.y);
            nvgClosePath(args.vg);
        }
        nvgStroke(args.vg);
    }

    void draw_rect(const DrawArgs& args, NVGcolor color, rack::math::Rect rect, bool fill = false) {
        auto pos = rect.pos;
        auto size = rect.size;
        if (fill) {
            nvgBeginPath(args.vg);
            nvgFillColor(args.vg, color);
            nvgRect(args.vg, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y);
            nvgFill(args.vg);
            nvgClosePath(args.vg);
        }
        draw_line(args, color, pos, pos + size * Vec(1, 0));
        draw_line(args, color, pos, pos + size * Vec(0, 1));
        draw_line(args, color, pos + size * Vec(1, 0), pos + size * Vec(1, 1));
        draw_line(args, color, pos + size * Vec(0, 1), pos + size * Vec(1, 1));
    }

    void draw_h_line(const DrawArgs& args, NVGcolor color, Rect rect, double pos_ratio) {
        int y_line = floor(pos_ratio * rect.size.y);
        nvgStrokeWidth(args.vg, 0.8);
        this->draw_line(args, color, rect.pos + Vec(0, y_line), rect.pos + Vec(rect.size.x, y_line));
    }

    void draw_v_line(const DrawArgs& args, NVGcolor color, Rect rect, double pos_ratio) {
        int x_line = floor(pos_ratio * rect.size.x);
        nvgStrokeWidth(args.vg, 0.8);
        this->draw_line(args, color, rect.pos + Vec(x_line, 0), rect.pos + Vec(x_line, rect.size.y));
    }

    void draw_text(const DrawArgs& args, NVGcolor color, Rect rect, const char* text) {
        nvgFontSize(args.vg, 8);
        nvgTextLetterSpacing(args.vg, 0);
        nvgFillColor(args.vg, color);
        nvgTextBox(args.vg, rect.pos.x + 2, rect.pos.y + rect.size.y - 2, rect.size.x, text, NULL);
    }

    void draw_sample(const DrawArgs& args, NVGcolor color, Rect rect, AudioClip& sample) {
        auto& display_buf = sample.display_buf;
        nvgStrokeColor(args.vg, color);
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

    SplitResult split_rect_h(const Rect rect, float ratio) {
        const float h = rect.getHeight() * ratio;
        return
        SplitResult {A: Rect(rect.pos, Vec(rect.size.x, h)), B: Rect(rect.pos + Vec(0, h), rect.size - Vec(0, h))};
    }

    SplitResult split_rect_v(const Rect rect, float ratio) {
        const float w = rect.getWidth() * ratio;
        return
        SplitResult {A: Rect(rect.pos, Vec(w, rect.size.y)), B: Rect(rect.pos + Vec(w, 0), rect.size - Vec(w, 0))};
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        const float title_height = 10;
        const auto color_mint = nvgRGB(56, 189, 153);
        const auto color_charcoal = nvgRGB(64, 64, 64);
        const auto color_white = nvgRGBA(255, 255, 255, 90);

        if (module) {
            AudioClip& sample = module->current_sample();
            if (layer == 1) {
                Rect local_box = Rect(Vec(0), box.size);
                Rect header_rect, waveform_rect, file_name_rect, file_info_rect;
                {
                    auto result = split_rect_h(local_box, title_height / local_box.getHeight());
                    header_rect = result.A;
                    waveform_rect = result.B;
                    result = split_rect_v(header_rect, 0.7);
                    file_name_rect = result.A;
                    file_info_rect = result.B;
                }

                this->draw_rect(args, color_charcoal, local_box, true);
                this->draw_rect(args, color_mint, file_name_rect);
                this->draw_rect(args, color_mint, file_info_rect);
                this->draw_rect(args, color_mint, waveform_rect);

                this->draw_text(args, color_mint, file_name_rect, sample.file_display.c_str());
                this->draw_text(args, color_mint, file_info_rect, sample.file_info_display.c_str());

                // Zero Line
                this->draw_h_line(args, color_mint, waveform_rect, 0.5);

                if (!sample.is_empty()) {
                    // Waveform
                    draw_sample(args, color_white, waveform_rect, sample);

                    // marker ratio
                    auto start_ratio = double(sample.start_head) / sample.num_frames;
                    auto stop_ratio = double(sample.stop_head) / sample.num_frames;
                    auto read_ratio = double(sample.read_head) / sample.num_frames;
                    auto write_ratio = double(sample.write_head) / sample.num_frames;

                    // Play region
                    auto color_lite_mint = color_mint;
                    color_lite_mint.a = 0.3;
                    Rect pre_start_rect = split_rect_v(waveform_rect, start_ratio).A;
                    Rect post_stop_rect = split_rect_v(waveform_rect, stop_ratio).B;
                    this->draw_rect(args, color_lite_mint, pre_start_rect, true);
                    this->draw_rect(args, color_lite_mint, post_stop_rect, true);

                    // Start head marker
                    draw_v_line(args, nvgRGB(255, 170, 0), waveform_rect, start_ratio);

                    // Stop head marker
                    draw_v_line(args, nvgRGB(155, 77, 202), waveform_rect, stop_ratio);

                    // Read head marker
                    draw_v_line(args, nvgRGB(30, 144, 255), waveform_rect, read_ratio);

                    // Write head marker
                    draw_v_line(args, nvgRGB(230, 0, 115), waveform_rect, write_ratio);
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

        addParam(createParamCentered<RoundSmallGraySnapKnob>(Vec(30, 205), module, Reflux::SELECTED_SAMPLE));
        addParam(createParamCentered<RoundSmallGrayOmniKnob<Reflux>>(Vec(60, 205), module, Reflux::SAMPLE_START));
        addParam(createParamCentered<RoundSmallGrayOmniKnob<Reflux>>(Vec(90, 205), module, Reflux::SAMPLE_STOP));
        addParam(createParamCentered<RoundSmallGrayOmniKnob<Reflux>>(Vec(120, 205), module, Reflux::SAMPLE_WRITE));
        addParam(createParamCentered<RoundSmallGrayOmniKnob<Reflux>>(Vec(150, 205), module, Reflux::SAMPLE_READ));

        {
            RefluxSampleDisplay* display = new RefluxSampleDisplay();
            display->box.pos = Vec(15, 230);
            display->box.size = Vec(150, 38);
            display->module = module;
            addChild(display);
        }

        addParam(createParamCentered<RubberSmallButton>(Vec(185, 205), module, Reflux::SAMPLE_RECORD));
        addParam(createParamCentered<LoadButton<Reflux>>(Vec(220, 205), module, Reflux::SAMPLE_LOAD));
        addParam(createParamCentered<SaveButton<Reflux>>(Vec(255, 205), module, Reflux::SAMPLE_SAVE));
        addParam(createParamCentered<RubberSmallButton>(Vec(185, 245), module, Reflux::SAMPLE_TRIG));
        addParam(createParamCentered<RubberSmallButton>(Vec(220, 245), module, Reflux::SAMPLE_AUTO_SLICE));
        addParam(createParamCentered<RubberSmallButton>(Vec(255, 245), module, Reflux::SAMPLE_MAKE_SLICE));

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
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(108, 18)), module, Reflux::OUTPUT_EOS));
    }
};

Model* modelReflux = createModel<Reflux, RefluxWidget>("Reflux");
