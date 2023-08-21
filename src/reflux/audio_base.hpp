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

enum { AUDIO_CLIP_DISPLAY_RES = 64 };
enum { AUDIO_CLIP_DISPLAY_CHANNELS = 2 };
using IdxType = uintptr_t;

using DisplayBufferType = std::array<std::vector<double>, 2>;

struct DisplayBufferBuilder {
    struct BuildArgs {
        std::function<double(IdxType, IdxType)> get_sample = nullptr;
        DisplayBufferType* dst = nullptr;
        IdxType start = 0;
        IdxType stop = 0;
        bool normalize = false;

        BuildArgs() = default;

        BuildArgs(
            std::function<double(IdxType, IdxType)> get_sample,
            DisplayBufferType* dst,
            IdxType start,
            IdxType stop,
            bool normalize = false
        ) :
            get_sample(get_sample),
            dst(dst),
            start(start),
            stop(stop),
            normalize(normalize) {}
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

        double inverse_chunk_size = 1.f / static_cast<double>(chunk_size);
        IdxType curr, i, j;
        double max, accum;
        int cidx = 0;
        int cidx1 = 1;

        // for each channel
        // for (int cidx = 0; cidx < 1; cidx++) {
            // we need to downsample the audio to fit the display
            if(buffer[cidx].size() != AUDIO_CLIP_DISPLAY_RES) buffer[cidx].resize(AUDIO_CLIP_DISPLAY_RES, 0.0);
            if(buffer[cidx1].size() != AUDIO_CLIP_DISPLAY_RES) buffer[cidx1].resize(AUDIO_CLIP_DISPLAY_RES, 0.0);
            curr = args.start;
            max = 0;
            for (i = 0; i < AUDIO_CLIP_DISPLAY_RES; i++) {
                accum = 0.0;
                for (j = 0; j < chunk_size; j++) {
                    accum += std::abs(args.get_sample(cidx, curr++));
                }
                buffer[cidx][i] = accum * inverse_chunk_size;
                buffer[cidx1][i] = accum * inverse_chunk_size;
                max = std::max(buffer[cidx][i], max);
            }
            auto inverse_max = 1.0 / max;
            if (args.normalize) {
                for (i = 0; i < AUDIO_CLIP_DISPLAY_RES; i++) {
                    buffer[cidx][i] *= inverse_max;
                    buffer[cidx1][i] *= inverse_max;
                }
            }
        //}
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

struct IIR4Filter {
    Biquad filter1;
    Biquad filter2;

    IIR4Filter(BQType FT, double sample_rate, double freq, double Q) :
        filter1(FT, freq / sample_rate, Q, 0.0),
        filter2(FT, freq / sample_rate, Q, 0.0) {}

    double config(double sample_rate, double freq, double Q) {
        filter1.setFc(freq / sample_rate);
        filter1.setQ(Q);
        filter1.reset();
        filter2.setFc(freq / sample_rate);
        filter2.setQ(Q);
        filter2.reset();
    }

    double process(double in) {
        return filter2.process(filter1.process(in));
    }
};

auto filter_many(std::vector<IIR4Filter>& filters, std::vector<double> frame) -> std::vector<double> {
    auto ret = std::vector<double>(frame.size());
    for (IdxType i = 0; i < frame.size(); i++) {
        ret[i] = filters[i].process(frame[i]);
    }
    return ret;
}

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
        auto data_up = get(index_up);
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
        else if (fsize < num_channels)
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
    return 0.5 * (1.0 - std::cos(M_PI * x));
}

int mod(int a, int b) {
    return (a % b + b) % b;
}

struct RealtimeMultiChannelTuner {
    MultiChannelBuffer filtered_buffer;
    MultiChannelBuffer reduction_buffer;
    std::vector<IIR4Filter> bandpass_filters;
    std::vector<IIR4Filter> lowpass_filters;
    std::vector<IIR4Filter> highpass_filters;

    enum OutputMode { OFF=0, MIX, WET, LP, BP, HP, NUM_MODES };

    OutputMode output_mode = OFF;

    double outptr = 0.0;
    double sample_rate = 1.0;
    double period_ratio = 1.0;
    double freq = 1000.0;
    double range = 1.0; // bandwith of the filter in octaves

    double period_length = 0.0;
    int zero_crossings_detected = 2;

    IdxType optimal_out_buffer_size() {
        return (IdxType)(sample_rate);  // 10000Hz is highest supported frequency
    }

    IdxType optimal_in_buffer_size() {
        return (IdxType)(3 * sample_rate / 100);  // 100Hz is lowest supported frequency
    }

    void set_output_mode(OutputMode mode) {
        output_mode = mode;
    }

    void set_channels(double num_channels) {
        filtered_buffer.set_channels(num_channels);
        reduction_buffer.set_channels(num_channels);
        config_filters(freq, range);
    }

    void set_sample_rate(double sample_rate) {
        this->sample_rate = sample_rate;
        auto in_buf_size = optimal_in_buffer_size();
        this->period_length = in_buf_size;
        filtered_buffer.set_size(in_buf_size);
        reduction_buffer.set_size(optimal_out_buffer_size());
    }

    void set_period_ratio(double period_ratio) {
        this->period_ratio = period_ratio;
    }

    void config_filters(double freq, double range) {
        this->freq = freq;
        this->range = range;
        
        double low_freq = pow(2.0, log2(freq)  - range * 0.75);
        double high_freq = pow(2.0, log2(freq) + range * 0.75);

        double w = 2.0 * M_PI * freq / sample_rate;
        double Q =  0.5 / sinh(0.5 * log(2) * range * w/sin(w));

        if (lowpass_filters.size() != filtered_buffer.channels())
            lowpass_filters.resize(filtered_buffer.channels(), IIR4Filter(BQType::lowpass, sample_rate, low_freq, 1));
        
        if (bandpass_filters.size() != filtered_buffer.channels())
            bandpass_filters.resize(filtered_buffer.channels(), IIR4Filter(BQType::bandpass, sample_rate, freq, Q));
        
        if (highpass_filters.size() != filtered_buffer.channels())
            highpass_filters.resize(filtered_buffer.channels(), IIR4Filter(BQType::highpass, sample_rate, high_freq, 1));

        for (IdxType i = 0; i < filtered_buffer.channels(); i++) {
            lowpass_filters[i].config(sample_rate, low_freq, 1);
            bandpass_filters[i].config(sample_rate, freq, Q);
            highpass_filters[i].config(sample_rate, high_freq, 1);
        }
    }

    struct FilterResult {
        std::vector<double> highpass;
        std::vector<double> bandpass;
        std::vector<double> lowpass;
    };

    auto filter_bands(std::vector<double> frame) -> FilterResult {
        auto bandpass = filter_many(bandpass_filters, frame);
        auto highpass = filter_many(highpass_filters, frame);
        auto lowpass = filter_many(lowpass_filters, frame);
        return {highpass, bandpass, lowpass};
    }

    auto process(std::vector<double> frame) -> std::vector<double> {
        if (frame.size() != filtered_buffer.channels())
            set_channels(frame.size());
        
        if(output_mode == OFF)
            return frame;

        auto bands = filter_bands(frame);
        auto filtered_frame = bands.bandpass;

        switch (output_mode) {
            case LP:
                return bands.lowpass;
            case BP:
                return bands.bandpass;
            case HP:
                return bands.highpass;
        }

        filtered_buffer.push(filtered_frame);

        // adjust outptr
        outptr += -1 + period_ratio;

        // how many output samples until the next underrun
        Optional<double> next_underrun = period_ratio < 1 ? Some(outptr / (1 - period_ratio)) : None<double>();

        // how many output samples until the next overrun
        Optional<double> next_overrun =
            period_ratio > 1 ? Some((filtered_buffer.size() - outptr) / (period_ratio - 1)) : None<double>();

        bool is_underrun = next_underrun.some() && next_underrun.value() <= 0;
        bool is_overrun = next_overrun.some() && next_overrun.value() <= 0;

        // pitch detection

        if (is_overrun || is_underrun) {
            period_length = 0;
            zero_crossings_detected = 0;

            IdxType recent_zero_corssing = filtered_buffer.size();
            std::vector<double>* newer_x = nullptr;
            std::vector<double>* x = nullptr;

            for (int i = recent_zero_corssing; i >= 0; i--) {
                newer_x = x;
                x = filtered_buffer.get(i);
                if (newer_x && (*newer_x)[0] > 0 && (*x)[0] < 0) {  // zero crossing detected
                    if (zero_crossings_detected > 0) {
                        period_length += recent_zero_corssing - i;
                    }
                    recent_zero_corssing = i;
                    zero_crossings_detected += 1;
                }
            }
            //period_length /= (zero_crossings_detected - 1);
            period_length = std::max(0.0, period_length);
        }

        // wrap outptr
        if (is_overrun)
            outptr -= period_length;
        else if (is_underrun)
            outptr += period_length;

        // get the actual output frame
        auto output_frame = filtered_buffer.get_smooth(outptr);

        // how many samples on avg bwteeen output zero crossings
        auto output_preriod_len = period_length * period_ratio / (zero_crossings_detected - 1);

        // apply overrun crossfade
        if (next_overrun.some()) {
            if (next_overrun.value() <= output_preriod_len) {
                double overrun = (1.0 - (next_overrun.value() / output_preriod_len));
                for (int i = 0; i < output_frame.size(); i++) {
                    output_frame[i] = output_frame[i] * (1.0 - overrun)
                        + filtered_buffer.get_smooth(outptr - period_length)[i] * overrun;
                }
            }
        }

        // apply underrun crossfade
        else if (next_underrun.some()) {
            if (next_underrun.value() <= output_preriod_len) {
                double underrun = (1.0 - (next_underrun.value() / output_preriod_len));
                for (int i = 0; i < output_frame.size(); i++) {
                    output_frame[i] = output_frame[i] * (1.0 - underrun)
                        + filtered_buffer.get_smooth(outptr + period_length)[i] * underrun;
                }
            }
        }
        
        if (output_mode == WET)
            return output_frame;

        for (int i = 0; i < output_frame.size(); i++) {
            output_frame[i] += (bands.lowpass[i] + bands.highpass[i]) * 0.5;
        }

        return output_frame;
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
    enum class TunerKnobMode { Range = 0, Frequency, Xhift, NUM_MODES };
    enum class VPKnobMode { Volume = 0, Pan, NUM_MODES };

    PlaybackMode mode = PlaybackMode::OneShot;
    TunerKnobMode tuner_knob_mode = TunerKnobMode::Range;
    VPKnobMode vp_knob_mode = VPKnobMode::Volume;

    Eventful<double>::Callback on_freq_range_changed = [this](EventfulBase::Event, double) { this->reconfig_filters(); };

    Eventful<double> volume = 1.l;
    Eventful<double> pan = 0.l;
    Eventful<double> speed = 1.l;
    Eventful<double> xhift {1.l, [this](EventfulBase::Event, double) { this->tuner.set_period_ratio(xhift); }};
    Eventful<double> freq {500, on_freq_range_changed};
    Eventful<double> range {1.0, on_freq_range_changed};

    RealtimeMultiChannelTuner tuner;

    double pong_mult = 1.l;

    void reconfig_filters() {
        tuner.config_filters(freq, range);
    }

    EventfulValueRange get_tune_knob_value() {
        switch (tuner_knob_mode) {
            case TunerKnobMode::Range:
                return {&range, 0.1, 9.99, fmt::format("R{:.2f}", range)};
            case TunerKnobMode::Frequency:
                return {&freq, 60, 10000, format_frequency(freq)};
            case TunerKnobMode::Xhift:
                return {&xhift, 0.1, 4, fmt::format("X{:.2f}", xhift)};
        }
    }

    EventfulValueRange get_pv_knob_value() {
        switch (vp_knob_mode) {
            case VPKnobMode::Volume:
                return {&volume, 0, 1, fmt::format("V{:.2f}", volume)};
            case VPKnobMode::Pan:
                return {&pan, -1, 1, fmt::format("{:.2f}", pan)};
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

    auto retune(std::vector<double> frame, IdxType frame_rate) -> std::vector<double> {
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

        data = retune(data, frame_rate);

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