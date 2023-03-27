#include <dirent.h>
#include <libgen.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../dep/babycat/babycat.h"
#include "./shared/components.hpp"
#include "./shared/nvg_helpers.hpp"
#include "./shared/optional.hpp"
#include "plugin.hpp"

enum { AUDIO_SAMPLE_DISPLAY_RES = 256 };
enum { AUDIO_SAMPLE_DISPLAY_CHANNELS = 2 };

// NOLINTNEXTLINE (google-build-using-namespace)
using namespace rage;
using IdxType = uintptr_t;
using DisplayBufferType = std::array<std::vector<double>, 2>;

struct Marker {
    float pos;
    std::string tag;

    bool operator<(const Marker& other) const {
        return pos < other.pos;
    }
};

class Stamp {
  public:
    const IdxType id;
    Marker marker;

    Stamp(float pos, std::string tag) : id(take_next_id()), marker({pos, tag}) {}

    bool operator<(const Stamp& other) const {
        return marker < other.marker;
    }

  private:
    static IdxType next_id;

    static IdxType take_next_id() {
        return ++next_id;
    }
};

IdxType Stamp::next_id = 0;

struct Region {
    float begin;
    float end;
    std::string tag;

    Region(float begin, float end, const std::string& tag = "region") : begin(begin), end(end), tag(tag) {}
};

struct BabycatWaveformInfo {
    IdxType num_frames;
    IdxType num_channels;
    IdxType num_samples;
    IdxType frame_rate_hz;

    explicit BabycatWaveformInfo(babycat_Waveform* waveform) :
        num_frames(babycat_waveform_get_num_frames(waveform)),
        num_channels(babycat_waveform_get_num_channels(waveform)),
        num_samples(babycat_waveform_get_num_samples(waveform)),
        frame_rate_hz(babycat_waveform_get_frame_rate_hz(waveform)) {}
};

struct AudioClip {
    int id = 0;
    IdxType num_frames = 0;
    IdxType num_channels = 0;
    IdxType frame_rate_hz = 0;
    std::vector<std::vector<double>> raw_data;

    std::string file_path;
    std::string file_display;
    std::string file_info_display;
    DisplayBufferType display_buf;

    bool has_loaded = false;
    bool has_recorded = false;
    bool is_playing = false;
    bool is_recording = false;

    IdxType read_head = 0;
    IdxType write_head = 1;
    IdxType start_head = 0;
    IdxType stop_head = 0;

    std::vector<std::reference_wrapper<const Stamp>> stamps = {};

    rack::dsp::Timer write_timer;

    AudioClip() : file_path("Untitled.***"), file_display(""), file_info_display("") {
        this->display_buf.fill({});
        this->update_display_data();
    }

    void load_babycat_waveform(babycat_Waveform* waveform) {
        const auto info = BabycatWaveformInfo(waveform);
        this->num_frames = info.num_frames;
        this->num_channels = info.num_channels;
        this->frame_rate_hz = info.frame_rate_hz;
        this->raw_data.resize(this->num_channels);

        for (IdxType cidx = 0; cidx < this->num_channels; cidx++) {
            this->raw_data[cidx].resize(this->num_frames, 0.0);
            for (IdxType fidx = 0; fidx < this->num_frames; fidx++) {
                this->raw_data[cidx][fidx] = babycat_waveform_get_unchecked_sample(waveform, fidx, cidx);
            }
        }
    }

    auto load_babycat_path(std::string& path) -> bool {
        const babycat_WaveformArgs waveform_args = babycat_waveform_args_init_default();
        const babycat_WaveformResult waveform_result = babycat_waveform_from_file(path.c_str(), waveform_args);
        if (waveform_result.error_num != 0) {
            printf("Failed to load audio clip [%s] with error: %u\n", path.c_str(), waveform_result.error_num);
            return false;
        }

        auto* waveform = waveform_result.result;
        {
            // make sure we are working with 1 sample per channel in each frame
            auto info = BabycatWaveformInfo(waveform);
            const IdxType samples_per_channel_per_frame = info.num_samples / (info.num_channels * info.num_frames);
            babycat_waveform_resample(waveform, info.frame_rate_hz * samples_per_channel_per_frame);
        }

        this->load_babycat_waveform(waveform);
        return true;
    }

    void update_display_data() {
        char* path_dup = strdup(this->file_path.c_str());
        std::string const file_description = basename(path_dup);
        this->file_display = file_description.substr(0, file_description.size() - 4);
        this->file_display = file_display.substr(0, 20);
        this->file_info_display =
            std::to_string(this->frame_rate_hz) + "Hz-" + std::to_string(this->num_channels) + "Ch";
        free(path_dup);
    }

    void build_display_buf() {
        for (int cidx = 0; cidx < AUDIO_SAMPLE_DISPLAY_CHANNELS; cidx++) {
            display_buf[cidx].resize(AUDIO_SAMPLE_DISPLAY_RES, 0.0);
            const IdxType chunk_size = this->num_frames / AUDIO_SAMPLE_DISPLAY_RES;
            IdxType pos = 0;
            double max = 0;
            for (IdxType i = 0; i < AUDIO_SAMPLE_DISPLAY_RES; i++) {
                double accum = 0.0;
                for (IdxType j = 0; j < chunk_size; j++) {
                    accum += std::abs(get_sample(cidx, pos++));
                }
                display_buf[cidx][i] = accum / ((double)chunk_size);
                max = std::max(display_buf[cidx][i], max);
            }
        }
    }

    auto load_file(std::string& path) -> bool {
        // Try load waveform using babycat
        if (!this->load_babycat_path(path))
            return false;

        this->stop_head = this->num_frames;
        this->file_path = path;
        this->has_loaded = true;
        this->has_recorded = false;
        this->update_display_data();
        this->build_display_buf();

        return true;
    }

    auto save_file(std::string& path) -> bool {
        SF_INFO info = {
            frames: (int)this->num_frames,
            samplerate: (int)this->frame_rate_hz,
            channels: (int)this->num_channels,
            format: SF_FORMAT_WAV | SF_FORMAT_PCM_24,
            sections: 1,
            seekable: 1,
        };

        std::cout << "Writting to file: " << path << std::endl;
        SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
        if (!(bool)file) {
            std::cout << "Failed to open output file: " << sf_strerror(file) << std::endl;
            return false;
        }

        const auto num_samples = (sf_count_t)(num_channels * num_frames);
        auto save_buffer = std::vector<double>(num_samples);

        for (IdxType cidx = 0; cidx < this->num_channels; cidx++) {
            for (IdxType fidx = 0; fidx < this->num_frames; fidx++) {
                save_buffer[num_channels * fidx + cidx] = get_sample(cidx, fidx);
            }
        }

        // Write the stereo sine wave samples to the output file.
        bool result = true;
        const sf_count_t num_written = sf_write_double(file, save_buffer.data(), num_samples);
        if (num_written != num_samples) {
            std::cerr << "Failed to write samples to output file" << std::endl;
            result = false;
        }

        // Close the output file.
        sf_close(file);

        if (result) {
            this->file_path = path;
            this->update_display_data();
        }

        return result;
    }

    auto get_sample(IdxType channel_idx, IdxType frame_idx) -> double {
        if (channel_idx >= this->raw_data.size() || frame_idx >= this->raw_data[channel_idx].size()) {
            return 0;
        }
        return this->raw_data[channel_idx][frame_idx];
    }

    auto set_sample(IdxType channel_idx, IdxType frame_idx, double value, bool overwrite = true) -> bool {
        if (channel_idx >= this->num_channels) {
            this->num_channels = channel_idx + 1;
            this->raw_data.resize(this->num_channels);
        }
        if (frame_idx >= this->num_frames) {
            this->num_frames = frame_idx + 1;
        }
        if (frame_idx >= this->raw_data[channel_idx].size() && overwrite) {
            this->raw_data[channel_idx].resize(frame_idx + 1);
        }

        if (overwrite) {
            this->raw_data[channel_idx][frame_idx] = value;
        } else {
            auto& channel = this->raw_data[channel_idx];
            channel.insert(channel.begin() + frame_idx, value);
        }
        return true;
    }

    auto has_data() const -> bool {
        return has_loaded || has_recorded;
    }

    void start_playing() {
        if (has_data()) {
            this->read_head = start_head;
            this->is_playing = true;
        }
    }

    void toggle_playing() {
        if (this->is_playing) {
            this->is_playing = false;
        } else {
            start_playing();
        }
    }

    void toggle_recording() {
        this->is_recording = !(this->is_recording);
        if (is_recording) {
        }
    }

    auto read_frame() -> std::vector<double> {
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

    struct WriteArgs {
        bool overwrite {true};
        float delta {0.0};
        WriteArgs() {}  // NOLINT
    };

    void write_frame(const std::vector<double>& channels, WriteArgs args = {}) {
        if (!is_recording) {
            return;
        }

        for (IdxType cidx = 0; cidx < channels.size(); cidx++) {
            set_sample(cidx, write_head, channels[cidx], args.overwrite);
        }

        this->has_recorded = true;

        if (write_timer.process(args.delta) > rage::UI_update_time) {
            write_timer.reset();
            this->update_display_data();
            this->build_display_buf();
        }

        if (write_head == stop_head) {
            is_recording = false;
        } else if (write_head > stop_head) {
            stop_head = write_head;
        }

        write_head++;
    }

    void fix_heads() {
        stop_head = std::max(start_head, stop_head);
        read_head = std::max(start_head, read_head);
        read_head = std::min(stop_head, read_head);
        //write_head = std::max(start_head, write_head);
    }

    const DisplayBufferType& get_display_buf() const {
        return display_buf;
    }

    std::vector<Marker> get_markers() const {
        auto start_ratio = float(start_head) / num_frames;
        auto stop_ratio = float(stop_head) / num_frames;
        auto read_ratio = float(read_head) / num_frames;
        auto write_ratio = float(write_head) / num_frames;
        return {
            Marker {start_ratio, "start"},
            Marker {stop_ratio, "stop"},
            Marker {read_ratio, "read"},
            Marker {write_ratio, "write"},
        };
    }

    std::vector<Region> get_regions() const {
        auto start_ratio = float(start_head) / num_frames;
        auto stop_ratio = float(stop_head) / num_frames;

        return {
            Region {0.0F, start_ratio},
            Region {stop_ratio, 1.0F},
        };
    }

    auto get_text_title() const -> std::string {
        return "";
    }

    auto get_text_info() const -> std::string {
        return "";
    }

    void add_stamp(const Stamp& stamp) {
        stamps.push_back(stamp);
        sort_stamps();
    }

    using StampRefType = const std::reference_wrapper<const Stamp>&;

    void sort_stamps() {
        std::sort(stamps.begin(), stamps.end(), [](StampRefType stamp1, StampRefType stamp2) {
            return stamp1.get() < stamp2.get();
        });
    }

    auto find_stamp(const Stamp& stamp) const -> int {
        auto iter = std::find_if(stamps.begin(), stamps.end(), [&](StampRefType stamp_ref) {
            return stamp_ref.get().id == stamp.id;
        });
        if (iter == stamps.end()) {
            return -1;
        }
        return std::distance(stamps.begin(), iter);
    }

    void remove_stamp(const Stamp& stamp) {
        const int idx = find_stamp(stamp);
        if (idx >= 0) {
            stamps.erase(stamps.begin() + idx);
        }
    }
};

struct AudioSlice {
  private:
    AudioClip& m_clip;
    int m_start = 0;
    int m_stop = 0;
    int m_slice_id = 0;
    Stamp m_stamp;
    DisplayBufferType display_buf;

  public:
    AudioSlice(AudioClip& clip, int start = 0, int stop = 0) :
        m_clip(clip),
        m_start(start),
        m_stop(stop),
        m_stamp(Stamp(0, "")) {
        update_stamp();
        m_clip.add_stamp(m_stamp);
    }

    void update_stamp() {
        m_stamp.marker.pos = (float)m_start / m_clip.num_frames;
        m_stamp.marker.tag = "start";
    }

    bool has_data() const {
        return true;
    }

    auto get_text_title() const -> std::string {
        int clip_slice_index = m_clip.find_stamp(m_stamp);
        return "clip-" + std::to_string(m_clip.id) + "-slice-" + std::to_string(clip_slice_index);
    }

    auto get_text_info() const -> std::string {
        return std::to_string(m_slice_id);
    }

    std::vector<Marker> get_markers() const {
        return {};
    }

    std::vector<Region> get_regions() const {
        return {};
    }

    const DisplayBufferType& get_display_buf() const {
        return display_buf;
    }

    ~AudioSlice() {
        m_clip.remove_stamp(m_stamp);
    }
};

struct Reflux: Module {
    enum ParamIds {
        // Slice Knobs
        PARAM_SELECTED_SLICE,
        PARAM_SLICE_START,
        PARAM_SLICE_ATTACK,
        PARAM_SLICE_RELEASE,
        PARAM_SLICE_STOP,

        // Slice Buttons 1
        PARAM_SLICE_PLAY,
        PARAM_SLICE_PAUSE,
        PARAM_SLICE_DELETE,

        // Slice Buttons 2
        PARAM_SLICE_SHIFT_L,
        PARAM_SLICE_SHIFT_R,
        PARAM_SLICE_LEARN_MIDI,

        // Sample Knobs
        PARAM_SELECTED_SAMPLE,
        PARAM_SAMPLE_START,
        PARAM_SAMPLE_READ,
        PARAM_SAMPLE_WRITE,
        PARAM_SAMPLE_STOP,

        // Sample Buttons 1
        PARAM_SAMPLE_RECORD,
        PARAM_SAMPLE_STOP_REC_SAVE,
        PARAM_SAMPLE_LOAD,

        // Sample Buttons 2
        PARAM_SAMPLE_PLAY,
        PARAM_SAMPLE_PAUSE_MAKE_SLICE,
        PARAM_SAMPLE_AUTO_SLICE,

        // Global 1
        PARAM_GLOBAL_DRY,
        PARAM_GLOBAL_GATE_MODE,
        PARAM_GLOBAL_OVERWRITE,
        PARAM_GLOBAL_REC_MANY,
        PARAM_GLOBAL_SLICES,
        PARAM_GLOBAL_SLICE_CV_MODE,
        PARAM_GLOBAL_SLICE_CV_ATNV,
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

    enum LightIds { LIGHT_SAMPLE_RECORD, LIGHT_SAMPLE_PLAY, NUM_LIGHTS };

    static const int NUM_CLIPS = 12;
    IdxType selected_sample = 0;
    IdxType selected_slice = 0;
    std::array<AudioClip, NUM_CLIPS> clips;
    std::vector<AudioSlice> slices {};
    std::string directory_;

    rack::dsp::BooleanTrigger record_button_trigger, play_button_trigger, pause_button_trigger;

    rack::dsp::Timer light_timer;

    Reflux() {
        clips.fill(AudioClip());
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(PARAM_SELECTED_SAMPLE, 0.0, NUM_CLIPS - 1, 0.0, "Selected Sample");
    }

    auto current_clip() -> AudioClip& {
        return clips.at(selected_sample);
    }

    auto current_slice() -> AudioSlice* {
        if (slices.size() > selected_slice) {
            return &slices.at(selected_slice);
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

    auto load_file(std::string filepath) -> bool {
        const bool loaded = current_clip().load_file(filepath);
        if (loaded) {
            directory_ = system::getDirectory(filepath);
        }
        return loaded;
    }

    auto can_save() -> bool {
        if (current_clip().is_recording) {
            current_clip().toggle_recording();
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

    //returns the target frame given
    auto lerp_current_sample_frames(IdxType current_frame, float delta) -> IdxType {
        const auto num_frames = current_clip().num_frames + 1;
        float ratio = ((float)current_frame) / ((float)num_frames);
        ratio = clamp(ratio + delta, 0.0, 1.0);
        return (IdxType)(ratio * num_frames);
    }

    void on_omni_knob_changed(int param_id, float delta) {
        IdxType* frame = nullptr;
        switch (param_id) {
            case PARAM_SAMPLE_START:
                frame = &(current_clip().start_head);
                break;
            case PARAM_SAMPLE_STOP:
                frame = &(current_clip().stop_head);
                break;
            case PARAM_SAMPLE_READ:
                frame = &(current_clip().read_head);
                break;
            case PARAM_SAMPLE_WRITE:
                frame = &(current_clip().write_head);
                break;
            default:
                return;
        }

        *frame = lerp_current_sample_frames(*frame, delta);
        current_clip().fix_heads();
    }

    void process_read_input_audio(const ProcessArgs& args) {
        for (IdxType i = 0; i < NUM_CLIPS; i++) {
            if (clips.at(i).is_recording) {
                if (i == selected_sample) {
                    auto data = std::vector<double>(2);
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

    void process_play_clips() {
        int clips_playing = 0;
        double audio_out_l = 0;
        double audio_out_r = 0;

        for (int i = 0; i < NUM_CLIPS; i++) {
            if (clips.at(i).is_playing) {
                clips_playing += 1;
                auto frame = clips.at(i).read_frame();
                audio_out_l += frame[0];
                audio_out_r += frame[1];
            }
        }

        clips_playing = std::max(1, clips_playing);
        audio_out_l /= clips_playing;
        audio_out_r /= clips_playing;

        getOutput(OUTPUT_AUDIOL).setVoltage((float)audio_out_l);
        getOutput(OUTPUT_AUDIOR).setVoltage((float)audio_out_r);
    }

    void process(const ProcessArgs& args) override {
        // update the select sample
        selected_sample = (int)getParam(PARAM_SELECTED_SAMPLE).getValue();

        // listen for start recording button event
        if (record_button_trigger.process(params[PARAM_SAMPLE_RECORD].getValue() > 0.0)) {
            current_clip().toggle_recording();
        }

        // listen for sample play button event
        if (play_button_trigger.process(params[PARAM_SAMPLE_PLAY].getValue() > 0.0)) {
            current_clip().start_playing();
        }

        // listen for sample pause button event
        if (pause_button_trigger.process(params[PARAM_SAMPLE_PAUSE_MAKE_SLICE].getValue() > 0.0)) {
            if (current_clip().is_playing) {
                current_clip().toggle_playing();
            } else {
                // make slice
            }
        }

        // update lights
        if (light_timer.process(args.sampleTime) > rage::UI_update_time) {
            light_timer.reset();
            lights[LIGHT_SAMPLE_PLAY].setSmoothBrightness(current_clip().is_playing ? .5f : 0.0f, UI_update_time);
            lights[LIGHT_SAMPLE_RECORD].setSmoothBrightness(current_clip().is_recording ? .5f : 0.0f, UI_update_time);
        }

        // read audio input to current sample
        process_read_input_audio(args);

        // write all playing clips to output
        process_play_clips();
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
    {"read", nvgRGB(30, 144, 255)},
    {"write", nvgRGB(230, 0, 115)},
    {"region", nvgRGB(56, 189, 153)}};

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
        for (unsigned int i = samples - 1; i > 0; i--) {
            const float spx = rect.pos.x + rect.size.x * ((float)i / ((float)samples - 1));
            const float spy = rect_center.y - (rect.size.y / 2) * ((float)display_buf[1][i]);
            nvgLineTo(args.vg, spx, spy);
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
        const auto color_mint = nvgRGB(56, 189, 153);
        const auto color_charcoal = nvgRGB(64, 64, 64);
        const auto color_white = nvgRGBA(255, 255, 255, 90);

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

                draw_rect(args, color_charcoal, local_box, true);
                draw_rect(args, color_mint, title_rect);
                draw_rect(args, color_mint, info_rect);

                // Zero Line
                draw_h_line(args, color_mint, waveform_rect, 0.5);

                if (waveform) {
                    // Text
                    draw_text(args, color_mint, title_rect, waveform->get_text_title());
                    draw_text(args, color_mint, info_rect, waveform->get_text_info());

                    if (waveform->has_data()) {
                        // Waveform
                        draw_waveform(args, color_white, waveform_rect);

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

                draw_rect(args, color_mint, waveform_rect);
            }
        }
        Widget::drawLayer(args, layer);
    }
};

struct RefluxWidget: ModuleWidget {
    struct ParamGroup {
        int first_id;
        int count;
    };

    using ParamWidgetCreator = ParamWidget* (*)(WidgetType, Vec, Module*, int);
    struct ParamWidgetGrirdArgs {
        ParamGroup group;
        Vec pos;
        int columns = std::numeric_limits<int>::max();;
        Vec spacing = Vec(35);
        WidgetType default_type = WTRegularButton;
        std::unordered_map<int, WidgetType> widget_types = {};
        ParamWidgetCreator create_widget = &create_centered_widget<Reflux>;
    };


    /// @brief Generate a grid of param widgets
    /// @param row_pos position of the first widget in the row
    /// @param params describes which succesive param group to link to
    /// @param opt_columns number of grid columns
    /// @param opt_spacing optionally define the spacing between widgets
    /// @param opt_default_type the default type of widget to create
    /// @param widget_types define the specific widget type for any
    /// @param opt_widget_creator which widget creator function to use
    void add_param_widget_grid(
        Vec row_pos,
        ParamGroup params,
        Optional<int> opt_columns = {},
        Optional<Vec> opt_spacing = {},
        Optional<WidgetType> opt_default_type = {},
        const std::unordered_map<int, WidgetType>& widget_types = {},
        Optional<ParamWidgetCreator> opt_widget_create = {}
    ) {
        const int columns = opt_columns.some() ? opt_columns.value() : INFINITY;
        const Vec spacing = opt_spacing.some() ? opt_spacing.value() : Vec(35, 35);
        const WidgetType default_type = opt_default_type.some() ? opt_default_type.value() : WTRegularButton;
        const ParamWidgetCreator create_widget =
            opt_widget_create.some() ? opt_widget_create.value() : &create_centered_widget<Reflux>;

        for (int idx = 0; idx < params.count; idx++) {
            const int param_id = params.first_id + idx;
            const Vec pos = row_pos + Vec((float)(idx % columns) * spacing.x, (float)(idx / columns) * spacing.y);
            const WidgetType wtype = ((bool)widget_types.count(idx)) ? widget_types.at(idx) : default_type;
            addParam(create_widget(wtype, pos, module, param_id));
        }
    }

    template<class WavefromType>
    void
    add_waveform_group(Vec group_pos, ParamGroup params, const std::unordered_map<int, WidgetType>& widget_types = {}) {
        const int knob_offset = 15;
        add_param_widget_grid(group_pos + Vec(knob_offset), params, {}, Some(Vec(30.0F)), Some(WTOmniKnob), widget_types);

        auto* display = new WaveformDisplayWidget<WavefromType>();
        display->box.pos = group_pos + Vec(0, 40);
        display->box.size = Vec(150, 38);
        display->module = dynamic_cast<Reflux*>(module);
        addChild(display);
    }

    RefluxWidget(Reflux* module) {
        const auto slice_group_pos = Vec(15, 95);
        const auto slice_group_knobs = 5;

        const auto clip_group_pos = Vec(15, 190);
        const std::unordered_map<int, WidgetType> clip_btn_types = {{1, WTSaveButton}, {2, WTLoadButton}};

        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Reflux.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        add_waveform_group<AudioSlice>(slice_group_pos, {Reflux::PARAM_SELECTED_SLICE, slice_group_knobs});
        add_param_widget_grid(Vec(185, 95), {Reflux::PARAM_SLICE_PLAY, 3});
        add_param_widget_grid(Vec(185, 135), {Reflux::PARAM_SLICE_SHIFT_L, 3});

        add_waveform_group<AudioClip>(clip_group_pos, {Reflux::PARAM_SELECTED_SAMPLE, 5}, {{0, WTSnapKnob}});
        add_param_widget_grid(Vec(185, 205), {Reflux::PARAM_SAMPLE_RECORD, 3}, {}, {}, {}, clip_btn_types);
        add_param_widget_grid(Vec(185, 245), {Reflux::PARAM_SAMPLE_PLAY, 3});

        addChild(createLightCentered<RubberSmallButtonLed<RedLight>>(Vec(185, 205), module, Reflux::LIGHT_SAMPLE_RECORD)
        );
        addChild(createLightCentered<RubberSmallButtonLed<BlueLight>>(Vec(185, 245), module, Reflux::LIGHT_SAMPLE_PLAY)
        );

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
