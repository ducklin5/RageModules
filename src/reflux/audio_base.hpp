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
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <list>

#include "plugin.hpp"
#include "src/shared/components.hpp"
#include "src/shared/make_builder.hpp"
#include "src/shared/math.hpp"
#include "src/shared/nvg_helpers.hpp"
#include "src/shared/utils.hpp"
#include "src/external/biquad.hpp"

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

    IIR4BandPass(double sample_rate, double freq, double Q): 
        filter1(BQType::bandpass, freq/sample_rate, Q, 0.0),
        filter2(BQType::bandpass, freq/sample_rate, Q, 0.0)  {}

    double config(double sample_rate, double freq, double Q) {
        filter1.setFc(freq/sample_rate);
        filter1.setQ(Q);
        filter2.setFc(freq/sample_rate);
        filter2.setQ(Q);
    }

    double process(double in) {
        return filter2.process(filter1.process(in));
    }
};


struct MultiChannelBuffer {
    // implements a circular buffer
    std::vector<std::vector<double>> data;
    IdxType num_channels = 2;
    IdxType block_size = 500;
    
    IdxType oldest_idx = 0;
    
    MultiChannelBuffer() = default;

    MultiChannelBuffer(IdxType num_channels, IdxType block_size) :
        num_channels(num_channels),
        block_size(block_size),
        oldest_idx(0)
    {
        data = std::vector<std::vector<double>>(block_size, std::vector<double>(num_channels, 0.0));
    }

    void reset() {
         for (int i = 0; i < block_size; i++) {
            for (int chan = 0; chan < num_channels; chan++) {
                data[i][chan] = 0.0;
            }
        }
        oldest_idx = 0;
    }

    std::vector<double>* get(IdxType idx) {
        if (idx < 0) return nullptr;
        if (idx >= block_size) return nullptr;
        auto data_idx = (oldest_idx + idx) % block_size;
        return &data[data_idx];
    }
    
    const std::vector<double> get_const(IdxType idx) const {
        auto data_idx = (oldest_idx + idx) % block_size;
        return data[data_idx];
    }
    
    std::vector<double>* oldest() {
        return this->get(0);
    }

    std::vector<double>* newest() {
        return this->get(-1 + block_size);
    }

    void push(std::vector<double> frame) {
        frame.resize(num_channels, 0.0);
        data[oldest_idx] = frame;
        oldest_idx = (oldest_idx + 1) % block_size;
    }

    void resize(IdxType num_channels, IdxType block_size) {
        this->num_channels = num_channels;
        this->block_size = block_size;
        data = std::vector<std::vector<double>>(block_size, std::vector<double>(num_channels, 0.0));
    }
    
    friend auto operator<<(std::ostream& os, MultiChannelBuffer const& m ) -> std::ostream& { 
        os << "[\n";
        for (int i = 0; i < m.block_size; i++) {
            os << "    [";
            std::vector<double> frame = m.get_const(i);
            for ( int j = 0; j < m.num_channels; j++) {
                os << frame[j];
                if (j + 1 == m.num_channels) break;
                os << ",";
            }
            if (i + 1 == m.block_size) os << "]\n";
            else os << "],\n";
        }
        os << "]\n";
        return os ;
    }
};

double window_fn(double x) {
    return 0.5 * (1.0 + std::cos(2.0 * M_PI * x));
}

struct RealtimeMultiChannelTuner {
    std::vector<IIR4BandPass> filters;
    IdxType num_channels = 2;
    double sample_rate = 44100;
    double in_block_size = 1000;
    double out_block_size = 1;
    double period_ratio = 1.0;

    MultiChannelBuffer in_buffer;
    MultiChannelBuffer out_buffer;


    void init (IdxType num_channels, double sample_rate, double period_ratio){
        this->num_channels = num_channels;
        this->sample_rate = sample_rate;
        this->period_ratio = period_ratio;
        filters = std::vector<IIR4BandPass>(num_channels, IIR4BandPass(sample_rate, 10000.0, 0.707));
        in_buffer.resize(num_channels, (int) in_block_size);
        out_buffer.resize(num_channels, (int) out_block_size);
    }
    
    auto bandpass(std::vector<double> frame) -> std::vector<double> {
        auto ret = std::vector<double>(frame.size());
        for (IdxType i = 0; i < frame.size(); i++) {
            ret[i] = filters[i].process(frame[i]);
        }
        return ret;
    }

    auto process(std::vector<double> frame) -> std::vector<double> {
        in_buffer.push(bandpass(frame));
        //out_buffer.reset();

        auto out = std::vector<double>(frame.size());
       
        IdxType period_length = 0;
        IdxType old_zero_corssing = 0;

        std::vector<double>* old_x = nullptr;
        std::vector<double>* x = nullptr;

        IdxType inptr = 0;
        IdxType outptr = 0;

        while(true) {
            inptr += 1;
            if (inptr >= in_block_size) break;

           // pitch detection
            old_x = x;
            x =  in_buffer.get(inptr);
           
            if (old_x && (*old_x)[0] > 0 && (*x)[0] < 0) { // zero crossing detected
                period_length = inptr - old_zero_corssing;
                old_zero_corssing = inptr;

                double outptr_ratio = (double) outptr / (double) out_block_size ;
                double inptr_ratio = (double) inptr / (double) in_block_size ;

                if ( outptr_ratio < inptr_ratio) {
                    outptr += (int) ((double) period_length * period_ratio);
                    for (int n = -period_length; n < (int) period_length; n++){
                        auto out_frame = out_buffer.get(n + outptr);
                        auto in_frame = in_buffer.get(n + inptr);
                        auto window = window_fn((double) n / period_length);
                        if (out_frame && in_frame) {
                            for (int chan = 0; chan < num_channels; chan++) {
                                (*out_frame)[chan] += (*in_frame)[chan] * window;
                            }
                        }
                    }
                }
            } 
            
        }
        
        return *out_buffer.oldest();
    }
};

using SampleGetter = std::function<double(double, double)>;

struct PlaybackProfile {
    enum PlaybackMode {
        OneShot=0,
        Loop,
        PingPong,
        NUM_MODES
    };


    PlaybackMode mode = OneShot;
    bool control_volume = true;

    Eventful<double> pan = 0.l;
    Eventful<double> volume = 1.l;
    Eventful<double> speed = 1.l;
    Eventful<double> pitch = 0.l;

   
    double pong_mult = 1.l;

    double sample_rate = 44100;
    IdxType num_channels = 1;

    bool enable_tuner = false;
    double last_pitch = 0;
    RealtimeMultiChannelTuner tuner;


    void init(IdxType num_channels, double sample_rate) {
        this->num_channels = num_channels;
        this->sample_rate = sample_rate;
        tuner.init(num_channels, sample_rate, pitch.value);
    }

    struct ReadParams {
        double read;
        double speed;
        bool finished;
    };

    ReadParams compute_params(double start, double stop, double read){
        double param_read = read;
        double param_speed = speed;

        if (read > stop || read < start) {
            switch(mode) {
                case OneShot:
                    return {speed > 0 ? start : stop, true};
                case Loop: {
                    read =  speed > 0 ? start : stop;
                    break;
                }
                case PingPong: {
                    pong_mult *= -1;
                    param_read =  read < start ? start : stop;
                    break;
                }
            }
        }
        
        if (mode == PingPong) param_speed *= pong_mult;

        return  {param_read, param_speed, false};
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

    auto retune(std::vector<double> frame) -> std::vector<double> {
        if (last_pitch != pitch.value) {
            //auto target_freq = 261.64 * pow(2.0, this->pitch.value/12.0);
            tuner.init(num_channels, sample_rate, pitch.value);
            last_pitch = pitch.value;
        }
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
        double read,
        double start,
        double stop
    ) -> ReadResult {
        auto params = compute_params(start, stop, read);

        if (params.finished) {
            return { std::vector<double>(num_channels, 0.0), params.read, true };
        }

        auto data = read_channels(get_sample, num_channels, params.read);
        
        data = retune(data);

        data = repan(data);

        return {data ,  params.read + params.speed, false };
    }

    json_t* make_json_obj() {
        json_t* root = json_object();

        json_object_set(root, "speed", json_real(speed));
        json_object_set(root, "pitch", json_real(pitch));
        json_object_set(root, "mode", json_integer((int) mode));
        json_object_set(root, "pong_mult", json_real(pong_mult));

        return root;
    }

    void load_json(json_t* root) {
        speed = json_real_value(json_object_get(root, "speed"));
        pitch = json_real_value(json_object_get(root, "pitch"));
        mode = (PlaybackMode)json_integer_value(json_object_get(root, "mode"));
        pong_mult = json_real_value(json_object_get(root, "pong_mult"));
    }
};