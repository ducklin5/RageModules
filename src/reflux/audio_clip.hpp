#pragma once
#include "audio_base.hpp"
#include "dep/babycat/babycat.h"

// NOLINTNEXTLINE (google-build-using-namespace)
using namespace rage;

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

using namespace std::placeholders;
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
    bool can_clear = false;

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
    PlaybackProfile playback_profile;

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
            format: SF_FORMAT_WAV | SF_FORMAT_PCM_32,
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

    void clear() {
        this->raw_data.clear();
        this->num_channels = 0;
        this->num_frames = 0;
        this->has_loaded = false;
        this->has_recorded = false;
        this->is_playing = false;
        this->is_recording = false;
        this->can_clear = false;
        this->file_path = "";
        this->file_display = "Cleared";
        this->file_info_display = "";
        this->start_head = 0;
        this->stop_head = 0;
        this->read_head = 0;
        this->notify_consumers();
    }

    void start_playing() {
        if (has_data()) {
            this->read_head = playback_profile.speed > 0 ? start_head : stop_head;
            this->is_playing = true;
            this->can_clear = false;
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
    }

    auto read_frame() -> std::vector<double> {
        if (!is_playing)
            return std::vector<double>(num_channels, 0.0);

        using namespace std::placeholders;
        auto result = playback_profile.read_frame(
            std::bind(get_sample, this, _1, _2),
            num_channels,
            frame_rate_hz,
            read_head,
            start_head,
            stop_head
        );

        is_playing = !result.reached_end;
        read_head.value = result.next;
        return result.data;
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
        return fmt::format("{}. {}", id + 1, file_display);
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

    void notify_consumers() {
        for (auto& consumer : consumers) {
            consumer->notify();
        }
    }

    json_t* make_json_obj() {
        json_t* root = json_object();

        json_object_set(root, "has_recorded", json_boolean(has_recorded));
        json_object_set(root, "has_loaded", json_boolean(has_loaded));
        json_object_set(root, "file_path", json_string(file_path.c_str()));
        json_object_set(root, "is_playing", json_boolean(is_playing));
        json_object_set(root, "is_recording", json_boolean(is_recording));
        json_object_set(root, "read_head", json_real(read_head));
        json_object_set(root, "write_head", json_real(write_head));
        json_object_set(root, "start_head", json_real(start_head));
        json_object_set(root, "stop_head", json_real(stop_head));
        json_object_set(root, "playback_profile", playback_profile.make_json_obj());

        return root;
    }

    void load_json(json_t* root) {
        has_recorded = json_boolean_value(json_object_get(root, "has_recorded"));
        has_loaded = json_boolean_value(json_object_get(root, "has_loaded"));
        file_path = json_string_value(json_object_get(root, "file_path"));
        is_playing = json_boolean_value(json_object_get(root, "is_playing"));
        is_recording = json_boolean_value(json_object_get(root, "is_recording"));
        read_head = json_real_value(json_object_get(root, "read_head"));
        write_head = json_real_value(json_object_get(root, "write_head"));
        start_head = json_real_value(json_object_get(root, "start_head"));
        stop_head = json_real_value(json_object_get(root, "stop_head"));
        playback_profile.load_json(json_object_get(root, "playback_profile"));
    }
};