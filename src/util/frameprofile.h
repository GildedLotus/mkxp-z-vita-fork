// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MKXP_FRAMEPROFILE_H
#define MKXP_FRAMEPROFILE_H

#include "vita_glue.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include "assetprofile.h"

namespace FrameProfile {
enum Bucket { Script, GC, Raster, Compose, Upload, Scene, Blit, Submit,
              Swap, Idle, IO, Decode };

// Secondary, mutually exclusive operation spans; these overlap the main buckets.
enum Operation { XPPrepare, VXPrepare, SpritePrepare, PlanePrepare, WindowPrepare,
                 OtherPrepare, TileVertices, TileBuffers, Autotile, Flash,
                 MapViewport, PlaneQuad, ViewportUpdate, ViewportDraw,
                 SpriteFirstBind, SpriteBind, SpriteDraw, PlaneDraw, TileDraw,
                 WindowDraw, OperationCount };

#ifdef __vita__
/* CPU-raster profile counters; defined in graphics.cpp, which
 * owns the State instance. finish() folds them into the logged batch and
 * restart() drops them with the interrupted batch. */
void foldRasterProfile(VitaRasterProfile *dst);
void resetRasterProfile();
#endif

#ifdef __vita__
/* Bitmap texture-cache eviction attribution; defined in
 * bitmap.cpp, which owns the cache. finish() emits the window's one
 * vita-texcache line and clears it; restart() drops an unflushed window. */
void logTexCacheWindow(unsigned long long serial);
void resetTexCacheWindow();
#endif

/* One writer (RGSS). Time belongs to the innermost span, never both parent
 * and child. No allocation, TLS, kernel object, or instrumentation in pixels. */
struct State {
    static const unsigned Bins = 4096;
    VitaFrameSummary batch;
    AssetProfile assets;
    unsigned imageToken = 0, pngDepth = 0;
    double frameUS[VITA_FRAME_BUCKETS];
    unsigned histogram[Bins + 1];
    double last, frameStart, batchStart;
    std::atomic<int> owner;
    Bucket current;
    unsigned depth;
    bool running;
    double operationUS[OperationCount]{};
    unsigned operationCalls[OperationCount]{};
    Operation operation = OperationCount;
    unsigned long long serial = 0;
    double previousLogUS = 0;

    State() : batch{}, frameUS{}, histogram{}, last(0), frameStart(0),
              batchStart(0), owner(0), current(Script), depth(0), running(false) {}

    void restart() {
        // Do not carry the previous game's batch across an engine reset.
        running = false;
        depth = 0;
        current = Script;
        batch = VitaFrameSummary{};
        assets.restart();
        operation = OperationCount;
        serial = 0;
        previousLogUS = 0;
        clearOperations();
        std::memset(frameUS, 0, sizeof(frameUS));
        std::memset(histogram, 0, sizeof(histogram));
#ifdef __vita__
        resetRasterProfile();
        resetTexCacheWindow();
#endif
    }

    void charge(double now) {
        frameUS[current] += now - last;
        if (operation != OperationCount) operationUS[operation] += now - last;
        last = now;
    }

    Bucket enter(Bucket next, unsigned long long bytes, bool counted) {
        charge(vita_glue_frame_profile_now_us());
        const Bucket previous = current;
        current = next;
        if (counted) ++batch.count[next];
        if (next == Upload) batch.upload_bytes += bytes;
        return previous;
    }

    void leave(Bucket previous) {
        charge(vita_glue_frame_profile_now_us());
        current = previous;
    }

    void clearOperations() {
        std::memset(operationUS, 0, sizeof(operationUS));
        std::memset(operationCalls, 0, sizeof(operationCalls));
    }

    static unsigned cappedUS(double us) {
        return us >= 999999 ? 999999 : (us > 0 ? unsigned(us) : 0);
    }

    void slowFrame(double wall, bool skipped, bool frozen) const {
        static const char *const names[] = {
            "xp_prepare", "vx_prepare", "sprite_prepare", "plane_prepare", "window_prepare",
            "other_prepare", "tile_vertices", "tile_buffers", "autotile", "flash",
            "map_viewport", "plane_quad", "viewport_update", "viewport_draw",
            "sprite_first_bind", "sprite_bind", "sprite_draw", "plane_draw", "tile_draw", "window_draw"
        };
        unsigned top = OperationCount;
        bool capped = wall > 999999 || previousLogUS > 999999;
        for (unsigned i = 0; i < OperationCount; ++i) {
            if (operationCalls[i] && (top == OperationCount || operationUS[i] > operationUS[top])) top = i;
            capped |= operationUS[i] > 999999 || operationCalls[i] > 999;
        }
        for (double us : frameUS) capped |= us > 999999;
        // Worst case is <464 bytes, leaving room for the sink's optional timestamp.
        char line[464];
        size_t at = std::snprintf(line, sizeof(line),
            "vita-slow: f=%llu wall=%u skip=%u frozen=%u top=%s prevlog=%u cap=%u b=",
            serial, cappedUS(wall), unsigned(skipped), unsigned(frozen),
            top == OperationCount ? "none" : names[top], cappedUS(previousLogUS), unsigned(capped));
        for (unsigned i = 0; i < VITA_FRAME_BUCKETS && at < sizeof(line); ++i)
            at += std::snprintf(line + at, sizeof(line) - at, "%s%u", i ? "," : "", cappedUS(frameUS[i]));
        if (at < sizeof(line)) at += std::snprintf(line + at, sizeof(line) - at, " op=");
        for (unsigned i = 0; i < OperationCount && at < sizeof(line); ++i)
            at += std::snprintf(line + at, sizeof(line) - at, "%s%u/%u", i ? "," : "",
                cappedUS(operationUS[i]), operationCalls[i] > 999 ? 999 : operationCalls[i]);
        vita_glue_trace(line);
    }

    double percentile(unsigned percent) const {
        const unsigned rank = (batch.frames * percent + 99) / 100;
        unsigned total = 0;
        for (unsigned i = 0; i <= Bins; ++i) {
            total += histogram[i];
            if (total >= rank)
                return i == Bins ? batch.max_us : (i + 1) * 250.0;
        }
        return batch.max_us;
    }

    void finish(bool skipped, bool frozen, double targetUS) {
        const double now = vita_glue_frame_profile_now_us();
        charge(now);
        const double wall = now - frameStart;
        ++serial;
        // Frame-debt capture retains its existing no-extra-output experiment.
        const bool slow = wall > 50000.0 && !vita_measure_mode;
        if (slow) slowFrame(wall, skipped, frozen);
        previousLogUS = slow ? vita_glue_frame_profile_now_us() - now : 0;
        clearOperations();
        for (unsigned i = 0; i < VITA_FRAME_BUCKETS; ++i) {
            batch.us[i] += frameUS[i];
            frameUS[i] = 0;
        }
        ++batch.frames;
        batch.skipped += skipped;
        batch.frozen += frozen;
        batch.late += targetUS > 0 && wall > targetUS + 1000.0;
        batch.wall_us += wall;
        if (wall > batch.max_us) batch.max_us = wall;
        const unsigned bin = wall >= Bins * 250.0 ? Bins : unsigned(wall / 250.0);
        ++histogram[bin];
        current = Script;
        frameStart = now;
        if (batch.frames < vita_glue_frame_profile_interval || now - batchStart < 1000000.0)
            return;
        batch.p50_us = percentile(50);
        batch.p95_us = percentile(95);
#ifdef __vita__
        foldRasterProfile(&batch.raster);
#endif
        const double logStart = vita_glue_frame_profile_now_us();
        vita_glue_frame_profile_log(&batch);
        // A load window is one existing summary batch with an image decode.
        // Idle/audio-only batches emit no asset rows; measurement mode is inert.
        if (!vita_measure_mode && batch.count[Decode]) assets.log(serial, batch.count[IO], batch.count[Decode], batch.upload_bytes);
        assets.clearBatch();
#ifdef __vita__
        // The cache window closes with the summary that covers it, always:
        // an all-zero line is evidence too (nothing evicted across a scene).
        if (!vita_measure_mode) logTexCacheWindow(serial);
#endif
        batch = VitaFrameSummary{};
        std::memset(histogram, 0, sizeof(histogram));
        // Logging/reset time stays in the next frame's script residual.
        batchStart = vita_glue_frame_profile_now_us();
        previousLogUS += batchStart - logStart;
    }
};

extern State state;

inline bool assetActive() {
    return vita_glue_frame_profile_interval && !vita_measure_mode &&
           vita_glue_frame_profile_thread() == state.owner.load() && state.running;
}
inline void assetUpload(unsigned token, unsigned long long bytes) {
    if (!assetActive()) return;
    auto &c = state.assets.get(token); ++c.uploads; c.uploadBytes += bytes;
}

class Scope {
    Bucket previous;
    bool active;
    unsigned measuredPrevious;
    bool measured;
public:
    __attribute__((always_inline)) explicit Scope(Bucket bucket, unsigned long long bytes = 0,
                   bool sharedThread = false, bool counted = true) : previous(Script), active(false), measuredPrevious(0), measured(false) {
        if (vita_measure_mode && (!sharedThread ||
            state.owner.load() == vita_glue_frame_profile_thread())) {
            measuredPrevious = vita_measure_enter(bucket);
            measured = true;
        }
        // The immutable boot gate precedes clocks, thread queries and state access.
        if (vita_glue_frame_profile_interval &&
            (!sharedThread || vita_glue_frame_profile_thread() == state.owner.load()) && state.running) {
            previous = state.enter(bucket, bytes, counted);
            active = true;
        }
    }
    __attribute__((always_inline)) ~Scope() { stop(); }
    __attribute__((always_inline)) void stop() {
        if (measured) { vita_measure_leave(measuredPrevious); measured = false; }
        if (active) {
            state.leave(previous);
            active = false;
        }
    }
    bool enabled() const { return active; }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};

class AssetDecodeScope {
    Scope &scope;
    unsigned token, width = 0, height = 0;
    bool active, failed = true;
    double before;
public:
    AssetDecodeScope(Scope &scope, unsigned token) : scope(scope), token(token),
        active(assetActive()), before(active ? state.frameUS[Decode] : 0) {}
    void result(unsigned w, unsigned h) { width = w; height = h; failed = !w || !h; }
    ~AssetDecodeScope() {
        scope.stop();
        if (!active) return;
        auto &c = state.assets.get(token); ++c.decodes; c.decodeFail += failed;
        c.width = width; c.height = height;
        c.decodeUS += state.frameUS[Decode] - before;
    }
    AssetDecodeScope(const AssetDecodeScope &) = delete;
    AssetDecodeScope &operator=(const AssetDecodeScope &) = delete;
};

// Owner-only scopes; inactive profiling never reads a clock or worker state.
class AssetImageScope {
    bool active;
    unsigned previous;
public:
    explicit AssetImageScope(unsigned token) : active(assetActive()), previous(0) {
        if (active) { previous = state.imageToken; state.imageToken = token; }
    }
    ~AssetImageScope() { if (active) state.imageToken = previous; }
    AssetImageScope(const AssetImageScope &) = delete;
    AssetImageScope &operator=(const AssetImageScope &) = delete;
};
class AssetStageScope {
public:
    enum Stage { PNG, Inflate, Convert };
private:
    Stage stage;
    bool active;
    unsigned token = 0;
    double start = 0, io = 0;
public:
    explicit AssetStageScope(Stage stage) : stage(stage), active(assetActive()) {
        active = active && state.imageToken && (stage != Inflate || state.pngDepth);
        if (!active) return;
        token = state.imageToken;
        start = vita_glue_frame_profile_now_us(); state.charge(start);
        io = state.frameUS[IO];
        if (stage == PNG) ++state.pngDepth;
    }
    ~AssetStageScope() {
        if (!active) return;
        const double end = vita_glue_frame_profile_now_us(); state.charge(end);
        auto &c = state.assets.get(token);
        const double elapsed = end - start - (state.frameUS[IO] - io);
        if (stage == PNG) { --state.pngDepth; ++c.pngCalls; c.pngUS += elapsed; }
        if (stage == Inflate) { ++c.inflateCalls; c.inflateUS += elapsed; }
        if (stage == Convert) { ++c.convertCalls; c.convertUS += elapsed; }
    }
    AssetStageScope(const AssetStageScope &) = delete;
    AssetStageScope &operator=(const AssetStageScope &) = delete;
};

class OperationScope {
    Operation previous = OperationCount;
    bool active = false;
public:
    explicit OperationScope(Operation next) {
        if (!vita_glue_frame_profile_interval || !state.running) return;
        state.charge(vita_glue_frame_profile_now_us());
        previous = state.operation;
        state.operation = next;
        ++state.operationCalls[next];
        active = true;
    }
    ~OperationScope() { stop(); }
    void stop() {
        if (!active) return;
        state.charge(vita_glue_frame_profile_now_us());
        state.operation = previous;
        active = false;
    }
    OperationScope(const OperationScope &) = delete;
    OperationScope &operator=(const OperationScope &) = delete;
};

class Frame {
    bool active;
    double target;
public:
    bool skipped, frozen;
    explicit Frame(double targetUS, bool internal = false) : active(false), target(targetUS), skipped(false), frozen(false) {
        if (vita_measure_mode) {
            state.owner = vita_glue_frame_profile_thread();
            vita_measure_begin(internal);
        }
        if (!vita_glue_frame_profile_interval) return;
        if (!state.running) {
            state.owner = vita_glue_frame_profile_thread();
            state.last = state.frameStart = state.batchStart = vita_glue_frame_profile_now_us();
            state.running = true;
        }
        active = ++state.depth == 1;
        if (active) state.enter(Submit, 0, false);
    }
    ~Frame() {
        if (vita_glue_frame_profile_interval) {
            --state.depth;
            if (active) state.finish(skipped, frozen, target);
        }
        if (vita_measure_mode) vita_measure_end((skipped ? 1u : 0u) | (frozen ? 2u : 0u));
    }
    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
};
} // namespace FrameProfile
#endif
