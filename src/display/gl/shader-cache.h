// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MKXP_SHADER_CACHE_H
#define MKXP_SHADER_CACHE_H

#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <dirent.h>
#include <sys/stat.h>

// Regenerable files only. No cache operation creates or destroys a GL object.
namespace ShaderCache {
constexpr size_t MaxBinary = 512 * 1024, MaxBytes = 8 * 1024 * 1024;
constexpr unsigned MaxEntries = 32, HeaderSize = 32, MaxFormats = 16;
constexpr const char *Directory = "ux0:/data/mkxp-z/cache/shaders";
constexpr const char *DisableMarker = "ux0:/data/mkxp-z/shader-cache.disabled";
constexpr GLenum BinaryLength = 0x8741, NumFormats = 0x87FE, Formats = 0x87FF;
using GetBinary = void (APIENTRYP)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
using PutBinary = void (APIENTRYP)(GLuint, GLenum, const void *, GLint);
using Buffer = std::unique_ptr<unsigned char[]>;

inline uint64_t hash(const void *bytes, size_t size, uint64_t h = UINT64_C(14695981039346656037)) {
    const unsigned char *p = static_cast<const unsigned char *>(bytes);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}
inline void put(unsigned char *p, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) { p[i] = static_cast<unsigned char>(v); v >>= 8; }
}
inline uint64_t get(const unsigned char *p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= uint64_t(p[i]) << (i * 8);
    return v;
}
struct Key {
    uint64_t value = UINT64_C(14695981039346656037);
    void bytes(const void *p, size_t n) {
        unsigned char size[8]; put(size, n, 8);
        value = hash(p, n, hash(size, sizeof(size), value));
    }
    void number(uint64_t n) { unsigned char b[8]; put(b, n, 8); bytes(b, sizeof(b)); }
    void text(const char *s) { bytes(s, std::strlen(s)); }
};
inline bool exists(const char *path) { struct stat s; return stat(path, &s) == 0; }
inline bool extension(const char *s, const char *wanted) {
    if (!s) return false;
    size_t n = std::strlen(wanted);
    for (const char *p = s; (p = std::strstr(p, wanted)); p += n)
        if ((p == s || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}
inline void path(char *out, size_t size, uint64_t key, const char *suffix = "bin") {
    std::snprintf(out, size, "%s/%016llx.%s", Directory,
                  static_cast<unsigned long long>(key), suffix);
}
inline bool owned(const char *name) {
    if (std::strlen(name) != 20) return false;
    for (int i = 0; i < 16; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return !std::strcmp(name + 16, ".bin") || !std::strcmp(name + 16, ".tmp");
}
inline bool room(size_t needed) {
    mkdir("ux0:/data", 0777); mkdir("ux0:/data/mkxp-z", 0777);
    mkdir("ux0:/data/mkxp-z/cache", 0777); mkdir(Directory, 0777);
    for (;;) {
        DIR *d = opendir(Directory);
        if (!d) return false;
        unsigned count = 0; size_t bytes = 0;
        bool ok = true, stale = false;
        char victim[160] = {};
        for (;;) {
            errno = 0; dirent *e = readdir(d);
            if (!e) { if (errno) ok = false; break; }
            if (!owned(e->d_name)) continue;
            char file[160]; std::snprintf(file, sizeof(file), "%s/%.20s", Directory, e->d_name);
            struct stat s;
            if (stat(file, &s) || !S_ISREG(s.st_mode)) { ok = false; break; }
            bool invalid = !std::strcmp(e->d_name + 16, ".tmp") ||
                           s.st_size < static_cast<off_t>(HeaderSize) ||
                           s.st_size > static_cast<off_t>(HeaderSize + MaxBinary);
            if (!victim[0] || invalid) std::strcpy(victim, file);
            stale |= invalid;
            if (count < MaxEntries) ++count;
            if (!invalid && bytes < MaxBytes) bytes += size_t(s.st_size);
        }
        if (closedir(d)) ok = false;
        if (!ok) return false;
        // Reserve a slot and bytes for the temporary. Never mutate an open
        // directory iteration: a device iterator could skip shifted entries.
        if (!stale && count < MaxEntries && bytes <= MaxBytes - needed) return true;
        if (!victim[0] || std::remove(victim)) return false;
    }
}
enum Read { Missing, Invalid, Valid };
inline Read read(uint64_t key, GLenum format, Buffer &data, unsigned &length) {
    char file[160]; path(file, sizeof(file), key);
    FILE *f = std::fopen(file, "rb");
    if (!f) return exists(file) ? Invalid : Missing;
    unsigned char h[HeaderSize];
    bool ok = std::fread(h, 1, sizeof(h), f) == sizeof(h) &&
              !std::memcmp(h, "MKXPSB01", 8) && get(h + 8, 8) == key && get(h + 16, 4) == format;
    length = ok ? unsigned(get(h + 20, 4)) : 0;
    ok = ok && length > 0 && length <= MaxBinary;
    if (ok) { data.reset(new (std::nothrow) unsigned char[length]); ok = bool(data); }
    if (ok) ok = std::fread(data.get(), 1, length, f) == length &&
                 std::fgetc(f) == EOF && !std::ferror(f) && hash(data.get(), length) == get(h + 24, 8);
    if (std::fclose(f)) ok = false;
    return ok ? Valid : Invalid;
}
inline bool write(uint64_t key, GLenum format, const unsigned char *data, unsigned length) {
    if (!length || length > MaxBinary || !room(HeaderSize + length)) return false;
    char file[160], temp[160]; path(file, sizeof(file), key); path(temp, sizeof(temp), key, "tmp");
    unsigned char h[HeaderSize]; std::memcpy(h, "MKXPSB01", 8);
    put(h + 8, key, 8); put(h + 16, format, 4); put(h + 20, length, 4); put(h + 24, hash(data, length), 8);
    FILE *f = std::fopen(temp, "wb");
    if (!f) return false;
    bool ok = std::fwrite(h, 1, sizeof(h), f) == sizeof(h) && std::fwrite(data, 1, length, f) == length;
    if (std::fflush(f)) ok = false;
    if (std::fclose(f)) ok = false;
    // Vita rename can remove the destination before failing; validated reads recover.
    if (ok) ok = std::rename(temp, file) == 0;
    if (!ok) std::remove(temp);
    return ok;
}
struct Session {
    GetBinary getBinary = nullptr;
    PutBinary putBinary = nullptr;
    GLint formats[MaxFormats] = {}, count = 0;
    unsigned hits = 0, misses = 0, fallbacks = 0, disabled = 0, writes = 0, writeFailures = 0;
    bool enabled = false;
    Session() {
#ifdef MKXPZ_VITAGL_BACKEND
        return;  // vitaGL keeps its own GXP cache (shipped in the VPK); it has no program-binary extension
#endif
        if (exists(DisableMarker) || !gl.glsles ||
            !extension(reinterpret_cast<const char *>(gl.GetString(GL_EXTENSIONS)), "GL_OES_get_program_binary")) return;
        getBinary = reinterpret_cast<GetBinary>(SDL_GL_GetProcAddress("glGetProgramBinaryOES"));
        putBinary = reinterpret_cast<PutBinary>(SDL_GL_GetProcAddress("glProgramBinaryOES"));
        if (!getBinary || !putBinary) return;
        gl.GetIntegerv(NumFormats, &count);
        if (gl.GetError() != GL_NO_ERROR) return;
        if (count <= 0 || count > GLint(MaxFormats)) return;
        gl.GetIntegerv(Formats, formats);
        if (gl.GetError() != GL_NO_ERROR || !gl.GetString(GL_RENDERER) || !gl.GetString(GL_VERSION)) return;
        enabled = true;
    }
    Key identity() const {
        Key key; key.text("mkxp-shader-cache-v1");
        for (GLenum field : {GL_RENDERER, GL_VERSION}) {
            const char *s = reinterpret_cast<const char *>(gl.GetString(field));
            key.text(s ? s : "");
        }
        return key;
    }
    uint64_t formatted(Key key, GLenum format) const { key.number(format); return key.value; }
    const char *load(GLuint program, Key key) {
        if (!enabled) { ++disabled; return "disabled"; }
        bool bad = false;
        for (GLint i = 0; i < count; ++i) {
            Buffer data; unsigned length = 0;
            Read result = read(formatted(key, formats[i]), formats[i], data, length);
            if (result == Missing) continue;
            if (result == Invalid) { bad = true; continue; }
            putBinary(program, formats[i], data.get(), length);
            GLint linked = GL_FALSE; gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
            // Binary rejection must not leave an error for the later GPU warm-up.
            GLenum error = gl.GetError();
            if (linked == GL_TRUE && error == GL_NO_ERROR) { ++hits; return "hit"; }
            bad = true;
        }
        if (bad) { ++fallbacks; return "fallback"; }
        ++misses; return "miss";
    }
    void save(GLuint program, Key key) {
        if (!enabled) return;
        GLint length = 0; gl.GetProgramiv(program, BinaryLength, &length);
        GLenum error = gl.GetError();
        if (error != GL_NO_ERROR || length <= 0 || size_t(length) > MaxBinary) { ++writeFailures; return; }
        Buffer data(new (std::nothrow) unsigned char[length]);
        if (!data) { ++writeFailures; return; }
        GLsizei actual = 0; GLenum format = 0;
        getBinary(program, length, &actual, &format, data.get());
        error = gl.GetError(); bool supported = false;
        for (GLint i = 0; i < count; ++i) supported |= GLenum(formats[i]) == format;
        if (error == GL_NO_ERROR && actual > 0 && actual <= length && supported &&
            write(formatted(key, format), format, data.get(), unsigned(actual))) ++writes;
        else ++writeFailures;
    }
    void summary() const {
        char line[224];
        std::snprintf(line, sizeof(line),
            "vita-shader-cache: enabled=%u hits=%u misses=%u fallbacks=%u disabled=%u writes=%u write_failures=%u max_entries=%u max_bytes=%u",
            unsigned(enabled), hits, misses, fallbacks, disabled, writes, writeFailures, MaxEntries, unsigned(MaxBytes));
        vita_glue_trace(line);
    }
};
inline Session &session() { static Session value; return value; }
}
#endif
