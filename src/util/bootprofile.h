// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MKXP_BOOTPROFILE_H
#define MKXP_BOOTPROFILE_H
namespace BootProfile {
struct PathCacheIO {
    unsigned readdirCount = 0, statCount = 0;
    double readdirUS = 0, statUS = 0;
};
}
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include <cstdio>
#include <cstring>
namespace BootProfile {
inline double now() { return vita_glue_frame_profile_now_us(); }
enum Phase { Glue, Ruby, Scripts, PathCache, RTP, Fonts, GLInit, Shaders, Archive, Count };
struct State {
    double start, elapsed[Count], began[Count];
    unsigned seen, active;
    bool presented;
    State() : start(vita_glue_frame_profile_now_us()), elapsed{}, began{}, seen(0), active(0), presented(false) {}
};
inline State &state() { static State value; return value; }
inline const char *name(Phase p) {
    static const char *names[] = {"glue", "ruby", "scripts", "path_cache", "rtp", "fonts", "gl_init", "shaders", "archive"};
    return names[p];
}
// These spans cross SDL's window/context threads and ShaderSet member construction.
inline void begin(Phase p) {
    State &s = state();
    if (s.presented || (s.active & (1u << p)) || (s.seen & (1u << p))) return;
    s.active |= 1u << p; s.began[p] = vita_glue_frame_profile_now_us();
    char line[128];
    std::snprintf(line, sizeof(line), "vita-boot: phase=%s begin t_ms=%.3f", name(p), s.began[p] / 1000.0);
    vita_glue_trace(line);
}
inline void end(Phase p) {
    State &s = state();
    if (!(s.active & (1u << p))) return;
    s.active &= ~(1u << p); s.seen |= 1u << p;
    double now = vita_glue_frame_profile_now_us(); s.elapsed[p] += now - s.began[p];
    char line[160];
    std::snprintf(line, sizeof(line), "vita-boot: phase=%s end t_ms=%.3f elapsed_ms=%.3f",
                  name(p), now / 1000.0, s.elapsed[p] / 1000.0);
    vita_glue_trace(line);
}
class Scope {
    Phase phase;
    double start;
    bool active;
    const PathCacheIO *io;
public:
    explicit Scope(Phase p, const PathCacheIO *stats = nullptr) :
        phase(p), start(0), active(!state().presented), io(stats) {
        if (active) {
            start = vita_glue_frame_profile_now_us();
            char line[128];
            std::snprintf(line, sizeof(line), "vita-boot: phase=%s begin t_ms=%.3f", name(p), start / 1000.0);
            vita_glue_trace(line);
        }
    }
    void finish() {
        if (!active) return;
        active = false;
        double end = vita_glue_frame_profile_now_us();
        state().elapsed[phase] += end - start;
        state().seen |= 1u << phase;
        char line[320];
        std::snprintf(line, sizeof(line), "vita-boot: phase=%s end t_ms=%.3f elapsed_ms=%.3f",
                      name(phase), end / 1000.0, (end - start) / 1000.0);
        if (io) {
            const size_t used = std::strlen(line);
            std::snprintf(line + used, sizeof(line) - used,
                " readdir_count=%u readdir_ms=%.3f fallback_stat_count=%u fallback_stat_ms=%.3f",
                io->readdirCount, io->readdirUS / 1000.0, io->statCount, io->statUS / 1000.0);
        }
        vita_glue_trace(line);
    }
    ~Scope() { finish(); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};
inline void firstFrame() {
    State &s = state();
    if (s.presented) return;
    s.presented = true;
    double now = vita_glue_frame_profile_now_us();
    char line[480];
    std::snprintf(line, sizeof(line),
        "vita-boot: first_frame t_ms=%.3f total_ms=%.3f glue_ms=%.3f ruby_ms=%.3f scripts_ms=%.3f path_cache_ms=%.3f rtp_ms=%.3f fonts_ms=%.3f gl_init_ms=%.3f shaders_ms=%.3f archive_ms=%.3f seen=0x%x inclusive=1",
        now / 1000.0, (now - s.start) / 1000.0,
        s.elapsed[Glue] / 1000.0, s.elapsed[Ruby] / 1000.0, s.elapsed[Scripts] / 1000.0,
        s.elapsed[PathCache] / 1000.0, s.elapsed[RTP] / 1000.0, s.elapsed[Fonts] / 1000.0,
        s.elapsed[GLInit] / 1000.0, s.elapsed[Shaders] / 1000.0, s.elapsed[Archive] / 1000.0, s.seen);
    vita_glue_trace(line);
}
}
#else
namespace BootProfile {
enum Phase { Glue, Ruby, Scripts, PathCache, RTP, Fonts, GLInit, Shaders, Archive };
inline void begin(Phase) {}
inline void end(Phase) {}
inline double now() { return 0; }
struct Scope { explicit Scope(Phase, const PathCacheIO * = nullptr) {} void finish() {} };
inline void firstFrame() {}
}
#endif
#endif
