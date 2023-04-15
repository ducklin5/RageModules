#include <dirent.h>
#include <fmt/core.h>
#include <libgen.h>
#include <math.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plugin.hpp"
#include "src/reflux/audio_base.hpp"
#include "src/reflux/audio_clip.hpp"
#include "src/reflux/audio_slice.hpp"
#include "src/shared/components.hpp"
#include "src/shared/make_builder.hpp"
#include "src/shared/nvg_helpers.hpp"
#include "src/shared/utils.hpp"

// NOLINTNEXTLINE (google-build-using-namespace)
using namespace rage;

struct Reflux: Module {
    enum ParamIds {
        // Slice Knobs
        PARAM_SELECTED_SLICE,
        PARAM_SLICE_START,
        PARAM_SLICE_ATTACK,
        PARAM_SLICE_RELEASE,
        PARAM_SLICE_STOP,

        // Slice Buttons
        PARAM_SLICE_SHIFTL,
        PARAM_SLICE_SHIFTR,
        PARAM_SLICE_DELETE,
        PARAM_SLICE_PLAY,
        PARAM_SLICE_PAUSE,
        PARAM_SLICE_LEARN_MIDI,

        // Clip Knobs
        PARAM_SELECTED_CLIP,
        PARAM_CLIP_START,
        PARAM_CLIP_READ,
        PARAM_CLIP_WRITE,
        PARAM_CLIP_STOP,

        // CLip Buttons
        PARAM_CLIP_RECORD,
        PARAM_CLIP_STOP_REC_SAVE,
        PARAM_CLIP_LOAD,
        PARAM_CLIP_PLAY,
        PARAM_CLIP_PAUSE_MAKE_SLICE,
        PARAM_CLIP_AUTO_SLICE,

        // Global
        PARAM_GLOBAL_TRIG0_TARGET,
        PARAM_GLOBAL_FOLLOW,
        NUM_PARAMS
    };

    enum InputIds {
        INPUT_TRIGGER0,
        INPUT_CV0,

        INPUT_CV1,
        INPUT_CV2,

        INPUT_AUDIOL,
        INPUT_AUDIOR,

        INPUT_TRIGGER1,
        INPUT_TRIGGER2,

        NUM_INPUTS
    };

    enum OutputIds { OUTPUT_AUDIOL, OUTPUT_AUDIOR, OUTPUT_BOS, OUTPUT_EOS, NUM_OUTPUTS };

    enum LightIds {
        LIGHT_SLICE_PLAY,
        LIGHT_CLIP_RECORD,
        LIGHT_CLIP_PLAY,
        LIGHT_CLIP_CLEAR,
        LIGHT_GLOBAL_TRIG0_TARGET,
        LIGHT_GLOBAL_FOLLOW,
        NUM_LIGHTS
    };

    enum InTrigTarget { INTRIG_TARGET_CLIP, INTRIG_TARGET_SLICE };

    static const int NUM_CLIPS = 12;
    DisplayBufferBuilder slice_dbb;
    DisplayBufferBuilder clip_dbb;
    std::array<AudioClip, NUM_CLIPS> clips;
    std::vector<std::shared_ptr<AudioSlice>> slices {};
    std::string directory_;

    InTrigTarget cvtrig0_target = INTRIG_TARGET_CLIP;
    InTrigTarget trig1_target = INTRIG_TARGET_CLIP;
    InTrigTarget trig2_target = INTRIG_TARGET_CLIP;
    bool global_follow = false;

    Eventful<double> selected_clip {0};
    Eventful<double> selected_slice {0};
    std::array<double, PORT_MAX_CHANNELS> selected_clip_cv;
    std::array<double, PORT_MAX_CHANNELS> selected_slice_cv;

    using BooleanTrigger = rack::dsp::BooleanTrigger;

    BooleanTrigger btntrig_slice_shiftl, btntrig_slice_shiftr, btntrig_slice_delete;
    BooleanTrigger btntrig_slice_play, btntrig_slice_pause, btntrig_slice_learn;
    BooleanTrigger btntrig_clip_record, btntrig_clip_play, btntrig_clip_pause;
    BooleanTrigger btntrig_global_trig0_taget, btntrig_global_follow;

    std::array<BooleanTrigger, PORT_MAX_CHANNELS> intrig_trig0;

    rack::dsp::Timer light_timer;

    Reflux() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(PARAM_SELECTED_CLIP, 0.0, NUM_CLIPS - 1, 0.0, "Selected Sample");
        for (int i = 0; i < NUM_CLIPS; i++) {
            clips[i].id = i;
            clips[i].display_buffer_builder = &clip_dbb;
        }
    }

    auto current_clip() -> AudioClip& {
        return clips.at((IdxType)selected_clip);
    }

    auto current_slice() -> AudioSlice* {
        if (slices.size() > selected_slice) {
            return slices.at((IdxType)selected_slice).get();
        }
        return nullptr;
    }

    template<class T>
    T* get_current_waveform() {
        static_assert(sizeof(T) == -1, "Unsupported property type");
    }

    auto get_last_directory() const -> std::string {
        return this->directory_;
    }

    auto can_load() -> bool {
        if (!current_clip().has_data()) {
            return true;
        }

        if (current_clip().can_clear) {
            current_clip().clear();
        } else {
            current_clip().can_clear = true;
        }
        return false;
    }

    auto load_file(std::string filepath) -> bool {
        const bool loaded = current_clip().load_file(filepath);
        if (loaded) {
            directory_ = system::getDirectory(filepath);
        }
        return loaded;
    }

    auto can_save() -> bool {
        if (!current_clip().has_recorded) {
            selected_clip = ((IdxType)selected_clip.value + 1) % NUM_CLIPS;
            return false;
        }
        return true;
    }

    auto save_file(std::string filepath) -> bool {
        const bool saved = current_clip().save_file(filepath);
        if (saved) {
            directory_ = system::getDirectory(filepath);
        }
        return saved;
    }

    // interpolates from current value to the max value using delta
    auto lerp_current_value(
        double current_value,
        float delta,
        double min_value,
        double max_value,
        Optional<double> multiplier = {}
    ) -> double {
        double mult = multiplier.some() ? multiplier.value() : max_value - min_value;
        return clamp(current_value + delta * mult, min_value, max_value);
    }

    void on_omni_knob_changed(int knob_id, float delta) {
        delta *= 3;
        Eventful<double>* value = nullptr;
        double max_value = 0;
        double min_value = 0;
        auto multiplier = Optional<double>();

        auto param_id = (ParamIds)knob_id;

        if (param_id >= PARAM_CLIP_START && param_id <= PARAM_CLIP_STOP) {
            max_value = current_clip().num_frames;
        }

        switch (param_id) {
            case PARAM_SELECTED_CLIP: {
                delta *= 4;
                max_value = NUM_CLIPS - 1;
                value = &selected_clip;
                break;
            }
            case PARAM_CLIP_START:
                value = &current_clip().start_head;
                break;
            case PARAM_CLIP_READ:
                value = &current_clip().read_head;
                break;
            case PARAM_CLIP_WRITE:
                value = &current_clip().write_head;
                // write head is allowed to be 1 frame past the end
                max_value += 1.0;
                break;
            case PARAM_CLIP_STOP:
                value = &current_clip().stop_head;
                break;
            case PARAM_SELECTED_SLICE: {
                const double size = slices.size();
                delta *= (40 + size) / (size + 4);
                max_value = size - 1;
                value = &selected_slice;
                break;
            }
            case PARAM_SLICE_START: {
                if (!current_slice())
                    return;
                value = &current_slice()->start;
                min_value = 0;
                max_value = current_slice()->stop;
                multiplier = Some(max_value - *value + 1);
                break;
            }
            case PARAM_SLICE_STOP: {
                if (!current_slice())
                    return;
                value = &current_slice()->stop;
                min_value = current_slice()->start;
                max_value = current_slice()->clip().num_frames;
                multiplier = Some(*value - min_value + 1);
                break;
            }
            case PARAM_SLICE_ATTACK: {
                if (!current_slice())
                    return;
                value = &current_slice()->attack;
                min_value = current_slice()->start;
                max_value = current_slice()->stop;
                break;
            }
            case PARAM_SLICE_RELEASE: {
                if (!current_slice())
                    return;
                value = &current_slice()->release;
                min_value = current_slice()->start;
                max_value = current_slice()->stop;
                break;
            }
            default:
                return;
        }

        double new_value = lerp_current_value(*value, delta, min_value, max_value, multiplier);
        *value = new_value;
    }

    void process_read_input_audio(const ProcessArgs& args) {
        for (IdxType i = 0; i < NUM_CLIPS; i++) {
            if (clips.at(i).is_recording) {
                if (i == selected_clip) {
                    double data[2];
                    data[0] = getInput(INPUT_AUDIOL).getVoltage();
                    data[1] = getInput(INPUT_AUDIOR).getVoltage();
                    AudioClip::WriteArgs wargs;
                    wargs.delta = args.sampleTime;
                    current_clip().write_frame(data, wargs);
                } else {
                    clips.at(i).is_recording = false;
                }
            }
        }
    }

    void process_read_input_cv(const ProcessArgs& args) {
        float* cv0s = inputs[INPUT_CV0].getVoltages();
        SelectionMode mode = SelectionMode::MIDI_WRAP;

        for (int i = 0; i < PORT_MAX_CHANNELS; i++) {
            switch (cvtrig0_target) {
                case InTrigTarget::INTRIG_TARGET_CLIP:
                    selected_clip_cv[i] = select_idx_by_cv(cv0s[i], mode, clips.size() - 1);
                    break;
                case InTrigTarget::INTRIG_TARGET_SLICE:
                    if (slices.size() > 0) {
                        selected_slice_cv[i] = select_idx_by_cv(cv0s[i], mode, slices.size() - 1);
                    }
                    break;
            }
        }
    }

    void process_read_input_trigs(const ProcessArgs& args) {
        bool use_cv0 = inputs[INPUT_CV0].isConnected();
        for (int i = 0; i < PORT_MAX_CHANNELS; i++) {
            if (intrig_trig0[i].process(inputs[INPUT_TRIGGER0].getVoltage(i) > 0.0)) {
                switch (cvtrig0_target) {
                    case InTrigTarget::INTRIG_TARGET_CLIP: {
                        const int clip_idx = use_cv0 ? (int)selected_clip_cv[i] : (int)selected_clip;
                        clips[clip_idx].start_playing();
                        if (global_follow)
                            selected_clip = clip_idx;
                        break;
                    }
                    case InTrigTarget::INTRIG_TARGET_SLICE: {
                        if (slices.size() > 0) {
                            const int slice_idx = use_cv0 ? (int)selected_slice_cv[i] : (int)selected_slice;
                            slices[slice_idx]->start_playing();
                            if (global_follow)
                                selected_slice = slice_idx;
                        }
                        break;
                    }
                }
            }
        }
    }

    void compute_output(const ProcessArgs& args) {
        int wavefroms_playing = 0;
        double audio_out_l = 0;
        double audio_out_r = 0;

        for (int i = 0; i < NUM_CLIPS; i++) {
            if (clips.at(i).is_playing) {
                wavefroms_playing += 1;
                auto frame = clips.at(i).read_frame();
                audio_out_l += frame[0];
                audio_out_r += frame[1];
            }
        }

        for (int i = 0; i < slices.size(); i++) {
            if (slices.at(i)->is_playing) {
                wavefroms_playing += 1;
                auto frame = slices.at(i)->read_frame();
                if (frame.empty()) {
                    continue;
                }
                audio_out_l += frame[0];
                if (frame.size() > 1) {
                    audio_out_r += frame[1];
                }
            }
        }

        wavefroms_playing = std::max(1, wavefroms_playing);
        audio_out_l /= wavefroms_playing;
        audio_out_r /= wavefroms_playing;

        getOutput(OUTPUT_AUDIOL).setVoltage((float)audio_out_l);
        getOutput(OUTPUT_AUDIOR).setVoltage((float)audio_out_r);
    }

    void process_slices(const ProcessArgs& args) {
        for (int i = 0; i < slices.size(); i++) {
            slices[i]->update_timer(args.sampleTime);
        }
    }

    void update_slices_idx() {
        for (IdxType idx = 0; idx < slices.size(); idx++) {
            slices[idx]->idx = idx;
            slices[idx]->total = slices.size();
        }
    }

    void process(const ProcessArgs& args) override {
        // listen for global trig0 target button event
        if (btntrig_global_trig0_taget.process(params[PARAM_GLOBAL_TRIG0_TARGET].getValue() > 0.0)) {
            cvtrig0_target =
                cvtrig0_target == (INTRIG_TARGET_CLIP && current_slice()) ? INTRIG_TARGET_SLICE : INTRIG_TARGET_CLIP;
        }

        // listen for global follow button event
        if (btntrig_global_follow.process(params[PARAM_GLOBAL_FOLLOW].getValue() > 0.0)) {
            global_follow = !global_follow;
        }

        // read cv input to select sample
        process_read_input_cv(args);

        // read audio input to current sample
        process_read_input_audio(args);

        // listen for start recording button event
        if (btntrig_clip_record.process(params[PARAM_CLIP_RECORD].getValue() > 0.0)) {
            current_clip().toggle_recording();
        }

        // listen for clip play button event
        if (btntrig_clip_play.process(params[PARAM_CLIP_PLAY].getValue() > 0.0)) {
            current_clip().start_playing();
        }

        // listen for clip pause button event
        if (btntrig_clip_pause.process(params[PARAM_CLIP_PAUSE_MAKE_SLICE].getValue() > 0.0)) {
            if (current_clip().is_playing) {
                current_clip().toggle_playing();
            } else if (current_clip().has_data()) {
                // make slice
                std::shared_ptr<AudioSlice> slice = AudioSlice::create(current_clip(), &slice_dbb);
                slices.push_back(slice);
                update_slices_idx();
                selected_slice = slices.size() - 1;
            }
        }

        // listen for clip delete button event
        if (btntrig_slice_play.process(params[PARAM_SLICE_PLAY].getValue() > 0.0)) {
            if (current_slice()) {
                current_slice()->start_playing();
            }
        }

        if (btntrig_slice_pause.process(params[PARAM_SLICE_PAUSE].getValue() > 0.0)) {
            if (current_slice() && current_slice()->is_playing) {
                current_slice()->toggle_playing();
            }
        }

        if (btntrig_slice_delete.process(params[PARAM_SLICE_DELETE].getValue() > 0.0)) {
            if (current_slice()) {
                slices.erase(slices.begin() + selected_slice);
                update_slices_idx();
                if (selected_slice >= 1.0) {
                    selected_slice -= 1.0;
                }
            }
        }

        if (btntrig_slice_shiftl.process(params[PARAM_SLICE_SHIFTL].getValue() > 0.0)) {
            if (selected_slice - 1 >= 0) {
                std::swap(slices[selected_slice], slices[selected_slice - 1.0]);
                selected_slice = selected_slice - 1.0;
                update_slices_idx();
            }
        }

        if (btntrig_slice_shiftr.process(params[PARAM_SLICE_SHIFTR].getValue() > 0.0)) {
            if (selected_slice + 1.0 < slices.size()) {
                std::swap(slices[selected_slice], slices[selected_slice + 1.0]);
                selected_slice += 1.0;
                update_slices_idx();
            }
        }

        process_read_input_trigs(args);

        // update lights
        if (light_timer.process(args.sampleTime) > rage::UI_update_time) {
            light_timer.reset();
            if (current_slice()) {
                lights[LIGHT_SLICE_PLAY].setSmoothBrightness(current_slice()->is_playing ? .5f : 0.0f, UI_update_time);
            }
            lights[LIGHT_CLIP_RECORD].setSmoothBrightness(current_clip().is_recording ? .5f : 0.0f, UI_update_time);
            lights[LIGHT_CLIP_CLEAR].setSmoothBrightness(current_clip().can_clear ? .5f : 0.0f, UI_update_time);
            lights[LIGHT_CLIP_PLAY].setSmoothBrightness(current_clip().is_playing ? .5f : 0.0f, UI_update_time);
            bool trig0_target_slice = cvtrig0_target == INTRIG_TARGET_SLICE;
            lights[LIGHT_GLOBAL_TRIG0_TARGET].setSmoothBrightness(trig0_target_slice ? .5f : 0.0f, UI_update_time);
            lights[LIGHT_GLOBAL_FOLLOW].setSmoothBrightness(global_follow ? .5f : 0.0f, UI_update_time);
        }

        // process_clips
        process_slices(args);

        // compute_output
        compute_output(args);
    }

    json_t* dataToJson() override {
        json_t* json_root = json_object();
        json_t* json_clips = json_array();
        json_t* json_slices = json_array();

        for (auto& clip : clips) {
            json_array_append_new(json_clips, clip.make_json_obj());
        }

        for (auto& slice : slices) {
            json_array_append_new(json_slices, slice->make_json_obj());
        }

        json_object_set_new(json_root, "clips", json_clips);
        json_object_set_new(json_root, "slices", json_slices);
        json_object_set_new(json_root, "cvtrig0_target", json_integer((int)cvtrig0_target));

        return json_root;
    }

    void dataFromJson(json_t* root) override {
        json_t* json_clips = json_object_get(root, "clips");
        json_t* json_slices = json_object_get(root, "slices");

        IdxType idx = 0;
        json_t* json_obj = nullptr;

        json_array_foreach(json_clips, idx, json_obj) {
            clips.at(idx).load_json(json_obj);
        }

        json_array_foreach(json_slices, idx, json_obj) {
            const double clip_idx = json_real_value(json_object_get(json_obj, "clip_idx"));
            auto slice = AudioSlice::create(clips.at(clip_idx), &slice_dbb);
            slice->load_json(json_obj);
            slices.push_back(slice);
        }
        cvtrig0_target = (Reflux::InTrigTarget)json_integer_value(json_object_get(root, "cvtrig0_target"));
    }

    void onAdd(const AddEvent& event) override {
        for (IdxType i = 0; i < clips.size(); i++) {
            Optional<std::string> path;
            if (clips.at(i).has_recorded) {
                std::string filename = fmt::format("clip_{}.wav", i);
                path.set_value(system::join(createPatchStorageDirectory(), filename));
            } else if (clips.at(i).has_loaded) {
                path.set_value(clips.at(i).file_path);
            }
            if (path.some()) {
                auto pathv = path.value();
                if (system::isFile(pathv))
                    clips.at(i).load_file(pathv);
            }
        }
    }

    void onSave(const SaveEvent& e) override {
        for (IdxType i = 0; i < clips.size(); i++) {
            Optional<std::string> path;
            if (clips.at(i).has_recorded) {
                std::string filename = fmt::format("clip_{}.wav", i);
                path.set_value(system::join(createPatchStorageDirectory(), filename));
            }
            if (path.some()) {
                auto pathv = path.value();
                clips.at(i).save_file(pathv);
            }
        }
    }

    void onReset() override {
        slices = {};
        clips = {};
    }

    void onRandomize() override {
        // TODO
    }
};

template<>
AudioClip* Reflux::get_current_waveform<AudioClip>() {
    return &current_clip();
}

template<>
AudioSlice* Reflux::get_current_waveform<AudioSlice>() {
    return current_slice();
}

using ColorSchemeMap = std::map<std::string, NVGcolor>;

const ColorSchemeMap default_colors = {
    {"start", nvgRGB(255, 170, 0)},
    {"stop", nvgRGB(155, 77, 202)},
    {"attack", nvgRGB(255, 170, 0)},
    {"release", nvgRGB(155, 77, 202)},
    {"read", nvgRGB(30, 144, 255)},
    {"write", nvgRGB(230, 0, 115)},
    {"region", nvgRGB(56, 189, 153)},
    {"borders", nvgRGB(56, 189, 153)},
    {"background", nvgRGB(64, 64, 64)},
    {"text", nvgRGBA(255, 255, 255, 90)},
};

template<class WaveformType>
struct WaveformDisplayWidget: TransparentWidget {
    Reflux* module {nullptr};
    const WaveformType* waveform;
    const ColorSchemeMap& colorscheme;

    WaveformDisplayWidget(const ColorSchemeMap& colorscheme = default_colors) :
        TransparentWidget(),
        colorscheme(colorscheme) {}

    void draw_waveform(const DrawArgs& args, NVGcolor color, Rect rect) {
        const auto& display_buf = waveform->get_display_buf();
        const auto samples = display_buf[0].size();
        if (samples <= 0) {
            return;
        }

        const auto rect_center = rect.getCenter();
        auto fill_color = color;
        fill_color.a -= 0.2;

        nvgSave(args.vg);
        nvgScissor(args.vg, rect.pos.x, rect.pos.y, rect.size.x, rect.size.y);
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, rect.pos.x, rect_center.y);

        for (unsigned int i = 0; i < samples; i++) {
            const float spx = rect.pos.x + rect.size.x * ((float)i / ((float)samples - 1));
            const float spy = rect_center.y + (rect.size.y / 2) * ((float)display_buf[0][i]);
            nvgLineTo(args.vg, spx, spy);
        }
        if (display_buf[1].size() >= samples) {
            for (unsigned int i = samples - 1; i > 0; i--) {
                const float spx = rect.pos.x + rect.size.x * ((float)i / ((float)samples - 1));
                const float spy = rect_center.y - (rect.size.y / 2) * ((float)display_buf[1][i]);
                nvgLineTo(args.vg, spx, spy);
            }
        }

        nvgLineTo(args.vg, rect.pos.x, rect_center.y);

        nvgFillColor(args.vg, fill_color);
        nvgStrokeColor(args.vg, color);
        nvgLineCap(args.vg, NVG_ROUND);
        nvgMiterLimit(args.vg, 1.0);
        nvgStrokeWidth(args.vg, 0.6);
        nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
        nvgStroke(args.vg);
        nvgFill(args.vg);
        nvgResetScissor(args.vg);
        nvgRestore(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        const float title_height = 10;
        const auto color_borders = default_colors.at("borders");
        const auto color_bg = default_colors.at("background");
        const auto color_txt = default_colors.at("text");

        if ((bool)module) {
            waveform = module->get_current_waveform<WaveformType>();
            if (layer == 1) {
                const Rect local_box = Rect(Vec(0), box.size);
                Rect waveform_rect;
                Rect title_rect;
                Rect info_rect;
                {
                    auto result = split_rect_h(local_box, title_height / local_box.getHeight());
                    auto header_rect = result.A;
                    waveform_rect = result.B;
                    result = split_rect_v(header_rect, 0.6);
                    title_rect = result.A;
                    info_rect = result.B;
                }

                draw_rect(args, color_bg, local_box, true);
                draw_rect(args, color_borders, title_rect);
                draw_rect(args, color_borders, info_rect);

                // Zero Line
                draw_h_line(args, color_borders, waveform_rect, 0.5);

                if (waveform) {
                    // Text
                    draw_text(args, color_borders, title_rect, waveform->get_text_title());
                    draw_text(args, color_borders, info_rect, waveform->get_text_info());

                    if (waveform->has_data()) {
                        // Waveform
                        draw_waveform(args, color_txt, waveform_rect);

                        // Regions
                        for (Region& region : waveform->get_regions()) {
                            auto color = colorscheme.at(region.tag);
                            color.a = 0.3;
                            auto rect = split_rect_v(waveform_rect, region.end).A;
                            rect = split_rect_v(rect, region.begin / region.end).B;
                            draw_rect(args, color, rect, true);
                        }

                        // Markers
                        for (Marker& marker : waveform->get_markers()) {
                            auto color = colorscheme.at(marker.tag);
                            draw_v_line(args, color, waveform_rect, marker.pos);
                        }
                    }
                }

                draw_rect(args, color_borders, waveform_rect);
            }
        }
        Widget::drawLayer(args, layer);
    }
};

struct TextDisplayWidget: TransparentWidget {
    Reflux* module {nullptr};
    std::string text;
    std::function<std::string()> get_txt;

    TextDisplayWidget(std::function<std::string()> get_txt) : TransparentWidget(), get_txt(get_txt) {}

    void drawLayer(const DrawArgs& args, int layer) override {
        const auto color_txt = default_colors.at("text");
        const auto color_borders = default_colors.at("borders");
        const Rect text_rect = Rect(Vec(0), box.size);
        if (layer == 0) {
            text = get_txt();
            draw_rect(args, color_borders, text_rect);
            draw_text(args, color_txt, Rect(Vec(0), box.size), text);
        }
    }
};

struct RefluxWidget: ModuleWidget {
    struct WidgetIdGroup {
        int first_id;
        int count;
    };

    using WidgetCreator = Widget* (*)(WidgetType, Vec, Module*, int);
    using WidgetTypeMap = std::unordered_map<int, WidgetType>;
    struct WidgetGrirdArgs {
        Vec pos;
        Vec spacing = Vec(35, 40);
        int columns = std::numeric_limits<int>::max();
        WidgetIdGroup group;
        WidgetType default_type = WTRegularButton;
        WidgetTypeMap custom_types = {};
        WidgetCreator create_widget = &create_centered_widget<Reflux>;
    };

    MAKE_BUILDER(WGArgs, WidgetGrirdArgs, pos, spacing, columns, group, default_type, custom_types, create_widget);

    void add_widget_grid(WidgetGrirdArgs args) {
        const int columns = args.columns;
        const Vec spacing = args.spacing;
        const auto custom_types = args.custom_types;

        for (int idx = 0; idx < args.group.count; idx++) {
            const int param_id = args.group.first_id + idx;
            const Vec pos = args.pos + Vec((float)(idx % columns) * spacing.x, (float)(idx / columns) * spacing.y);
            const WidgetType wtype = ((bool)custom_types.count(idx)) ? custom_types.at(idx) : args.default_type;
            addChild(args.create_widget(wtype, pos, module, param_id));
        }
    }

    template<class WavefromType>
    void add_waveform_group(Vec pos, WidgetIdGroup group, const WidgetTypeMap& custom_types = {}) {
        add_widget_grid(
            WGArgs().group(group).pos(pos).spacing(Vec(30.0F, 40.F)).default_type(WTOmniKnob).custom_types(custom_types)
        );

        auto* display = new WaveformDisplayWidget<WavefromType>();
        display->box.pos = pos + Vec(-15, 25);
        display->box.size = Vec(150, 38);
        display->module = dynamic_cast<Reflux*>(module);
        addChild(display);
    }

    void add_info_display(Vec pos, std::function<std::string()> get_text) {
        auto* display = new TextDisplayWidget(get_text);
        display->box.pos = pos;
        addChild(display);
    }

    template<class T>
    using RSBLed = RubberSmallButtonLed<T>;

    explicit RefluxWidget(Reflux* module) {
        const auto slice_group_pos = Vec(30, 110);
        const auto clip_group_pos = Vec(30, 205);

        setModule(module);

        // Panel Background
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Reflux.svg")));

        // Screws
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Audio Slice
        add_waveform_group<AudioSlice>(slice_group_pos, {Reflux::PARAM_SELECTED_SLICE, 5});
        add_widget_grid(WGArgs().pos(Vec(185, 110)).group({Reflux::PARAM_SLICE_SHIFTL, 6}).columns(3));
        addChild(createLightCentered<RSBLed<BlueLight>>(Vec(185, 150), module, Reflux::LIGHT_SLICE_PLAY));

        // Audio Clip
        const WidgetTypeMap btns = {{1, WTSaveButton}, {2, WTLoadButton}};
        add_waveform_group<AudioClip>(clip_group_pos, {Reflux::PARAM_SELECTED_CLIP, 5});
        add_widget_grid(WGArgs().pos(Vec(185, 205)).group({Reflux::PARAM_CLIP_RECORD, 6}).columns(3).custom_types(btns)
        );
        addChild(createLightCentered<RSBLed<RedLight>>(Vec(185, 205), module, Reflux::LIGHT_CLIP_RECORD));
        addChild(createLightCentered<RSBLed<RedLight>>(Vec(255, 205), module, Reflux::LIGHT_CLIP_CLEAR));
        addChild(createLightCentered<RSBLed<BlueLight>>(Vec(185, 245), module, Reflux::LIGHT_CLIP_PLAY));

        // Global
        add_widget_grid(WGArgs().pos(Vec(170, 300)).group({Reflux::PARAM_GLOBAL_TRIG0_TARGET, 2}).columns(3));
        addChild(createLightCentered<RSBLed<BlueLight>>(Vec(170, 300), module, Reflux::LIGHT_GLOBAL_TRIG0_TARGET));
        addChild(createLightCentered<RSBLed<BlueLight>>(Vec(205, 300), module, Reflux::LIGHT_GLOBAL_FOLLOW));
        /*
        if (module) {
            add_info_display(Vec(170, 400), [module]() {
                return fmt::format("Clip {}/{}", module->selected_clip_cv.value + 1, 12);
            });
            add_info_display(Vec(170, 450), [module]() {
                return fmt::format("Slice {}/{}", module->selected_slice_cv. + 1, module->slices.size());
            });
        }
        */
        // Inputs
        add_widget_grid(WGArgs()
                            .pos(mm2px(Vec(10, 101)))
                            .group({Reflux::INPUT_TRIGGER0, 8})
                            .columns(4)
                            .default_type(WidgetType::WTInputPort)
                            .spacing(mm2px(Vec(10, 11.5))));

        // Outputs
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(103, 10)), module, Reflux::OUTPUT_AUDIOL));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(113, 10)), module, Reflux::OUTPUT_AUDIOR));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(108, 18)), module, Reflux::OUTPUT_EOS));
    }
};

Model* modelReflux = createModel<Reflux, RefluxWidget>("Reflux");
