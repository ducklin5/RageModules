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

#include "../dep/babycat/babycat.h"
#include "./shared/components.hpp"
#include "./shared/make_builder.hpp"
#include "./shared/math.hpp"
#include "./shared/nvg_helpers.hpp"
#include "./shared/utils.hpp"
#include "plugin.hpp"

enum { AUDIO_CLIP_DISPLAY_RES = 128 };
enum { AUDIO_CLIP_DISPLAY_CHANNELS = 2 };

// NOLINTNEXTLINE (google-build-using-namespace)
using namespace rage;
using IdxType = uintptr_t;
using DisplayBufferType = std::array<std::vector<double>, 2>;

struct DisplayBufferBuilder {
    struct BuildArgs {
        std::function<double(IdxType, IdxType)> get_sample = nullptr;
        DisplayBufferType* dst = nullptr;
        IdxType start = 0;
        IdxType stop = 0;

        BuildArgs() = default;

        BuildArgs(std::function<double(IdxType, IdxType)> get_sample, DisplayBufferType* dst, IdxType start, IdxType stop) :
            get_sample(get_sample),
            dst(dst),
            start(start),
            stop(stop) {}
    };

  private:
    std::thread workerThread;
    std::mutex workerMutex;
    std::condition_variable workerCv;
    bool running;
    std::queue<DisplayBufferType*> tasks;
    std::unordered_map<DisplayBufferType*, BuildArgs> task_args;

  public:
    DisplayBufferBuilder() {
        workerThread = std::thread([this] { run(); });
    }

    ~DisplayBufferBuilder() {
        running = false;
        workerCv.notify_one();
        workerThread.join();
    }

    void build(BuildArgs args) {
        std::lock_guard<std::mutex> lock(workerMutex);
        tasks.push(args.dst);
        task_args[args.dst] = args;
        workerCv.notify_one();
    }

  private:
    void run() {
        running = true;
        std::unique_lock<std::mutex> lock(workerMutex);
        while (running) {
            if (tasks.empty()) {
                workerCv.wait(lock);
                continue;
            }
            DisplayBufferType* dst = tasks.front();
            tasks.pop();
            if (task_args.count(dst)) {
                BuildArgs args = task_args[dst];
                task_args.erase(dst);
                lock.unlock();
                build_(args);
                lock.lock();
            }
        }
    }

    void build_(BuildArgs args) {
        DisplayBufferType& buffer = *args.dst;
        IdxType chunk_size = (args.stop - args.start) / AUDIO_CLIP_DISPLAY_RES;
        chunk_size = chunk_size ? chunk_size : 1;

        for (int cidx = 0; cidx < AUDIO_CLIP_DISPLAY_CHANNELS; cidx++) {
            buffer[cidx].resize(AUDIO_CLIP_DISPLAY_RES, 0.0);
            IdxType curr = args.start;
            double max = 0;
            for (IdxType i = 0; i < AUDIO_CLIP_DISPLAY_RES; i++) {
                double accum = 0.0;
                for (IdxType j = 0; j < chunk_size; j++) {
                    accum += std::abs(args.get_sample(cidx, curr++));
                }
                buffer[cidx][i] = accum / ((double)chunk_size);
                max = std::max(buffer[cidx][i], max);
            }
        }
    }
};

struct Marker {
    double pos;
    std::string tag;

    bool operator<(const Marker& other) const {
        return pos < other.pos;
    }
};

class AudioConsumer {
  public:
    using NotificationListener = std::function<void(void)>;
    std::string name;
    Marker marker;
    NotificationListener on_notify;

    AudioConsumer(std::string name, float pos, std::string tag, NotificationListener on_notify) :
        name(name),
        marker({pos, tag}),
        on_notify(on_notify) {}

    void notify() {
        on_notify();
    }

    bool operator<(const AudioConsumer& other) const {
        return marker < other.marker;
    }
};

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
    bool events_enabled = true;

    Eventful<double>::Callback on_head_event = [this](EventfulBase::Event, double) {
        // prevent recursive events when we fix heads
        this->fix_heads();
    };

    Eventful<double> read_head {0, on_head_event};
    Eventful<double> write_head {1.0, on_head_event};
    Eventful<double> start_head {0, on_head_event};
    Eventful<double> stop_head {0, on_head_event};

    using StoredConsumer = std::shared_ptr<AudioConsumer>;
    std::vector<StoredConsumer> consumers;
    rack::dsp::Timer write_timer;
    DisplayBufferBuilder* display_buffer_builder = nullptr;

    AudioClip() : file_path("Unsaved.***") {
        this->update_display_data();
    }

    void set_id(int id) {
        this->id = id;
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
        this->file_info_display = fmt::format("{}Hz-{}Ch", this->frame_rate_hz, this->num_channels);

        free(path_dup);
    }

    void build_display_buf_self() {
        using namespace std::placeholders;
        const auto get_sample_lambda = std::bind(&AudioClip::get_sample, this, _1, _2);
        if (this->display_buffer_builder) {
            display_buffer_builder->build({get_sample_lambda, &display_buf, 0, num_frames});
        }
    }

    auto load_file(std::string& path) -> bool {
        // Try load waveform using babycat
        if (!this->load_babycat_path(path)) {
            return false;
        }

        this->stop_head = this->num_frames;
        this->file_path = path;
        this->has_loaded = true;
        this->has_recorded = false;
        this->update_display_data();
        this->build_display_buf_self();

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

        // Write the audio samples to the output file.
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
            this->has_recorded = false;
            this->has_loaded = true;
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
        this->num_frames = std::max(num_frames, frame_idx + 1);

        auto& channel = raw_data[channel_idx];

        if (frame_idx >= channel.size()) {
            double segment_size = (double)frame_rate_hz * 60 * 3;
            double capacity_segments = ceil((double)frame_idx / segment_size);
            channel.reserve(capacity_segments * segment_size);
            channel.resize(frame_idx + 1);
        }

        channel[frame_idx] = value;
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

        read_head.value += 1.0;

        return data;
    }

    struct WriteArgs {
        bool overwrite {true};
        float delta {0.0};
        IdxType channel_count {2};
        WriteArgs() {}  // NOLINT
    };

    void write_frame(const double* channels, WriteArgs args = {}) {
        if (!is_recording) {
            return;
        }

        frame_rate_hz = 1.0 / args.delta;

        for (IdxType cidx = 0; cidx < args.channel_count; cidx++) {
            set_sample(cidx, write_head.value, channels[cidx], args.overwrite);
        }

        this->has_recorded = true;

        if (write_head.value == stop_head.value) {
            is_recording = false;
        } else if (write_head.value > stop_head.value) {
            stop_head.value = write_head;
        }

        write_head.value += 1;

        if (write_timer.process(args.delta) > rage::UI_update_time) {
            write_timer.reset();
            this->update_display_data();
            this->build_display_buf_self();
        }
    }

    void fix_heads() {
        stop_head.silent_set(std::max<double>(start_head, stop_head));
        read_head.silent_set(std::max<double>(start_head, read_head));
        read_head.silent_set(std::min<double>(stop_head, read_head));
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
        return file_display;
    }

    auto get_text_info() const -> std::string {
        return file_info_display;
    }

    StoredConsumer create_consumer(float pos, std::string tag, AudioConsumer::NotificationListener on_notify) {
        std::string name = "";
        do {
            name = random_string(4);
        } while (find_consumer_by_name(name) >= 0);

        auto obj = std::make_shared<AudioConsumer>(name, pos, tag, on_notify);
        consumers.push_back(obj);
        sort_consumers();
        return obj;
    }

    void sort_consumers() {
        std::sort(consumers.begin(), consumers.end(), [](StoredConsumer consumer1, StoredConsumer consumer2) {
            return *consumer1 < *consumer2;
        });
    }

    auto find_consumer(std::function<bool(const StoredConsumer)> predicate) const -> int {
        auto iter = std::find_if(consumers.begin(), consumers.end(), std::move(predicate));
        if (iter == consumers.end()) {
            return -1;
        }
        return std::distance(consumers.begin(), iter);
    }

    auto find_consumer_by_name(std::string name) const -> int {
        return find_consumer([&](const StoredConsumer other) { return other->name == name; });
    }

    void remove_consumer(const StoredConsumer& consumer) {
        const int idx = find_consumer_by_name(consumer->name);
        if (idx >= 0) {
            consumers.erase(consumers.begin() + idx);
        }
    }

    json_t* make_json_obj() {
        return json_object();
    }

    void load_json(json_t* obj) {}
};

struct AudioSlice {
  private:
    AudioClip& m_clip;
    DisplayBufferType m_display_buf;
    rack::dsp::Timer m_update_timer;
    Eventful<double>::Callback m_handle_range_changed = [this](EventfulBase::Event, double) { this->update_data(); };

    AudioConsumer::NotificationListener on_notification = [this]() { this->update_data(); };

    void update_data() {
        stop.silent_set(clamp(stop, start, m_clip.num_frames));
        start.silent_set(clamp((double)start, 0.0L, m_clip.num_frames));
        attack.silent_set(clamp((double)attack, start, stop));
        release.silent_set(clamp((double)release, start, stop));
        read = clamp((double)read, start, stop);
        consumer->marker.pos = (float)start / (m_clip.num_frames + 1);
        consumer->marker.tag = "start";
        needs_ui_update = true;
    }

  public:
    Eventful<double> start;
    Eventful<double> stop;
    Eventful<double> attack;
    Eventful<double> release;

    double read;
    bool needs_ui_update = true;
    std::shared_ptr<AudioConsumer> consumer;
    IdxType idx = 0;
    IdxType total = 0;
    bool is_playing = false;
     
    DisplayBufferBuilder* display_buffer_builder;

    static std::shared_ptr<AudioSlice> create(AudioClip& clip, DisplayBufferBuilder* dbb = nullptr) {
        return std::make_shared<AudioSlice>(clip, dbb);
    }

    AudioSlice(AudioClip& clip, IdxType start, IdxType stop, DisplayBufferBuilder* dbb = nullptr) :
        m_clip(clip),
        consumer(m_clip.create_consumer(0, "", on_notification)),
        start(Eventful<double>(start, m_handle_range_changed)),
        stop(Eventful<double>(stop, m_handle_range_changed)),
        attack(Eventful<double>(start, m_handle_range_changed)),
        release(Eventful<double>(stop, m_handle_range_changed)),
        read(start),
        display_buffer_builder(dbb)
    {
        update_data();
    }

    AudioSlice(AudioClip& clip, DisplayBufferBuilder* dbb) : AudioSlice(clip, clip.start_head, clip.stop_head, dbb) {}

    const AudioClip& clip() {
        return m_clip;
    }

    bool has_data() const {
        return true;
    }

    void start_playing() {
        this->read = start;
        this->is_playing = true;
    }

    void toggle_playing() {
        is_playing = !is_playing;
    }

    auto get_sample(IdxType channel_idx, IdxType frame_idx) -> double {
        const double attack_mult =
            (attack > start && frame_idx < attack) ? (frame_idx - start) / (attack - start) : 1.0;

        const double release_mult =
            (release < stop && frame_idx > release) ? 1.0 - ((frame_idx - release) / (stop - release)) : 1.0;

        return attack_mult * release_mult * m_clip.get_sample(channel_idx, frame_idx);
    }

    auto read_frame() -> std::vector<double> {
        auto num_channels = m_clip.num_channels;
        auto data = std::vector<double>(num_channels);

        if (!is_playing || read >= stop) {
            read = start;
            is_playing = false;
            return data;
        }
        for (IdxType channel_idx = 0; channel_idx < num_channels; channel_idx++) {
            data[channel_idx] = get_sample(channel_idx, read);
        }
        read += 1.0;

        return data;
    }

    auto get_text_title() const -> std::string {
        int clip_slice_index = m_clip.find_consumer_by_name(consumer->name);
        return fmt::format("clip{}-{}-[{}]", (int) m_clip.id, consumer->name, clip_slice_index);
    }

    auto get_text_info() const -> std::string {
        return fmt::format("{}/{}", idx + 1, total);
    }

    std::vector<Marker> get_markers() const {
        auto read_ratio = float(read - start) / (stop - start);
        auto attack_ratio = float(attack - start) / (stop - start);
        auto release_ratio = float(release - start) / (stop - start);
        return {
            Marker {read_ratio, "read"},
            Marker {attack_ratio, "attack"},
            Marker {release_ratio, "release"},
        };
    }

    std::vector<Region> get_regions() const {
        return {};
    }

    const DisplayBufferType& get_display_buf() const {
        return m_display_buf;
    }

    void update_timer(float delta) {
        using namespace std::placeholders;
        const auto get_sample_lambda = std::bind(&AudioSlice::get_sample, this, _1, _2);

        if (m_update_timer.process(delta) >= rage::UI_update_time) {
            m_update_timer.reset();
            if (needs_ui_update) {
                m_clip.sort_consumers();
                if(display_buffer_builder)
                    display_buffer_builder->build({get_sample_lambda, &m_display_buf, (IdxType) start, (IdxType) stop});
                needs_ui_update = false;
            }
        }
    }

    json_t* make_json_obj() {
        return json_object();
    }

    void load_json(json_t* obj) {}

    ~AudioSlice() {
        m_clip.remove_consumer(consumer);
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
        INPUT_SELECT_CV,
        INPUT_SELECT_TRIGGER,

        INPUT_TRIGGER1,
        INPUT_TRIGGER2,

        INPUT_AUDIOL,
        INPUT_AUDIOR,

        INPUT_CV1,
        INPUT_CV2,

        NUM_INPUTS
    };

    enum OutputIds { OUTPUT_AUDIOL, OUTPUT_AUDIOR, OUTPUT_BOS, OUTPUT_EOS, NUM_OUTPUTS };

    enum LightIds { LIGHT_SLICE_PLAY, LIGHT_SAMPLE_RECORD, LIGHT_SAMPLE_PLAY, NUM_LIGHTS };

    static const int NUM_CLIPS = 12;
    Eventful<double> selected_sample {0};
    Eventful<double> selected_slice {0};
    DisplayBufferBuilder slice_dbb;
    DisplayBufferBuilder clip_dbb;
    std::array<AudioClip, NUM_CLIPS> clips;
    std::vector<std::shared_ptr<AudioSlice>> slices {};
    std::string directory_;

    rack::dsp::BooleanTrigger btntrig_clip_record, btntrig_clip_play, btntrig_clip_pause;
    rack::dsp::BooleanTrigger btntrig_slice_play, btntrig_slice_pause, btntrig_slice_delete;
    rack::dsp::BooleanTrigger btntrig_slice_shiftl, btntrig_slice_shiftr, btntrig_slice_learn;

    rack::dsp::BooleanTrigger intrig_trigger;

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
        return clips.at((IdxType)selected_sample);
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
        // for (IdxType i = 0; i < NUM_CLIPS; i++) {
        if (clips.at(selected_sample).is_recording) {
            //if (i == selected_sample) {
            double data[2];
            data[0] = getInput(INPUT_AUDIOL).getVoltage();
            data[1] = getInput(INPUT_AUDIOR).getVoltage();
            AudioClip::WriteArgs wargs;
            wargs.delta = args.sampleTime;
            current_clip().write_frame(data, wargs);
            //} else {
            //    clips.at(i).is_recording = false;
            //}
        }
        //}
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
                audio_out_l += frame[0];
                audio_out_r += frame[1];
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
        // update the select sample
        selected_sample = (int)getParam(PARAM_SELECTED_CLIP).getValue();

        // read audio input to current sample
        process_read_input_audio(args);

        // listen for start recording button event
        if (btntrig_clip_record.process(params[PARAM_CLIP_RECORD].getValue() > 0.0)) {
            current_clip().toggle_recording();
        }

        // listen for sample play button event
        if (btntrig_clip_play.process(params[PARAM_CLIP_PLAY].getValue() > 0.0)) {
            current_clip().start_playing();
        }

        // listen for sample pause button event
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

        // update lights
        if (light_timer.process(args.sampleTime) > rage::UI_update_time) {
            light_timer.reset();
            if (current_slice()) {
                lights[LIGHT_SLICE_PLAY].setSmoothBrightness(current_slice()->is_playing ? .5f : 0.0f, UI_update_time);
            }
            lights[LIGHT_SAMPLE_PLAY].setSmoothBrightness(current_clip().is_playing ? .5f : 0.0f, UI_update_time);
            lights[LIGHT_SAMPLE_RECORD].setSmoothBrightness(current_clip().is_recording ? .5f : 0.0f, UI_update_time);
        }

        // process_clips
        process_slices(args);

        // compute_output
        compute_output(args);
    }
    /*
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
            const json_int_t clip_idx = json_integer_value(json_object_get(json_obj, "clip_idx"));
            auto slice = AudioSlice::create(clips.at(clip_idx));
            slice->load_json(json_obj);
            slices.push_back(slice);
        }
    }
    */

    /*
    void onAdd(const AddEvent& event) override {
        for (IdxType i = 0; i < clips.size(); i++) {
            Optional<std::string> path;
            if (clips.at(i).has_recorded) {
                std::string filename = fmt::format("clip_{}.wav", i);
                path.set_value(system::join(createPatchStorageDirectory(), filename));
            } else if (clips.at(i).has_loaded) {
                path.set_value(clips.at(i).file_path);
            }
            if (path.some() && system::isFile(path.value())) {
                auto pathv = path.value();
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
            if (path.some() && system::isFile(path.value())) {
                auto pathv = path.value();
                clips.at(i).save_file(pathv);
            }
        }
    }
    */

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
    template<class T>
    using RSBLed = RubberSmallButtonLed<T>;

    explicit RefluxWidget(Reflux* module) {
        const auto slice_group_pos = Vec(30, 110);
        const auto slice_group_knobs = 5;
        const auto clip_group_pos = Vec(30, 205);

        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Reflux.svg")));

        // Screws
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Audio Slice
        add_waveform_group<AudioSlice>(slice_group_pos, {Reflux::PARAM_SELECTED_SLICE, slice_group_knobs});
        add_widget_grid(WGArgs().pos(Vec(185, 110)).group({Reflux::PARAM_SLICE_SHIFTL, 6}).columns(3));

        // Audio Clip
        const WidgetTypeMap btns = {{1, WTSaveButton}, {2, WTLoadButton}};
        add_waveform_group<AudioClip>(clip_group_pos, {Reflux::PARAM_SELECTED_CLIP, 5}, {{0, WTSnapKnob}});
        add_widget_grid(WGArgs().pos(Vec(185, 205)).group({Reflux::PARAM_CLIP_RECORD, 6}).columns(3).custom_types(btns)
        );

        addChild(createLightCentered<RSBLed<BlueLight>>(Vec(185, 150), module, Reflux::LIGHT_SLICE_PLAY));
        addChild(createLightCentered<RSBLed<RedLight>>(Vec(185, 205), module, Reflux::LIGHT_SAMPLE_RECORD));
        addChild(createLightCentered<RSBLed<BlueLight>>(Vec(185, 245), module, Reflux::LIGHT_SAMPLE_PLAY));

        add_widget_grid(WGArgs()
                            .pos(mm2px(Vec(10, 101)))
                            .group({Reflux::INPUT_SELECT_CV, 8})
                            .columns(4)
                            .default_type(WidgetType::WTInputPort)
                            .spacing(mm2px(Vec(10, 11.5))));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(103, 10)), module, Reflux::OUTPUT_AUDIOL));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(113, 10)), module, Reflux::OUTPUT_AUDIOR));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(108, 18)), module, Reflux::OUTPUT_EOS));
    }
};

Model* modelReflux = createModel<Reflux, RefluxWidget>("Reflux");
