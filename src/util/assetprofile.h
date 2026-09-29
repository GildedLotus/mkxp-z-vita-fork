// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MKXP_ASSETPROFILE_H
#define MKXP_ASSETPROFILE_H

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace FrameProfile {
// One RGSS writer; identities survive batch flushes so lazy uploads and open
// streams retain their asset. Overflow is explicit, never silently discarded.
struct AssetProfile {
    enum Source { Loose, Archive, RTP, RTPArchive, Unknown };
    static const unsigned Limit = 64;
    struct Counts {
        uint64_t bytes = 0, requested = 0, minimum = 0, maximum = 0, uploadBytes = 0;
        unsigned opens = 0, openFail = 0, reads = 0, shortReads = 0, readFail = 0;
        unsigned seeks = 0, seekFail = 0, decodes = 0, decodeFail = 0, uploads = 0;
        unsigned width = 0, height = 0;
        double ioUS = 0, decodeUS = 0;
        unsigned pngCalls = 0, inflateCalls = 0, convertCalls = 0;
        double pngUS = 0, inflateUS = 0, convertUS = 0;
        bool used() const { return opens || reads || seeks || decodes || uploads; }
    };
    struct Entry {
        char path[512]{};
        uint64_t mount = 0;
        Source source = Unknown;
        Counts counts;
    } entries[Limit + 1];
    unsigned used = 0, generation = 1;
    uint64_t rtps[16]{};
    unsigned rtpCount = 0;
    uint64_t gameMount = 0;
    bool rtpOverflow = false;

    static uint64_t hash(const char *s) {
        uint64_t value = UINT64_C(14695981039346656037);
        if (s) for (; *s; ++s) { value ^= (unsigned char)*s; value *= UINT64_C(1099511628211); }
        return value;
    }
    void clearMounts() { rtpCount = 0; rtpOverflow = false; }
    void mountGame(const char *path) { gameMount = hash(path); }
    void mountRTP(const char *path) {
        const uint64_t key = hash(path);
        for (unsigned i = 0; i < rtpCount; ++i) if (rtps[i] == key) return;
        if (rtpCount < 16) rtps[rtpCount++] = key; else rtpOverflow = true;
    }
    Source source(const char *mount, bool archive) const {
        if (!mount) return Unknown;
        const uint64_t key = hash(mount);
        for (unsigned i = 0; i < rtpCount; ++i)
            if (rtps[i] == key) return archive ? RTPArchive : RTP;
        return archive ? Archive : (key == gameMount ? Loose : Unknown);
    }
    void restart() {
        used = 0;
        generation = generation == 0xffffff ? 1 : generation + 1;
        for (auto &entry : entries) entry = Entry{};
    }
    unsigned identify(const char *path, const char *mount, Source source) {
        unsigned i = Limit;
        if (path && std::strlen(path) < sizeof(entries[0].path)) {
            const uint64_t key = hash(mount);
            for (i = 0; i < used; ++i)
                if (entries[i].mount == key && entries[i].source == source &&
                    !std::strcmp(entries[i].path, path)) break;
            if (i == used && used < Limit) {
                std::strcpy(entries[i].path, path);
                entries[i].mount = key; entries[i].source = source; ++used;
            }
        }
        return (generation << 7) | (i + 1);
    }
    Counts &get(unsigned token) {
        const unsigned slot = (token & 127) - 1;
        return entries[(token >> 7) == generation && slot < used ? slot : Limit].counts;
    }
    void read(unsigned token, uint64_t requested, int64_t result, double us) {
        auto &c = get(token); ++c.reads; c.requested += requested; c.ioUS += us;
        if (c.reads == 1 || requested < c.minimum) c.minimum = requested;
        if (requested > c.maximum) c.maximum = requested;
        if (result < 0) ++c.readFail;
        else { c.bytes += uint64_t(result); c.shortReads += uint64_t(result) < requested; }
    }
    static unsigned micros(double us) { return us > 999999999 ? 999999999 : (us > 0 ? unsigned(us) : 0); }
    void log(unsigned long long frame, unsigned ioCount, unsigned decodeCount,
             unsigned long long uploadBytes) const {
        unsigned opens = 0, reads = 0, decodes = 0;
        uint64_t uploads = 0;
        for (const auto &e : entries) {
            opens += e.counts.opens; reads += e.counts.reads;
            decodes += e.counts.decodes; uploads += e.counts.uploadBytes;
        }
        char summary[320];
        std::snprintf(summary, sizeof(summary),
            "vita-asset-window: f=%llu reads=%u open=%u rwread=%u decodes=%u attributed=%u upload_bytes=%llu bitmap_bytes=%llu rtp_overflow=%u",
            frame, ioCount, opens, reads, decodeCount, decodes, uploadBytes,
            (unsigned long long)uploads, unsigned(rtpOverflow));
        vita_glue_trace(summary);
        static const char *const sources[] = {"loose", "archive", "rtp", "rtp-archive", "unknown"};
        for (unsigned i = 0; i <= Limit; ++i) {
            const auto &e = entries[i]; const auto &c = e.counts;
            if (!c.used()) continue;
            // Percent-escape path bytes (including line breaks); show truncation.
            char path[289]; unsigned n = 0, at = 0;
            const char *name = i == Limit ? "<overflow-or-untracked>" : e.path;
            for (; name[n] && n < 96; ++n) {
                const unsigned char ch = name[n];
                if (ch >= 33 && ch <= 126 && ch != '%') path[at++] = ch;
                else { std::snprintf(path + at, 4, "%%%02X", ch); at += 3; }
            }
            path[at] = 0;
            char line[512];
            std::snprintf(line, sizeof(line),
                "vita-asset: f=%llu id=%u src=%s mount=%016llx path=%s trunc=%u",
                frame, i, sources[e.source], (unsigned long long)e.mount, path, unsigned(name[n] != 0));
            vita_glue_trace(line);
            std::snprintf(line, sizeof(line),
                "vita-asset-counts: id=%u open=%u/%u read=%u/%u/%u bytes=%llu/%llu size=%llu/%llu seek=%u/%u decode=%u/%u wh=%u/%u us=%u/%u upload=%u/%llu",
                i, c.opens, c.openFail, c.reads, c.shortReads, c.readFail,
                (unsigned long long)c.bytes, (unsigned long long)c.requested,
                (unsigned long long)c.minimum, (unsigned long long)c.maximum,
                c.seeks, c.seekFail, c.decodes, c.decodeFail, c.width, c.height,
                micros(c.ioUS), micros(c.decodeUS), c.uploads, (unsigned long long)c.uploadBytes);
            vita_glue_trace(line);
            if (c.pngCalls || c.convertCalls) {
                std::snprintf(line, sizeof(line),
                    "vita-asset-counts: id=%u stages png=%u/%u inflate=%u/%u convert=%u/%u",
                    i, c.pngCalls, micros(c.pngUS), c.inflateCalls, micros(c.inflateUS),
                    c.convertCalls, micros(c.convertUS));
                vita_glue_trace(line);
            }
        }
    }
    void clearBatch() { for (auto &e : entries) e.counts = Counts{}; }
};
} // namespace FrameProfile
#endif
