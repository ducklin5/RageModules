#pragma once

#include <dirent.h>
#include <fmt/core.h>
#include <libgen.h>
#include <math.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plugin.hpp"
#include "src/external/biquad.hpp"
#include "src/shared/components.hpp"
#include "src/shared/make_builder.hpp"
#include "src/shared/math.hpp"
#include "src/shared/nvg_helpers.hpp"
#include "src/shared/utils.hpp"

// NOLINTNEXTLINE (google-build-using-namespace)
using namespace rage;

enum { AUDIO_CLIP_DISPLAY_RES = 128 };
enum { AUDIO_CLIP_DISPLAY_CHANNELS = 2 };
using IdxType = uintptr_t;

using DisplayBufferType = std::array<std::vector<double>, 2>;

struct DisplayBufferBuilder {
    struct BuildArgs {
        std::function<double(IdxType, IdxType)> get_sample = nullptr;
        DisplayBufferType* dst = nullptr;
        IdxType start = 0;
        IdxType stop = 0;

        BuildArgs() = default;

        BuildArgs(
            std::function<double(IdxType, IdxType)> get_sample,
            DisplayBufferType* dst,
            IdxType start,
            IdxType stop
        ) :
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

struct IIR4BandPass {
    Biquad filter1;
    Biquad filter2;

    IIR4BandPass(double sample_rate, double freq, double Q) :
        filter1(BQType::bandpass, freq / sample_rate, Q, 0.0),
        filter2(BQType::bandpass, freq / sample_rate, Q, 0.0) {}

    double config(double sample_rate, double freq, double Q) {
        filter1.setFc(freq / sample_rate);
        filter1.setQ(Q);
        filter2.setFc(freq / sample_rate);
        filter2.setQ(Q);
    }

    double process(double in) {
        return filter2.process(filter1.process(in));
    }
};

struct MultiChannelBuffer {
    // implements a circular buffer
  private:
    std::vector<std::vector<double>> data;
    IdxType num_channels = 0;
    IdxType m_size = 0;

    IdxType oldest_idx = 0;

  public:
    MultiChannelBuffer() = default;

    MultiChannelBuffer(IdxType num_channels, IdxType size) : num_channels(num_channels), m_size(size), oldest_idx(0) {
        data = std::vector<std::vector<double>>(size, std::vector<double>(num_channels, 0.0));
    }

    void reset() {
        for (int i = 0; i < m_size; i++) {
            for (int chan = 0; chan < num_channels; chan++) {
                data[i][chan] = 0.0;
            }
        }
        oldest_idx = 0;
    }
    
    void mult(double x) {
        for (int i = 0; i < m_size; i++) {
            for (int chan = 0; chan < num_channels; chan++) {
                data[i][chan] *= x;
            }
        }
    }

    std::vector<double>* get(IdxType idx) {
        if (idx < 0)
            return nullptr;
        if (idx >= m_size)
            return nullptr;
        auto data_idx = (oldest_idx + idx) % m_size;
        return &data[data_idx];
    }

    std::vector<double> get_smooth(double idx) {
        auto index_up = ceil(idx);
        auto index_down = floor(idx);
        auto data_up = get( index_up);
        auto data_down = get(index_down);
        if (data_up == nullptr || data_down == nullptr)
            return std::vector<double>(num_channels, 0.0);
        auto frac = idx - index_down;
        auto result = std::vector<double>(num_channels, 0.0);
        for (int i = 0; i < num_channels; i++) {
            result[i] = data_up->at(i) * frac + data_down->at(i) * (1.0 - frac);
        }
        return result;
    }

    const std::vector<double> get_const(IdxType idx) const {
        auto data_idx = (oldest_idx + idx) % m_size;
        return data[data_idx];
    }

    std::vector<double>* oldest() {
        return this->get(0);
    }

    std::vector<double>* newest() {
        return this->get(-1 + m_size);
    }

    void push(std::vector<double> frame) {
        auto fsize = frame.size();
        if (fsize > num_channels)
            set_channels(frame.size());
        else if(fsize < num_channels)
            frame.resize(num_channels, 0.0);

        data[oldest_idx] = frame;
        oldest_idx = (oldest_idx + 1) % m_size;
    }

    IdxType size() const {
        return m_size;
    }

    void set_size(IdxType size) {
        data.resize(size, std::vector<double>(num_channels, 0.0));
        if (oldest_idx >= size) {
            oldest_idx = 0;
        }
        this->m_size = size;
    }

    IdxType channels() const {
        return num_channels;
    }

    void set_channels(IdxType num_channels) {
        if (num_channels == this->num_channels)
            return;
        this->num_channels = num_channels;
        for (int i = 0; i < m_size; i++) {
            data[i].resize(num_channels, 0.0);
        }
    }

    friend auto operator<<(std::ostream& os, const MultiChannelBuffer& m) -> std::ostream& {
        os << "[\n";
        for (int i = 0; i < m.size(); i++) {
            if (i == m.oldest_idx)
                os << "->[";
            else
                os << "--[";
            std::vector<double> frame = m.data[i];
            for (int j = 0; j < m.num_channels; j++) {
                os << frame[j];
                if (j + 1 == m.num_channels)
                    break;
                os << ",";
            }
            if (i + 1 == m.size())
                os << "]\n";
            else
                os << "],\n";
        }
        os << "]\n";
        return os;
    }
};

double window_fn(double x) {
    return 0.5 * (1.0 + std::cos(2.0 * M_PI * x));
}

struct RealtimeMultiChannelTuner {
    MultiChannelBuffer filtered_buffer;
    std::vector<IIR4BandPass> filters;

    double sample_rate = 1.0;
    double period_ratio = 1.0;
    double freq = 1000.0;
    double Q = 1.0;

    IdxType optimal_out_buffer_size() {
        return (IdxType)(1);  // 10000Hz is highest supported frequency
    }

    IdxType optimal_in_buffer_size() {
        return (IdxType)(sample_rate / 100);  // 100Hz is lowest supported frequency
    }

    void set_channels(double num_channels) {
        filtered_buffer.set_channels(num_channels);
        config_filters(freq, Q);
    }

    void set_sample_rate(double sample_rate) {
        this->sample_rate = sample_rate;
        filtered_buffer.set_size(optimal_in_buffer_size());
    }

    void set_period_ratio(double period_ratio) {
        this->period_ratio = period_ratio;
    }

    void config_filters(double freq, double Q) {
        this->freq = freq;
        this->Q = Q;

        if (filters.size() != filtered_buffer.channels())
            filters.resize(filtered_buffer.channels(), IIR4BandPass(sample_rate, freq, Q));

        for (IdxType i = 0; i < filtered_buffer.channels(); i++) {
            filters[i].config(sample_rate, freq, Q);
        }
    }

    auto bandpass(std::vector<double> frame) -> std::vector<double> {
        auto ret = std::vector<double>(frame.size());
        for (IdxType i = 0; i < frame.size(); i++) {
            ret[i] = filters[i].process(frame[i]);
        }
        return ret;
    }

    double outptr = 0.0;

    auto process(std::vector<double> frame) -> std::vector<double> {
        if (frame.size() != filtered_buffer.channels())
            set_channels(frame.size());

        auto filtered_frame = bandpass(frame);
        
        // apply gain
        for (IdxType i = 0; i < filtered_frame.size(); i++) {
            filtered_frame[i] *= Q;
        } 

        filtered_buffer.push(filtered_frame);

        auto residual_frame = std::vector<double>(filtered_frame.size());
        for (IdxType i = 0; i < filtered_frame.size(); i++) {
            residual_frame[i] = frame[i] - Q * filtered_frame[i];
        }

        // pitch detection
        IdxType period_length = filtered_buffer.size();
        bool zero_crossing_detected = false;
        IdxType recent_zero_corssing;
        std::vector<double>* newer_x = nullptr;
        std::vector<double>* x = nullptr;

        for (int i = filtered_frame.size() - 1; i >= 0; i--) {
            newer_x = x;
            x = filtered_buffer.get(i);
            if (newer_x && (*newer_x)[0] > 0 && (*x)[0] < 0) {  // zero crossing detected
                if (zero_crossing_detected) {
                    period_length = recent_zero_corssing - i;
                    break;
                }
                zero_crossing_detected = true;
                recent_zero_corssing = i;
            }
        }

        // adjust outptr
        outptr += -1 + period_ratio;

        // wrap outptr
        if (outptr >= filtered_buffer.size()) outptr -= period_length;
        if (outptr < 0) outptr += period_length;

        return filtered_buffer.get_smooth(outptr);
    }
};

using SampleGetter = std::function<double(double, double)>;

struct EventfulValueRange {
    Eventful<double>* value;
    double min_value;
    double max_value;
    std::string str_value;
};

std::string format_frequency(double amount) {
    if (amount < 1000)
        return fmt::format("{:.0f}", amount);
    else if (amount < 10000)
        return fmt::format("{:.2f}", amount / 1000) + 'k';
    return fmt::format("{:.1f}", amount / 1000) + 'k';
}

struct PlaybackProfile {
    enum class PlaybackMode { OneShot = 0, Loop, PingPong, NUM_MODES };
    enum class TunerKnobMode { Resonance = 0, Frequency, Xhift, NUM_MODES };
    enum class PVKnobMode { Pan = 0, Volume, NUM_MODES };

    PlaybackMode mode = PlaybackMode::OneShot;
    TunerKnobMode tuner_knob_mode = TunerKnobMode::Resonance;
    PVKnobMode pv_knob_mode = PVKnobMode::Volume;

    Eventful<double>::Callback on_freq_q_changed = [this](EventfulBase::Event, double) { this->reconfig_filters(); };

    Eventful<double> pan = 0.l;
    Eventful<double> volume = 1.l;
    Eventful<double> speed = 1.l;
    Eventful<double> xhift {1.l, [this](EventfulBase::Event, double) { this->tuner.set_period_ratio(xhift); }};
    Eventful<double> freq {500, on_freq_q_changed};
    Eventful<double> q {1.0, on_freq_q_changed};

    RealtimeMultiChannelTuner tuner;

    double pong_mult = 1.l;

    bool enable_tuner = false;

    void reconfig_filters() {
        tuner.config_filters(freq, q);
    }

    EventfulValueRange get_tune_knob_value() {
        switch (tuner_knob_mode) {
            case TunerKnobMode::Resonance:
                return {&q, 0.1, 9.99, fmt::format("R{:.2f}", q)};
            case TunerKnobMode::Frequency:
                return {&freq, 60, 10000, format_frequency(freq)};
            case TunerKnobMode::Xhift:
                return {&xhift, 0.1, 4, fmt::format("X{:.2f}", xhift)};
        }
    }

    EventfulValueRange get_pv_knob_value() {
        switch (pv_knob_mode) {
            case PVKnobMode::Pan:
                return {&pan, -1, 1};
            case PVKnobMode::Volume:
                return {&volume, 0, 1};
        }
    }

    struct ReadParams {
        double read;
        double speed;
        bool finished;
    };

    ReadParams compute_params(double start, double stop, double read) {
        double param_read = read;
        double param_speed = speed;

        if (read > stop || read < start) {
            switch (mode) {
                case PlaybackMode::OneShot:
                    return {speed > 0 ? start : stop, speed, true};
                case PlaybackMode::Loop: {
                    param_read = speed > 0 ? start : stop;
                    break;
                }
                case PlaybackMode::PingPong: {
                    pong_mult *= -1;
                    param_read = read < start ? start : stop;
                    break;
                }
            }
        }

        if (mode == PlaybackMode::PingPong)
            param_speed *= pong_mult;

        return {param_read, param_speed, false};
    }

    auto read_channels(SampleGetter get_sample, IdxType num_channels, double pos) -> std::vector<double> {
        auto data = std::vector<double>(num_channels);

        for (IdxType channel_idx = 0; channel_idx < num_channels; channel_idx++) {
            auto result = rounded_sum(pos, speed);
            auto p = result.more == result.less ? 0.0 : (result.actual - result.less) / (result.more - result.less);

            auto less_sample = get_sample(channel_idx, result.less);
            auto more_sample = get_sample(channel_idx, result.more);

            data[channel_idx] = less_sample + (more_sample - less_sample) * p;
        }

        return data;
    }

    auto repan(std::vector<double> frame) -> std::vector<double> {
        auto pan = this->pan.value;
        auto left = frame[0];
        auto right = frame[1];
        auto mid = (left + right) / 2;
        auto side = (left - right) / 2;
        auto new_left = mid + side * pan;
        auto new_right = mid - side * pan;

        auto result = std::vector<double>();
        return {new_left * volume, new_right * volume};
    }

    auto retune(IdxType frame_rate, std::vector<double> frame) -> std::vector<double> {
        if (tuner.sample_rate != frame_rate)
            tuner.set_sample_rate(frame_rate);
        return tuner.process(frame);
    }

    struct ReadResult {
        std::vector<double> data;
        double next;
        bool reached_end;
    };

    auto read_frame(
        SampleGetter get_sample,
        IdxType num_channels,
        IdxType frame_rate,
        double read,
        double start,
        double stop
    ) -> ReadResult {
        auto params = compute_params(start, stop, read);

        if (params.finished) {
            return {std::vector<double>(num_channels, 0.0), params.read, true};
        }

        auto data = read_channels(get_sample, num_channels, params.read);

        data = retune(frame_rate, data);

        data = repan(data);

        return {data, params.read + params.speed, false};
    }

    json_t* make_json_obj() {
        json_t* root = json_object();

        json_object_set(root, "speed", json_real(speed));
        json_object_set(root, "xhift", json_real(xhift));
        json_object_set(root, "mode", json_integer((int)mode));
        json_object_set(root, "pong_mult", json_real(pong_mult));

        return root;
    }

    void load_json(json_t* root) {
        speed = json_real_value(json_object_get(root, "speed"));
        xhift = json_real_value(json_object_get(root, "xhift"));
        mode = (PlaybackMode)json_integer_value(json_object_get(root, "mode"));
        pong_mult = json_real_value(json_object_get(root, "pong_mult"));
    }
};