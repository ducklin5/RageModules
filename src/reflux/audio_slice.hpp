#pragma once
#include "audio_base.hpp"
#include "audio_clip.hpp"

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
    std::shared_ptr<AudioConsumer> consumer;
    
    Eventful<double> start;
    Eventful<double> stop;
    Eventful<double> attack;
    Eventful<double> release;

    double read;
    bool needs_ui_update = true;
    IdxType idx = 0;
    IdxType total = 0;
    bool is_playing = false;

    DisplayBufferBuilder* display_buffer_builder;

    static std::shared_ptr<AudioSlice> create(AudioClip& clip, DisplayBufferBuilder* dbb) {
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
        display_buffer_builder(dbb) {
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
        return fmt::format("clip{}-{}-[{}]", (int)m_clip.id, consumer->name, clip_slice_index);
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
                if (display_buffer_builder)
                    display_buffer_builder->build({get_sample_lambda, &m_display_buf, (IdxType)start, (IdxType)stop});
                needs_ui_update = false;
            }
        }
    }

    json_t* make_json_obj() {
        json_t* root = json_object();

        json_object_set(root, "idx", json_real(idx));
        json_object_set(root, "total", json_real(total));
        json_object_set(root, "clip_idx", json_real(m_clip.id));
        json_object_set(root, "start", json_real(start.value));
        json_object_set(root, "stop", json_real(stop.value));
        json_object_set(root, "attack", json_real(attack.value));
        json_object_set(root, "release", json_real(release.value));
        json_object_set(root, "read", json_real(read));
        json_object_set(root, "is_playing", json_boolean(is_playing));

        return root;
    }

    void load_json(json_t* root) {
        idx = json_real_value(json_object_get(root, "idx"));
        total = json_real_value(json_object_get(root, "total"));
        start.value = json_real_value(json_object_get(root, "start"));
        stop.value = json_real_value(json_object_get(root, "stop"));
        attack.value = json_real_value(json_object_get(root, "attack"));
        release.value = json_real_value(json_object_get(root, "release"));
        read = json_real_value(json_object_get(root, "read"));
        is_playing = json_boolean_value(json_object_get(root, "is_playing"));
        needs_ui_update = true;
    }

    ~AudioSlice() {
        m_clip.remove_consumer(consumer);
    }
};