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

#include "plugin.hpp"
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