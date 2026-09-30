/*
 ** graphics.cpp
 **
 ** This file is part of mkxp.
 **
 ** Copyright (C) 2013 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
 **
 ** mkxp is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 2 of the License, or
 ** (at your option) any later version.
 **
 ** mkxp is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "bootprofile.h"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
namespace FrameProfile { State state; }
#ifdef MKXPZ_SOFTWARE_BITMAPS
namespace GPUBudget { void logMemoryLedger(); }
#endif
#endif

#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include "swraster.h"
namespace FrameProfile {
/* CPU-raster profile counters: fold swraster's accumulated
 * per-operation and destination-alpha counters into the batch about to be
 * logged, and drop them on engine reset so a discarded batch leaves nothing
 * behind. Single writer (the RGSS thread), like the rest of the state.
 * take() moves the counters, so a fold is idempotent per batch. */
void foldRasterProfile(VitaRasterProfile *dst)
{
    swraster::Profile p;
    swraster::profile_take(&p);
    static_assert(swraster::ProfileOpCount == VITA_RASTER_OPS,
                  "swraster ProfileOp table drifted from vita_glue.h");
    static_assert(swraster::DstAlphaClassCount == 3,
                  "swraster dst-alpha class table drifted from vita_glue.h");
    for (unsigned i = 0; i < VITA_RASTER_OPS; ++i) {
        dst->calls[i] += p.calls[i];
        dst->pixels[i] += p.pixels[i];
    }
    for (unsigned i = 0; i < 3; ++i) {
        dst->dst_alpha_calls[i] += p.dst_alpha_calls[i];
        dst->dst_alpha_pixels[i] += p.dst_alpha_pixels[i];
    }
}

void resetRasterProfile()
{
    swraster::profile_reset();
}

/* The marker is read once at boot (vita_glue_init), long before the first
 * Bitmap op; registering the address of the boot-cached interval makes it
 * the gate's single source of truth. */
static const struct SwRasterProfileGate {
    SwRasterProfileGate()
    {
        swraster::profile_set_gate(&vita_glue_frame_profile_interval);
    }
} swRasterProfileGate;
} // namespace FrameProfile
#endif
#include "graphics.h"

#include "alstream.h"
#include "audio.h"
#include "binding.h"
#include "bitmap.h"
#include "config.h"
#include "debugwriter.h"
#include "disposable.h"
#include "etc.h"
#include "etc-internal.h"
#include "eventthread.h"
#include "filesystem.h"
#include "gl-fun.h"
#include "gl-meta.h"
#include "gl-util.h"
#include "glstate.h"
#include "intrulist.h"
#include "quad.h"
#include "scene.h"
#include "shader.h"
#include "sharedstate.h"
#include "texpool.h"
#include "theoraplay/theoraplay.h"
#include "util.h"
#include "input.h"
#include "sprite.h"

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* The CPU-composed HUD surface (vita/overlay), on the include path like
 * swraster.h. */
#include "overlay.h"
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "settings_menu.h"
#endif
#include <cstring>
#endif

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_timer.h>
#include <SDL_video.h>
#include <SDL_mutex.h>
#include <SDL_thread.h>

#ifdef MKXPZ_STEAM
#include "steamshim_child.h"
#endif

#include <algorithm>
#include <atomic>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <cmath>
#include <climits>
#include <cstdio>
#include <memory>
#include <new>
#include <physfs.h>


#define DEF_SCREEN_W (rgssVer == 1 ? 640 : 544)
#define DEF_SCREEN_H (rgssVer == 1 ? 480 : 416)

#define DEF_FRAMERATE (rgssVer == 1 ? 40 : 60)

#define DEF_MAX_VIDEO_FRAMES 30
#define VIDEO_DELAY 10
/* Movie audio reaches AL in up to MOVIE_AUDIO_BUFS buffers of at most
 * MOVIE_AUDIO_BUFFER_MS (and MOVIE_AUDIO_BUFFER_SIZE interleaved samples):
 * ~0.35-0.4 s queued, against 3 x 2048 samples (~70 ms) before 0235. */
#define MOVIE_AUDIO_BUFFER_SIZE 4096
#define MOVIE_AUDIO_BUFS 8
#define MOVIE_AUDIO_BUFFER_MS 50
#define AUDIO_BUFFER_LEN_MS 2000
/* Movie header parse + first frame. Header pages sit at the front of the
 * file, so this is generous even on a slow card. */
#define MOVIE_PREPARE_TIMEOUT_MS 10000
/* First audio packet. A broken/silent audio stream must not hold the picture
 * hostage: after this, playback continues video-only. */
#define MOVIE_AUDIO_PREBUFFER_TIMEOUT_MS 1500
/* The audio thread polls the AL position every AUDIO_SLEEP; once it has not
 * advanced for this long, the movie clock stops following it. */
#define MOVIE_AUDIO_CLOCK_STALE_MS 100
/* Holding the first frame for the AL source to start playing. A source that
 * never starts must not hold the picture hostage either. */
#define MOVIE_AUDIO_START_TIMEOUT_MS 1500
/* Waiting on the decoder for the next frame. */
#define MOVIE_DECODER_POLL_MS 2
/* Once the picture ends, the sound still queued plays out for at most this
 * long: the queue reaches AUDIO_BUFFER_LEN_MS ahead, AL a little more. */
#define MOVIE_AUDIO_TAIL_MAX_MS (AUDIO_BUFFER_LEN_MS + 1000)

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* ---- The on-screen overlay's layout and storage -------
 *
 * vita/overlay gives a 512x128 ARGB surface with an 8x8 face; these are the
 * only numbers this file adds on top of it. Every one is a compile-time
 * constant, so the per-line address arithmetic below is a shift and an add --
 * the Cortex-A9 has no hardware integer divide. */
#define OVERLAY_MAX_LINES 12
#define OVERLAY_MAX_CHARS 62         /* (512 - 2*4) / 8 = 63, one spare */
#define OVERLAY_PAD_X 4
#define OVERLAY_PAD_Y 3
#define OVERLAY_LINE_H 10            /* 8 px face + 2 px leading */

/* The upload staging buffer. vita/overlay keeps its pixels as ARGB8888
 * (0xAARRGGBB) and this engine uploads every texture as GL_RGBA, so the two
 * bytes have to be swapped somewhere. The alternative was a BGRA upload --
 * the device does advertise GL_EXT_texture_format_BGRA8888 -- and it was
 * rejected: GLES2 wants internalformat == format, so it could not go through
 * TEX::uploadImage (which fixes internalformat at GL_RGBA), and changing this
 * texture's format after boot is exactly the re-specification that makes the
 * driver free and re-take its storage. overlay_copy_rgba8888() keeps the
 * overlay on the same upload path as every other texture in the engine.
 *
 * File-static, not a member: it is scratch, it is 256 KiB, and this way "no
 * dynamic allocation after boot" is true by construction rather than by
 * inspection of when GraphicsPrivate is built. */
static uint8_t overlayStaging[OVERLAY_PIXELS * 4];
#endif

typedef struct AudioQueue
{
    const THEORAPLAY_AudioPacket *audio;
    int offset;
    struct AudioQueue *next;
} AudioQueue;


/* What the decode worker reads through. On Vita the worker also places
 * itself on its first read: theoraplay creates it with
 * no pthread attributes, so it would run at the SDK's default priority on
 * any core; vita-movie reports where it ran. */
struct MovieSource
{
    SDL_RWops ops;
#ifdef __vita__
    int priority;
    int cpuMask;
    int pinResult;
    std::atomic<int> worker;

    MovieSource() : priority(0), cpuMask(0), pinResult(0), worker(0) {}
#endif
};

static long readMovie(THEORAPLAY_Io *io, void *buf, long buflen)
{
    MovieSource *source = (MovieSource *) io->userdata;
#ifdef __vita__
    if (!source->worker.load())
    {
        const SceUID self = sceKernelGetThreadId();
        if (source->priority)
            sceKernelChangeThreadPriority(self, source->priority);
        if (source->cpuMask)
            source->pinResult = sceKernelChangeThreadCpuAffinityMask(self, source->cpuMask);
        source->worker.store(self);
    }
#endif
    return (long) SDL_RWread(&source->ops, buf, 1, buflen);
} // IoFopenRead


static void closeMovie(THEORAPLAY_Io *io)
{
    MovieSource *source = (MovieSource *) io->userdata;
    SDL_RWclose(&source->ops);
    free(io);
} // IoFopenClose


#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* Cheap content probe behind the vita-movie line's changed= count: hash a
 * few strided Y rows of the decoded frame, so a device run can tell "the
 * decoder repeats itself" from "the picture does not follow the frames".
 * Constant divisors only. */
static uint32_t movieFrameHash(const THEORAPLAY_VideoFrame *frame)
{
    const size_t pitch = THEORAPLAY_YUVTEX_PITCH(frame->width);
    const unsigned bytes = frame->width < 256 ? frame->width : 256;
    uint32_t hash = 2166136261u;
    for (int r = 0; r < 5; ++r)
    {
        const size_t y = (size_t) ((uint64_t) (frame->height - 1) * r / 4);
        const uint8_t *row = frame->pixels + y * pitch;
        for (unsigned i = 0; i < bytes; ++i)
            hash = (hash ^ row[i]) * 16777619u;
    }
    return hash;
}
#endif

#ifdef __vita__
/* Scheduling evidence for the vita-movie line: priority, the cores a
 * thread was seen on, migrations and run time (SceKernelThreadInfo). */
struct MovieThreadSample
{
    int priority = -1;
    unsigned cpus = 0;
    unsigned moves = 0;
    uint64_t runClocks = 0;

    bool sample(SceUID thread)
    {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(thread, &info) < 0)
            return false;
        priority = info.currentPriority;
        for (int cpu : { (int) info.currentCpuId, (int) info.lastExecutedCpuId })
            if (cpu >= 0 && cpu < 4)
                cpus |= 1u << cpu;
        moves = info.changeCpuCount;
        runClocks = info.runClocks;
        return true;
    }
};
#endif


/* Stereo weights for a Vorbis channel layout over two channels (Vorbis I
 * spec 4.3.9): L/R at full weight, centre and surrounds at -3 dB, rear
 * centre halved, LFE dropped; each side normalised so full-scale input
 * cannot clip. Undefined layouts (over 8) mix every channel equally. */
static void movieDownmixWeights(int channels, float *left, float *right)
{
    static const char *const layouts[] = {
        "LCR", "LRlr", "LCRlr", "LCRlrE", "LCRlrcE", "LCRlrlrE" };
    const char *layout = channels >= 3 && channels <= 8 ? layouts[channels - 3] : 0;
    float sumL = 0.0f, sumR = 0.0f;
    for (int c = 0; c < channels; ++c) {
        float l = 1.0f, r = 1.0f;
        switch (layout ? layout[c] : 0) {
        case 'L': r = 0.0f; break;
        case 'R': l = 0.0f; break;
        case 'C': l = r = 0.7071f; break;
        case 'l': l = 0.7071f; r = 0.0f; break;
        case 'r': l = 0.0f; r = 0.7071f; break;
        case 'c': l = r = 0.5f; break;
        case 'E': l = r = 0.0f; break;
        }
        left[c] = l;
        right[c] = r;
        sumL += l;
        sumR += r;
    }
    for (int c = 0; c < channels; ++c) {
        left[c] /= sumL;
        right[c] /= sumR;
    }
}

static inline ALshort movieSample(float val)
{
    if (val < -1.0f)
        return SHRT_MIN;
    if (val > 1.0f)
        return SHRT_MAX;
    return (ALshort) (val * SHRT_MAX);
}

struct Movie
{
    THEORAPLAY_Decoder *decoder;
    const THEORAPLAY_AudioPacket *audio;
    const THEORAPLAY_VideoFrame *video;
    bool hasVideo;
    bool hasAudio;
    bool skippable;
    /* The frame's planes as decoded (THEORAPLAY_VIDFMT_YUVTEX) in one
     * sample-only texture, re-specified in place per frame and converted by
     * the boot-reserved MovieYuvShader: 3/8 of the RGBA bytes, and no
     * per-pixel float conversion on the decode worker. */
    TEX::ID planeTex;
    Vec2i planeSize;
    bool planeReady;
    /* Sampled from the first frame for the vita-movie telemetry line. */
    int videoW;
    int videoH;
    float videoFps;
    MovieSource source;
    SDL_Thread *audioThread;
    AtomicFlag audioThreadTermReq;
    volatile AudioQueue *audioQueueHead;
    volatile AudioQueue *audioQueueTail;
    ALuint audioSource;
    ALuint alBuffers[MOVIE_AUDIO_BUFS];
    ALshort audioBuffer[MOVIE_AUDIO_BUFFER_SIZE];
    SDL_mutex *audioMutex;
    /* The AL source and buffers exist. */
    bool audioStarted;
    /* Audio could not start: packets are dropped, the picture plays on. */
    bool audioDiscard;
    /* Sample frames waiting in the packet queue and taken into AL buffers
     * not yet played (both audioMutex), and the times a starved AL source
     * had to be restarted. */
    size_t pendingFrames;
    size_t inFlightFrames;
    std::atomic<unsigned> audioUnderruns;
    /* Audio clock. The audio thread alone keeps the sample-frame counts of
     * its queued AL buffers (oldest first) and of those already played, and
     * publishes the stream position, with the tick it was first seen at,
     * under audioMutex. */
    Uint32 queuedFrames[MOVIE_AUDIO_BUFS];
    unsigned queuedCount;
    uint64_t playedFrames;
    uint64_t clockFrames;
    int clockFreq;
    Uint32 clockBaseMs;
    Uint32 clockMs;
    Uint32 clockTicks;
    bool clockLive;

    Movie(bool skippable_)
    /* hasVideo/hasAudio must start defined: ~Movie reads hasAudio even when
     * preparePlayback bailed before setting it (UBSan-invalid bool load). */
    : decoder(0), audio(0), video(0), hasVideo(false), hasAudio(false),
      skippable(skippable_), planeTex(0), planeReady(false), videoW(0), videoH(0),
      videoFps(0.0f), audioThread(0), audioQueueHead(0), audioQueueTail(0),
      audioSource(0), audioMutex(0), audioStarted(false), audioDiscard(false),
      pendingFrames(0), inFlightFrames(0),
      audioUnderruns(0), queuedCount(0),
      playedFrames(0), clockFrames(0), clockFreq(0), clockBaseMs(0), clockMs(0),
      clockTicks(0), clockLive(false)
    {
    }
    /* The decode worker can exit before init on an I/O or header error, and
     * it stops feeding frames once it dies, so every prepare wait must also
     * end when THEORAPLAY_isDecoding() goes false or on the timeout -- the
     * only thing between a dead decoder and an RGSS thread wedged forever
     * (reproduced on the host offscreen runner). */
    bool decoderAlive(Uint32 &waited) const
    {
        return THEORAPLAY_isDecoding(decoder) && waited < MOVIE_PREPARE_TIMEOUT_MS;
    }
    bool preparePlayback()
    {
        
        // https://theora.org/doc/libtheora-1.0/codec_8h.html
        // https://ffmpeg.org/doxygen/0.11/group__lavc__misc__pixfmt.html
        THEORAPLAY_Io *io = (THEORAPLAY_Io *) malloc(sizeof (THEORAPLAY_Io));
        if(!io) {
            SDL_RWclose(&source.ops);
            return false;
        }

        io->read = readMovie;
        io->close = closeMovie;
        io->userdata = &source;
#ifdef __vita__
        /* Decode beside the rgss thread, not under it: a user core it is not
         * on (core 2 first), at its own priority rather than the pthread
         * default. */
        {
            SceKernelThreadInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0)
            {
                if (info.currentPriority >= 64 && info.currentPriority <= 191)
                    source.priority = info.currentPriority;
                for (int core : { 2, 1, 0 })
                    if (core != info.currentCpuId)
                    {
                        source.cpuMask = SCE_KERNEL_CPU_MASK_USER_0 << core;
                        break;
                    }
            }
        }
#endif
        decoder = THEORAPLAY_startDecode(io, DEF_MAX_VIDEO_FRAMES, THEORAPLAY_VIDFMT_YUVTEX);
        if (!decoder) {
            /* startDecode already closed the source through closeMovie. */
            return false;
        }
        
        // Wait until the decoder has parsed out some basic truths from the file.
        Uint32 waited = 0;
        while (!THEORAPLAY_isInitialized(decoder)) {
            if (!decoderAlive(waited))
                return false;
            SDL_Delay(VIDEO_DELAY);
            waited += VIDEO_DELAY;
        }
        
        // Once we're initialized, we can tell if this file has audio and/or video.
        hasAudio = THEORAPLAY_hasAudioStream(decoder);
        hasVideo = THEORAPLAY_hasVideoStream(decoder);
        
        // No video, so no point in doing anything else
        if (!hasVideo) {
            THEORAPLAY_stopDecode(decoder);
            decoder = 0;
            return false;
        }
        
        // Wait until we have video
        waited = 0;
        while ((video = THEORAPLAY_getVideo(decoder)) == NULL) {
            if (!decoderAlive(waited))
                return false;
            SDL_Delay(VIDEO_DELAY);
            waited += VIDEO_DELAY;
        }
        videoW = video->width;
        videoH = video->height;
        videoFps = video->fps;
        
        // Wait until we have audio, if applicable. The decoder reads past a
        // full video queue for it, so a full queue no longer
        // ends this wait: a clip whose first audio page follows more video
        // than the queue holds started without its sound.
        if (hasAudio) {
            waited = 0;
            while ((audio = THEORAPLAY_getAudio(decoder)) == NULL
                   && THEORAPLAY_isDecoding(decoder)
                   && waited < MOVIE_AUDIO_PREBUFFER_TIMEOUT_MS) {
                SDL_Delay(VIDEO_DELAY);
                waited += VIDEO_DELAY;
            }
        }
        /* Four samples a texel: pitch/4 wide, the Y rows plus the half-height
         * Cb|Cr rows tall, inside the 4096 texture limit. */
        planeSize = Vec2i((int) THEORAPLAY_YUVTEX_PITCH(videoW) / 4, videoH + videoH / 2);
        if (videoW < 2 || videoH < 2 || planeSize.x > 4096 || planeSize.y > 4096)
            return false;
        planeTex = TEX::gen();
        if (!planeTex.gl)
            return false;
        TEX::bind(planeTex);
        TEX::setRepeat(false);
        TEX::setSmooth(false);
        audioQueueHead = NULL;
        audioQueueTail = NULL;
        
        return true;
    }
    
    void queueAudioPacket(const THEORAPLAY_AudioPacket *audio) {
        AudioQueue *item = NULL;
        
        if (!audio) {
            return;
        }
        
        item = (AudioQueue *) malloc(sizeof (AudioQueue));
        if (!item) {
            THEORAPLAY_freeAudio(audio);
            return;  // oh well.
        }
        
        item->audio = audio;
        item->offset = 0;
        item->next = NULL;
        
        SDL_LockMutex(audioMutex);
        pendingFrames += (size_t) audio->frames;
        if (audioQueueTail) {
            audioQueueTail->next = item;
        } else {
            audioQueueHead = item;
        }
        audioQueueTail = item;
        SDL_UnlockMutex(audioMutex);
    }
    
    /* Queues decoded packets up to AUDIO_BUFFER_LEN_MS past `now`; the first
     * packet beyond that waits in `audio`. The check used to follow the
     * enqueue, so every call took one more packet. */
    void bufferMovieAudio(THEORAPLAY_Decoder *decoder, const Uint32 now) {
        for (;;) {
            if (!audio)
                audio = THEORAPLAY_getAudio(decoder);
            if (!audio || audio->playms >= now + AUDIO_BUFFER_LEN_MS)
                break;
            queueAudioPacket(audio);
            audio = NULL;
        }
    }

    /* Audio could not start: drop what the decoder produces. */
    void discardMovieAudio(THEORAPLAY_Decoder *decoder) {
        if (audio)
            THEORAPLAY_freeAudio(audio);
        while ((audio = THEORAPLAY_getAudio(decoder)) != NULL)
            THEORAPLAY_freeAudio(audio);
    }

    /* Sound still to be heard: held, queued, or in AL buffers not yet
     * played. The audio thread moves frames from queued to in flight under
     * audioMutex, so no frame is ever in neither. */
    bool audioPending()
    {
        if (audio)
            return true;
        SDL_LockMutex(audioMutex);
        const bool pending = pendingFrames > 0 || inFlightFrames > 0;
        SDL_UnlockMutex(audioMutex);
        return pending;
    }

    /* Audio thread: the stream position is the frames of every unqueued
     * buffer plus AL_SAMPLE_OFFSET into the still-queued ones. It moves in
     * mixer-period steps, so it is published only when it advances while
     * playing, stamped with the tick it was first seen at: the movie loop
     * extrapolates from that tick, and the clock is live from the first
     * sample the mixer actually consumed. */
    void publishAudioClock()
    {
        ALint offset = 0, state = 0;
        alGetSourcei(audioSource, AL_SAMPLE_OFFSET, &offset);
        alGetSourcei(audioSource, AL_SOURCE_STATE, &state);
        if (clockFreq <= 0 || offset < 0)
            return;
        const uint64_t frames = playedFrames + (uint64_t) offset;
        const bool playing = state == AL_PLAYING;
        SDL_LockMutex(audioMutex);
        if (playing && frames > clockFrames) {
            clockFrames = frames;
            clockMs = clockBaseMs + (Uint32) (frames * 1000u / (uint64_t) clockFreq);
            clockTicks = SDL_GetTicks();
        }
        clockLive = playing && clockFrames > 0;
        SDL_UnlockMutex(audioMutex);
    }

    /* Movie loop: the published position advanced to `ticks`, or false while
     * audio is not playing or its position has not moved recently. */
    bool audioClock(Uint32 ticks, Sint32 &ms)
    {
        SDL_LockMutex(audioMutex);
        Sint32 age = (Sint32) (ticks - clockTicks);
        if (age < 0)
            age = 0;
        const bool live = clockLive && age <= MOVIE_AUDIO_CLOCK_STALE_MS;
        ms = (Sint32) (clockMs + (Uint32) age);
        SDL_UnlockMutex(audioMutex);
        return live;
    }

    /* Converts up to `limit` sample frames from the packet queue into
     * audioBuffer as `out` (1 or 2) channels, freeing finished packets;
     * more than two channels are downmixed to stereo (they
     * used to reach AL labelled STEREO16). The caller holds
     * audioMutex. */
    ALuint takeMovieAudio(ALuint limit, int out)
    {
        ALshort *sampleBuffer = audioBuffer;
        ALuint taken = 0;
        float left[256], right[256];
        int weights = 0;
        while (audioQueueHead && taken < limit) {
            volatile AudioQueue *head = audioQueueHead;
            const int channels = head->audio->channels;
            const float *sourceSamples = head->audio->samples + (size_t) head->offset * channels;
            ALuint frames = head->audio->frames - head->offset;
            if (frames > limit - taken) frames = limit - taken;

            if (channels == out) {
                for (ALuint i = 0; i < frames * (ALuint) channels; i++)
                    *(sampleBuffer++) = movieSample(*(sourceSamples++));
            } else {
                if (weights != channels) {
                    movieDownmixWeights(channels, left, right);
                    weights = channels;
                }
                for (ALuint i = 0; i < frames; i++, sourceSamples += channels) {
                    float l = 0.0f, r = 0.0f;
                    for (int c = 0; c < channels; c++) {
                        l += sourceSamples[c] * left[c];
                        r += sourceSamples[c] * right[c];
                    }
                    if (out == 1) {
                        *(sampleBuffer++) = movieSample((l + r) * 0.5f);
                    } else {
                        *(sampleBuffer++) = movieSample(l);
                        *(sampleBuffer++) = movieSample(r);
                    }
                }
            }

            head->offset += frames;
            taken += frames;
            if (head->offset >= head->audio->frames) {
                audioQueueHead = head->next;
                THEORAPLAY_freeAudio(head->audio);
                free((void *) head);
            }
        }
        if (!audioQueueHead) audioQueueTail = NULL;
        pendingFrames -= taken;
        inFlightFrames += taken;
        return taken;
    }

    void streamMovieAudio(){
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        /* A refill worker sets its own priority, as ALStream::streamData
         * does: SDL starts it at its creator's, the rgss thread's. */
        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif
        /* Each pass takes back the buffers AL has played and refills them:
         * whole buffers while enough audio waits, a short one only when AL
         * is about to run dry (which also plays the clip's tail). A source
         * that ran dry is restarted and counted. */
        ALuint freeBufs[MOVIE_AUDIO_BUFS];
        ALint freeCount = MOVIE_AUDIO_BUFS;
        memcpy(freeBufs, alBuffers, sizeof(freeBufs));
        /* What reaches AL: mono stays mono, anything wider is stereo. */
        int outChannels = 0;
        int sampleRate = 0;
        ALuint bufferFrames = 0;

        while (!audioThreadTermReq) {
            ALint processed = 0;
            alGetSourcei(audioSource, AL_BUFFERS_PROCESSED, &processed);
            if (processed > 0) {
                alSourceUnqueueBuffers(audioSource, processed, freeBufs + freeCount);
                freeCount += processed;
                uint64_t retired = 0;
                for (ALint i = 0; i < processed && queuedCount; ++i) {
                    retired += queuedFrames[0];
                    for (unsigned j = 1; j < queuedCount; ++j)
                        queuedFrames[j - 1] = queuedFrames[j];
                    --queuedCount;
                }
                playedFrames += retired;
                SDL_LockMutex(audioMutex);
                inFlightFrames -= std::min<uint64_t>(retired, inFlightFrames);
                SDL_UnlockMutex(audioMutex);
            }
            publishAudioClock();

            while (freeCount > 0) {
                ALuint frames = 0;
                SDL_LockMutex(audioMutex);
                if (!bufferFrames && audioQueueHead) {
                    outChannels = audioQueueHead->audio->channels == 1 ? 1 : 2;
                    sampleRate = audioQueueHead->audio->freq;
                    clockFreq = sampleRate;
                    clockBaseMs = audioQueueHead->audio->playms;
                    bufferFrames = std::max<ALuint>(1, std::min<ALuint>(
                        (ALuint) (sampleRate * MOVIE_AUDIO_BUFFER_MS / 1000),
                        MOVIE_AUDIO_BUFFER_SIZE / outChannels));
                }
                const bool low = MOVIE_AUDIO_BUFS - freeCount < 2;
                if (bufferFrames && (pendingFrames >= bufferFrames || (low && pendingFrames)))
                    frames = takeMovieAudio(bufferFrames, outChannels);
                SDL_UnlockMutex(audioMutex);
                if (!frames) break;

                const ALuint buffer = freeBufs[--freeCount];
                alBufferData(buffer, outChannels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, audioBuffer,
                    frames * outChannels * sizeof(ALshort), sampleRate);
                alSourceQueueBuffers(audioSource, 1, &buffer);
                queuedFrames[queuedCount++] = frames;
            }

            ALint state = 0;
            alGetSourcei(audioSource, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING && freeCount < MOVIE_AUDIO_BUFS) {
                if (state == AL_STOPPED)
                    ++audioUnderruns;
                alSourcePlay(audioSource);
            }
            SDL_Delay(AUDIO_SLEEP);
        }
    }
    
    /* Every step is checked: Vita SDL treats locking a
     * NULL mutex as success, so an unchecked failure left the queue
     * unsynchronised. The AL names are checked, not alGetError(), whose
     * slot the BGM stream threads share. On false the caller closes what
     * was made. */
    bool startAudio(float volume)
    {
        audioMutex = SDL_CreateMutex();
        if (!audioMutex)
            return false;
        audioSource = 0;
        alGenSources(1, &audioSource);
        if (!audioSource || !alIsSource(audioSource))
            return false;
        memset(alBuffers, 0, sizeof(alBuffers));
        alGenBuffers(MOVIE_AUDIO_BUFS, alBuffers);
        for (ALuint buffer : alBuffers)
            if (!buffer || !alIsBuffer(buffer)) {
                /* A partial alGenBuffers still made the earlier names. */
                alDeleteSources(1, &audioSource);
                for (ALuint made : alBuffers)
                    if (made && alIsBuffer(made))
                        alDeleteBuffers(1, &made);
                memset(alBuffers, 0, sizeof(alBuffers));
                return false;
            }
        audioStarted = true;
        alSourcef(audioSource, AL_GAIN, volume);

        audioThreadTermReq.clear();
        queueAudioPacket(audio);
        audio = NULL;
        bufferMovieAudio(decoder, 0);
        audioThread = createSDLThread <Movie, &Movie::streamMovieAudio>(this, "movieaudio");

        return audioThread != 0;
    }

    /* Stops the audio thread and releases whatever startAudio made. */
    void closeAudio()
    {
        audioThreadTermReq.set();
        if (audioThread) {
            SDL_WaitThread(audioThread, 0);
            audioThread = 0;
        }
        if (audioStarted) {
            alSourceStop(audioSource);
            alDeleteSources(1, &audioSource);
            alDeleteBuffers(MOVIE_AUDIO_BUFS, alBuffers);
            audioStarted = false;
        }
        /* Free every queued packet once (the stock teardown freed the head
         * and tail, twice when they were one packet, and leaked the rest). */
        while (audioQueueHead) {
            volatile AudioQueue *next = audioQueueHead->next;
            THEORAPLAY_freeAudio(audioQueueHead->audio);
            free((void *) audioQueueHead);
            audioQueueHead = next;
        }
        audioQueueTail = NULL;
        pendingFrames = inFlightFrames = 0;
        if (audioMutex) {
            SDL_DestroyMutex(audioMutex);
            audioMutex = 0;
        }
    }

    /* Re-specify the plane texture in place with this frame (the same-size
     * whole-level path); a failure keeps the previous frame on screen. */
    bool uploadPlanes(const THEORAPLAY_VideoFrame *frame)
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileUpload(FrameProfile::Upload,
                                          (uint64_t) planeSize.x * (uint64_t) planeSize.y * 4u);
#endif
        TEX::bind(planeTex);
        const bool uploaded = TEX::uploadImageChecked(planeSize.x, planeSize.y, frame->pixels, GL_RGBA);
        planeReady = planeReady || uploaded;
        return uploaded;
    }

    void play(float volume)
    {
        const Uint32 baseTicks = SDL_GetTicks();
        /* The movie clock is wall time plus this offset.
         * With audio it holds the first frame until the AL source is
         * consuming samples, then follows the audio clock: video that leads
         * it by more than a frame waits, video that lags it by more drops
         * frames, and within a frame it runs on wall time, so the position's
         * mixer-period steps do not jitter the pacing. While the audio clock
         * is not live (never started, underrun, ended) the picture runs on
         * wall time; a clip without audio is wall time from the start. */
        const Sint32 frameMs = videoFps >= 1.0f ? (Sint32) (1000.0f / videoFps) : 33;
        const Sint32 firstMs = video ? (Sint32) video->playms : 0;
        bool clockStarted = !hasAudio;
        Sint32 clockOffset = 0;
        Sint32 audioStartMs = -1;
        bool openedAudio = false;
        bool interrupted = false;
        unsigned framesShown = 0;
        unsigned framesDropped = 0;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        unsigned framesChanged = 0;
        unsigned uploadRetries = 0;
        uint32_t lastPixelsHash = 0;
        bool havePixelsHash = false;
        double uploadUS = 0;
        unsigned driftFrames = 0;
        uint64_t driftSum = 0;
        Uint32 driftMax = 0;
        unsigned resyncs = 0;
        unsigned preAudio = 0;
#endif
#ifdef __vita__
        const SceUID rgssThread = sceKernelGetThreadId();
        MovieThreadSample rgss, worker;
        rgss.sample(rgssThread);
        const uint64_t rgssRunStart = rgss.runClocks;
#endif
        /* A frame held for later is still owed once the decoder is done. */
        while (THEORAPLAY_isDecoding(decoder) || video) {
            // Check for reset/shutdown input, or an attempted skip
            if (leaveRequested()) {
                interrupted = true;
                break;
            }

            if (!video) {
                video = THEORAPLAY_getVideo(decoder);
            }

            /* Only until audio starts: a packet taken here afterwards was
             * never queued (the stock loop lost one that way). */
            if (hasAudio && !openedAudio) {
                if (!audio) {
                    audio = THEORAPLAY_getAudio(decoder);
                }

                if (audio) {
                    if (startAudio(volume)) {
                        openedAudio = true;
                    } else {
                        /* Play on without sound. */
                        Debug() << "Error opening movie audio! Playing it without sound";
                        closeAudio();
                        hasAudio = false;
                        audioDiscard = true;
                    }
                }

            }
            if (audioDiscard)
                discardMovieAudio(decoder);

            const Uint32 ticks = SDL_GetTicks();
            const Sint32 wall = (Sint32) (ticks - baseTicks);
            Sint32 audioNow = 0;
            const bool audioLive = openedAudio && audioClock(ticks, audioNow);
            if (audioLive && audioStartMs < 0)
                audioStartMs = wall;
            if (!clockStarted) {
                /* No packet even after the prepare wait: start video-only;
                 * audio that opens later is caught up with below. */
                clockOffset = audioLive ? audioNow - wall : firstMs - wall;
                clockStarted = audioLive || !openedAudio
                               || wall >= MOVIE_AUDIO_START_TIMEOUT_MS;
            } else if (audioLive) {
                const Sint32 lead = wall + clockOffset - audioNow;
                if (lead > frameMs || lead < -frameMs) {
                    clockOffset = audioNow - wall;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                    ++resyncs;
#endif
                }
            }
            const Sint32 now = wall + clockOffset;

            if (video && ((Sint32) video->playms <= now)) {
                /* Present the newest frame that is due: a frame whose
                 * successor is due too is dropped unshown, so a slow present
                 * costs frames, not speed (a decoder slower than real time
                 * still lags; decode_ms shows it). */
                const THEORAPLAY_VideoFrame *next;
                while ((next = THEORAPLAY_getVideo(decoder)) != NULL && (Sint32) next->playms <= now)
                {
                    THEORAPLAY_freeVideo(video);
                    ++framesDropped;
                    video = next;
                }

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                const double uploadStart = vita_glue_frame_profile_now_us();
                if (!uploadPlanes(video))
                    ++uploadRetries;
                uploadUS += vita_glue_frame_profile_now_us() - uploadStart;
                {
                    const uint32_t pixelsHash = movieFrameHash(video);
                    if (!havePixelsHash || pixelsHash != lastPixelsHash)
                        ++framesChanged;
                    lastPixelsHash = pixelsHash;
                    havePixelsHash = true;
                }
                if (openedAudio && audioStartMs < 0)
                    ++preAudio;
                if (audioLive)
                {
                    const Sint32 drift = (Sint32) video->playms - audioNow;
                    const Uint32 magnitude = drift < 0 ? (Uint32) -drift : (Uint32) drift;
                    driftSum += magnitude;
                    driftMax = std::max(driftMax, magnitude);
                    ++driftFrames;
                }
#else
                uploadPlanes(video);
#endif
#ifdef __vita__
                rgss.sample(rgssThread);
                if (const int workerThread = source.worker.load())
                    worker.sample(workerThread);
#endif
                ++framesShown;
                shState->graphics().update(false);
                THEORAPLAY_freeVideo(video);
                video = next;

            } else {
                // Sleep until the held frame is due, or poll the decoder.
                Sint32 wait = MOVIE_DECODER_POLL_MS;
                if (video)
                    wait = std::min<Sint32>((Sint32) video->playms - now, VIDEO_DELAY);
                SDL_Delay((Uint32) wait);
            }

            if (openedAudio) {
                bufferMovieAudio(decoder, now > 0 ? (Uint32) now : 0);
            }
        }
        const Uint32 elapsed = SDL_GetTicks() - baseTicks;

        /* The picture has ended; the sound already decoded plays out, on
         * the same clock, bounded (a short video track
         * cut the audio still queued in AL and in the packet queue). */
        const Uint32 tailStart = SDL_GetTicks();
        if (openedAudio && !interrupted) {
            while (audioPending() && SDL_GetTicks() - tailStart < MOVIE_AUDIO_TAIL_MAX_MS) {
                if (leaveRequested())
                    break;
                const Sint32 now = (Sint32) (SDL_GetTicks() - baseTicks) + clockOffset;
                bufferMovieAudio(decoder, now > 0 ? (Uint32) now : 0);
                SDL_Delay(VIDEO_DELAY);
            }
        }
        const Uint32 tailMs = SDL_GetTicks() - tailStart;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        /* One line per movie: dimensions, frames shown
         * vs dropped, presented rate, A/V drift (mean/max over frames shown
         * while the audio clock ran; -1 without audio), when the audio
         * clock first ran, the frames shown before it did, how often video
         * was re-aligned to it and how often the AL source ran dry, and the
         * rgss-side upload cost; on Vita also where the decode worker ran. */
        {
            char line[480];
            int at = snprintf(line, sizeof(line),
                     "vita-movie: %dx%d fps=%.2f shown=%u dropped=%u "
                     "changed=%u retry=%u ms=%u fps_presented=%.1f "
                     "av_drift_ms=%d av_drift_max_ms=%d av_start_ms=%d pre_audio=%u "
                     "av_resyncs=%u av_underruns=%u upload_ms=%.2f av_tail_ms=%u",
                     videoW, videoH, videoFps, framesShown, framesDropped,
                     framesChanged, uploadRetries, (unsigned) elapsed,
                     elapsed ? framesShown * 1000.0 / elapsed : 0.0,
                     driftFrames ? (int) (driftSum / driftFrames) : -1,
                     driftFrames ? (int) driftMax : -1, (int) audioStartMs, preAudio, resyncs,
                     audioUnderruns.load(), framesShown ? uploadUS / 1000.0 / framesShown : 0.0,
                     (unsigned) tailMs);
#ifdef __vita__
            rgss.sample(rgssThread);
            if (const int workerThread = source.worker.load())
                worker.sample(workerThread);
            const unsigned decoded = framesShown + framesDropped;
            if (at > 0 && at < (int) sizeof(line))
                snprintf(line + at, sizeof(line) - at,
                         " decode_ms=%.2f worker_pri=%d worker_pin=%x/%x worker_cpus=%x "
                         "worker_moves=%u worker_run_ms=%u rgss_pri=%d rgss_cpus=%x rgss_run_ms=%u",
                         decoded ? worker.runClocks / 1000.0 / decoded : 0.0,
                         worker.priority, (unsigned) source.cpuMask >> 16, (unsigned) source.pinResult,
                         worker.cpus, worker.moves, (unsigned) (worker.runClocks / 1000u),
                         rgss.priority, rgss.cpus,
                         (unsigned) ((rgss.runClocks - rgssRunStart) / 1000u));
#else
            (void) at;
#endif
            vita_glue_trace(line);
        }
#else
        (void) elapsed;
        (void) tailMs;
#endif
    }

    /* Reset/shutdown, or a skip when the movie allows one. */
    bool leaveRequested()
    {
        if (shState->graphics().updateMovieInput(this))
            return true;
        if (!skippable)
            return false;
        shState->input().update();
        return shState->input().isTriggered(Input::C) || shState->input().isTriggered(Input::B);
    }

    ~Movie()
    {
        /* Stop the audio thread before tearing down what it uses. */
        closeAudio();
        if (video) THEORAPLAY_freeVideo(video);
        if (audio) THEORAPLAY_freeAudio(audio);
        if (decoder) THEORAPLAY_stopDecode(decoder);
        if (planeTex.gl) TEX::del(planeTex);
    }
};

/* Shows the movie: the current frame's planes through the boot-reserved
 * MovieYuvShader, where the stock player drew an RGBA Bitmap sprite. */
class MovieFrame : public ViewportElement
{
public:
    MovieFrame(Movie &movie, const FloatRect &dst)
    : ViewportElement(0, 5001), movie(movie)
    {
        quad.setTexPosRect(FloatRect(0, 0, movie.videoW, movie.videoH), dst);
    }

    void aboutToAccess() const {}

protected:
    void draw()
    {
        if (!movie.planeReady)
            return;

        MovieYuvShader &shader = shState->shaders().movieYuv;
        shader.bind();
        shader.applyViewportProj();
        shader.setTexSize(Vec2i(1, 1));
        shader.setTranslation(Vec2i());
        shader.setPlanes(movie.planeSize, movie.videoH, movie.planeSize.x / 2,
                         Vec2i(movie.videoW / 2 - 1, movie.videoH / 2 - 1));
        TEX::bind(movie.planeTex);
        quad.draw();
    }

private:
    Movie &movie;
    Quad quad;
};

struct MovieOpenHandler : FileSystem::OpenHandler
{
    SDL_RWops *srcOps;
    
    MovieOpenHandler(SDL_RWops &srcOps)
    :   srcOps(&srcOps)
    {}
    
    bool tryRead(SDL_RWops &ops, const char *ext)
    {
        *srcOps = ops;
        return true;
    }
};

/* Per-frame render-target switch accounting. The bound-FBO
 * shadow in gl-util.h is the one choke point every bind passes through.
 * Counters reset at each present path's entry and log once per frame
 * through the gpu-telemetry marker; a bind outside that window (freeze,
 * snapshot) is discarded by the next reset. */
static unsigned frameTargetSwitches = 0;
static unsigned frameGrayEffects = 0;
static unsigned frameBatchedEffects = 0;

static void noteTargetSwitch(FBO::ID before)
{
	if (FBO::boundFramebufferID != before)
		++frameTargetSwitches;
}

static void resetTargetCounters()
{
	frameTargetSwitches = frameGrayEffects = frameBatchedEffects = 0;
}

#ifdef MKXPZ_SOFTWARE_BITMAPS
static void logTargetCounters()
{
	if (!GPUBudget::telemetryEnabled())
		return;

	char line[96];
	snprintf(line, sizeof(line), "targets n=%u gray=%u batched=%u",
	         frameTargetSwitches, frameGrayEffects, frameBatchedEffects);
	GPUBudget::breadcrumb("frame", line);
}
#else
static void logTargetCounters() {}
#endif

/* Bounding-box helpers for the gray-effect batching decision. */
static bool rectsIntersect(const IntRect &a, const IntRect &b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

static IntRect rectsUnion(const IntRect &a, const IntRect &b)
{
	const int left = std::min(a.x, b.x);
	const int top = std::min(a.y, b.y);
	return IntRect(left, top,
	               std::max(a.x + a.w, b.x + b.w) - left,
	               std::max(a.y + a.h, b.y + b.h) - top);
}

/* Defined in scene.cpp: bumped once per element actually drawn. */
unsigned long long sceneDrawEpoch();

struct PingPong {
#ifndef MKXPZ_SOFTWARE_BITMAPS
    /* Stock: PingPong owns its two render targets. */
    TEXFBO rtOwned[2];
#endif
    /* Under the bounded-GPU backend these point INTO
     * GPUBudget's fixed-surface registry instead of copying out of it. A
     * copy went stale the moment reallocChecked re-specified the registry's
     * entry -- cached width/height that no longer matched the texture -- and
     * ~PingPong then fini()d surfaces the registry still believed in. */
    TEXFBO *rt[2];
    uint8_t srcInd, dstInd;
    int screenW, screenH;

    PingPong(int screenW, int screenH)
    : srcInd(0), dstInd(1), screenW(screenW), screenH(screenH) {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Reservation uses the same boot dimensions as GraphicsPrivate,
         * so these checks ordinarily leave attachment storage untouched. */
        rt[0] = &GPUBudget::reservedMutable(GPUBudget::PingPong0);
        rt[1] = &GPUBudget::reservedMutable(GPUBudget::PingPong1);

        for (int i = 0; i < 2; ++i)
            TEXFBO::reallocChecked(*rt[i], screenW, screenH,
                                   i == 0 ? "ping-pong 0" : "ping-pong 1");
#else
        for (int i = 0; i < 2; ++i) {
            rt[i] = &rtOwned[i];
            TEXFBO::init(*rt[i]);
            TEXFBO::allocEmpty(*rt[i], screenW, screenH);
            TEXFBO::linkFBO(*rt[i]);
            gl.ClearColor(0, 0, 0, 1);
            FBO::clear();
        }
#endif
    }

    ~PingPong() {
        /* Under the bounded-GPU backend TEXFBO::fini() refuses to delete a
         * reserved surface; this loop is the stock path's teardown. */
        for (int i = 0; i < 2; ++i)
            TEXFBO::fini(*rt[i]);
    }

    TEXFBO &backBuffer() { return *rt[srcInd]; }

    TEXFBO &frontBuffer() { return *rt[dstInd]; }

    /* Better not call this during render cycles */
    void resize(int width, int height) {
        screenW = width;
        screenH = height;

#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* resizeScreen has already resized all reserved targets together. */
        for (int i = 0; i < 2; ++i)
            TEXFBO::reallocChecked(*rt[i], width, height,
                                   i == 0 ? "ping-pong 0" : "ping-pong 1");
#else
        for (int i = 0; i < 2; ++i)
            TEXFBO::allocEmpty(*rt[i], width, height);
#endif
    }

    void startRender() { bind(); }

    void swapRender() {
        std::swap(srcInd, dstInd);

        bind();
    }

    void clearBuffers() {
        glState.clearColor.pushSet(Vec4(0, 0, 0, 1));

        for (int i = 0; i < 2; ++i) {
            FBO::bind(rt[i]->fbo);
            FBO::clear();
        }

        glState.clearColor.pop();
    }

private:
    void bind() {
        FBO::ID before = FBO::boundFramebufferID;
        FBO::bind(rt[dstInd]->fbo);
        noteTargetSwitch(before);
    }
};

static void requireRenderTargets()
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    if (!GPUBudget::fixedSurfacesValid())
        throw Exception(Exception::MKXPError,
                        "Render targets unavailable after failed resize; retry Graphics.resize_screen");
#endif
}

class ScreenScene : public Scene {
public:
    ScreenScene(int width, int height) : pp(width, height) {
        updateReso(width, height);
        
        brightEffect = false;
        brightnessQuad.setColor(Vec4());

        effectEpoch = 0;
        effectEpochValid = false;
        effectStaleValid = false;
    }
    
    void composite() {
        requireRenderTargets();
        GLStateGuard state(glState);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileScene(FrameProfile::Scene);
#endif
        const int w = geometry.rect.w;
        const int h = geometry.rect.h;

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
        {
            char tb[160];
            snprintf(tb, sizeof(tb), "trace: ScreenScene::composite %dx%d", w, h);
            vita_glue_trace(tb);
        }
#endif
        {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::OperationScope profilePrepare(FrameProfile::OtherPrepare);
#endif
            shState->prepareDraw();
        }

        pp.startRender();

        glState.viewport.set(IntRect(0, 0, w, h));

        FBO::clear();

        /* No effect run is in progress at the start of a composition, so
         * the first gray-toned viewport always takes the stock swap path
         */
        effectEpochValid = false;
        effectStaleValid = false;

        Scene::composite();
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
        vita_glue_trace("trace: ScreenScene::composite scene done");
#endif

        if (brightEffect) {
            SimpleColorShader &shader = shState->shaders().simpleColor;
            shader.bind();
            shader.applyViewportProj();
            shader.setTranslation(Vec2i());
            
            brightnessQuad.draw();
        }
        state.release();
    }
    
    void requestViewportRender(const Vec4 &c, const Vec4 &f, const Vec4 &t) {
        const IntRect &viewpRect = glState.scissorBox.get();
        const IntRect &screenRect = geometry.rect;
        
        const bool toneRGBEffect = t.xyzNotNull();
        const bool toneGrayEffect = t.w != 0;
        const bool colorEffect = c.w > 0;
        const bool flashEffect = f.w > 0;
        
        if (toneGrayEffect) {
            /* The pass reads one buffer and writes the other, so stock
             * swaps the ping-pong for every toned viewport.
             * When no element has drawn since the previous effect pass and
             * this viewport's rect misses every rect the other buffer is
             * stale in, the same quad into the current target reads the
             * same pixels and saves the swap bind plus the full-screen
             * copy blit. A full-screen rect can never batch: it overlaps
             * any stale region, and an in-place pass is illegal. */
            const bool batch = effectEpochValid && effectStaleValid &&
                               effectEpoch == sceneDrawEpoch() &&
                               !rectsIntersect(effectStaleRect, viewpRect);

            if (batch) {
                ++frameBatchedEffects;
            } else {
                pp.swapRender();

                if (!viewpRect.encloses(screenRect)) {
                    /* Scissor test _does_ affect FBO blit operations,
                     * and since we're inside the draw cycle, it will
                     * be turned on, so turn it off temporarily */
                    glState.scissorTest.pushSet(false);

                    int scaleIsSpecial = GLMeta::blitScaleIsSpecial(pp.frontBuffer(), false, geometry.rect, pp.backBuffer(), geometry.rect);

                    GLMeta::blitBegin(pp.frontBuffer(), false, scaleIsSpecial);
                    GLMeta::blitSource(pp.backBuffer(), scaleIsSpecial);
                    GLMeta::blitRectangle(geometry.rect, Vec2i());
                    GLMeta::blitEnd();

                    glState.scissorTest.pop();
                }

                effectEpoch = sceneDrawEpoch();
                effectEpochValid = true;
                effectStaleRect = viewpRect;
                effectStaleValid = true;
            }

            ++frameGrayEffects;

            GrayShader &shader = shState->shaders().gray;
            shader.bind();
            shader.setGray(t.w);
            shader.applyViewportProj();
            shader.setTexSize(screenRect.size());
            
            TEX::bind(pp.backBuffer().tex);
            
            glState.blend.pushSet(false);
            screenQuad.draw();
            glState.blend.pop();
        }
        
        if (!toneRGBEffect && !colorEffect && !flashEffect)
            return;
        
        FlatColorShader &shader = shState->shaders().flatColor;
        shader.bind();
        shader.applyViewportProj();
        
        if (toneRGBEffect) {
            /* First split up additive / substractive components */
            Vec4 add, sub;
            
            if (t.x > 0)
                add.x = t.x;
            if (t.y > 0)
                add.y = t.y;
            if (t.z > 0)
                add.z = t.z;
            
            if (t.x < 0)
                sub.x = -t.x;
            if (t.y < 0)
                sub.y = -t.y;
            if (t.z < 0)
                sub.z = -t.z;
            
            /* Then apply them using hardware blending */
            gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            
            if (add.xyzNotNull()) {
                gl.BlendEquation(GL_FUNC_ADD);
                shader.setColor(add);
                
                screenQuad.draw();
            }
            
            if (sub.xyzNotNull()) {
                gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                shader.setColor(sub);
                
                screenQuad.draw();
            }
        }
        
        if (colorEffect || flashEffect) {
            gl.BlendEquation(GL_FUNC_ADD);
            gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                 GL_ONE);
        }
        
        if (colorEffect) {
            shader.setColor(c);
            screenQuad.draw();
        }
        
        if (flashEffect) {
            shader.setColor(f);
            screenQuad.draw();
        }

        /* Every pass above wrote the viewport rect into the current
         * target, which grows the region the other buffer is stale in
         */
        if (effectStaleValid)
            effectStaleRect = rectsUnion(effectStaleRect, viewpRect);

        glState.blendMode.refresh();
    }
    
    void setBrightness(float norm) {
        brightnessQuad.setColor(Vec4(0, 0, 0, 1.0f - norm));
        
        brightEffect = norm < 1.0f;
    }
    
    void updateReso(int width, int height) {
        geometry.rect.w = width;
        geometry.rect.h = height;
        
        screenQuad.setTexPosRect(geometry.rect, geometry.rect);
        brightnessQuad.setTexPosRect(geometry.rect, geometry.rect);
        
        notifyGeometryChange();
    }
    
    void setResolution(int width, int height) {
        pp.resize(width, height);
        updateReso(width, height);
    }
    
    PingPong &getPP() { return pp; }
    
private:
    PingPong pp;
    Quad screenQuad;
    
    Quad brightnessQuad;
    bool brightEffect;

    /* Gray-effect batching state: whether the other
     * ping-pong buffer still backs the batched read (stale rect), and
     * the draw epoch of the last swap, invalidated by any later draw. */
    unsigned long long effectEpoch;
    bool effectEpochValid;
    IntRect effectStaleRect;
    bool effectStaleValid;
};

/* Nanoseconds per second */
#define NS_PER_S 1000000000

struct FPSLimiter {
    uint64_t lastTickCount;
    
    /* ticks per frame */
    int64_t tpf;
    
    /* Ticks per second */
    const uint64_t tickFreq;
    
    /* Ticks per milisecond */
    const uint64_t tickFreqMS;
    
    /* Ticks per nanosecond */
    const double tickFreqNS;
    
    bool disabled;
    
    /* Data for frame timing adjustment */
    struct {
        /* Last tick count */
        uint64_t last;
        
        /* How far behind/in front we are for ideal frame timing */
        int64_t idealDiff;
        
        bool resetFlag;
    } adj;
    
    FPSLimiter(uint16_t desiredFPS)
    : lastTickCount(SDL_GetPerformanceCounter()),
    tickFreq(SDL_GetPerformanceFrequency()), tickFreqMS(tickFreq / 1000),
    tickFreqNS((double)tickFreq / NS_PER_S), disabled(false) {
        setDesiredFPS(desiredFPS);
        
        adj.last = SDL_GetPerformanceCounter();
        adj.idealDiff = 0;
        adj.resetFlag = false;
    }
    
    void setDesiredFPS(uint16_t value) { tpf = tickFreq / value; }
    
    void delay() {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        const int64_t measuredDebt = adj.idealDiff;
        const unsigned measuredReset = adj.resetFlag;
        if (vita_measure_mode && disabled)
            vita_measure_delay(measuredDebt, measuredDebt, 0, tpf, tickFreq, 2);
#endif
        if (disabled)
            return;
        
        int64_t tickDelta = SDL_GetPerformanceCounter() - lastTickCount;
        int64_t toDelay = tpf - tickDelta;
        
        /* Compensate for the last delta
         * to the ideal timestep */
        toDelay -= adj.idealDiff;
        
        if (toDelay < 0)
            toDelay = 0;
        
        delayTicks(toDelay);
        
        uint64_t now = lastTickCount = SDL_GetPerformanceCounter();
        int64_t diff = now - adj.last;
        adj.last = now;
        
        /* Recalculate our temporal position
         * relative to the ideal timestep */
        adj.idealDiff = diff - tpf + adj.idealDiff;
        
        if (adj.resetFlag) {
            adj.idealDiff = 0;
            adj.resetFlag = false;
        }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (vita_measure_mode)
            vita_measure_delay(measuredDebt, adj.idealDiff, diff, tpf, tickFreq, measuredReset);
#endif
    }
    
    void resetFrameAdjust() {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (vita_measure_mode) vita_measure_reset();
#endif
        adj.resetFlag = true;
    }
    
    /* If we're more than a full frame's worth
     * of ticks behind the ideal timestep,
     * there's no choice but to skip frame(s)
     * to catch up */
    bool frameSkipRequired() const {
        if (disabled)
            return false;
        
        return adj.idealDiff > tpf;
    }
    
private:
    void delayTicks(uint64_t ticks) {
#if defined(HAVE_NANOSLEEP)
        struct timespec req;
        uint64_t nsec = ticks / tickFreqNS;
        req.tv_sec = nsec / NS_PER_S;
        req.tv_nsec = nsec % NS_PER_S;
        errno = 0;
        
        while (nanosleep(&req, &req) == -1) {
            int err = errno;
            errno = 0;
            
            if (err == EINTR)
                continue;
            
            Debug() << "nanosleep failed. errno:" << err;
            SDL_Delay(ticks / tickFreqMS);
            break;
        }
#else
        SDL_Delay(ticks / tickFreqMS);
#endif
    }
};

/* Boot gate for the diagnostic swap-capture observer.
 *
 * The observer's own marker (`swap-capture`) is written by the diagnostic
 * scene during play, so it cannot be read at boot. What can be decided once
 * is whether a diagnostic payload is installed at all: product packages ship
 * neither a custom script nor an app0:/diagnostics directory, so an ordinary
 * transition then resolves no PhysFS path, builds no string and opens no file
 * Set by Graphics::Graphics on the RGSS thread before the
 * first freeze; read by the RGSS thread only. */
static bool transitionCaptureDiagnostics = false;

static bool diagnosticPayloadInstalled(const std::string &customScript)
{
    if (!customScript.empty()) return true;
    DIR *dir = opendir("app0:/diagnostics");
    if (!dir) return false;
    closedir(dir);
    return true;
}

// Diagnostic only: arm once per transition, never poll the filesystem per swap.
struct TransitionSwapCapture {
    std::string directory;
    unsigned frame = 0;
    int probeFrame = -1;
    bool auxiliary = false;
    size_t capacity = 0;
    std::unique_ptr<unsigned char[]> pixels;

    explicit TransitionSwapCapture(const char *filename) {
        if (!transitionCaptureDiagnostics) return;
        if (!*filename) return;
        const char *real = PHYSFS_getRealDir(filename);
        const char *mount = real ? PHYSFS_getMountPoint(real) : 0;
        if (!mount) return;
        std::string relative(filename), prefix(mount);
        if (prefix == "/") prefix.clear();
        if (relative.compare(0, prefix.size(), prefix) != 0) return;
        relative.erase(0, prefix.size());
        const size_t slash = relative.find_last_of('/');
        std::string path = std::string(real) + "/";
        if (slash != std::string::npos) path += relative.substr(0, slash + 1);
        FILE *marker = std::fopen((path + "swap-capture").c_str(), "r");
        if (!marker) return;
        char name[80] = {};
        const bool armed = std::fscanf(marker, "%79s", name) == 1 &&
                           std::string(name) == "transition_mask";
        std::fclose(marker);
        if (armed) directory = path;
        if (!armed) return;
        marker = std::fopen((path + "transition-probe").c_str(), "rb");
        if (!marker) return;
        char selection[4] = {};
        const size_t count = std::fread(selection, 1, sizeof(selection), marker);
        const bool ok = !std::ferror(marker) && count == 3 && selection[0] == '0' &&
                        (selection[1] == '5' || selection[1] == '6') && selection[2] == '\n';
        std::fclose(marker);
        if (ok) probeFrame = selection[1] - '0';
        else std::fputs("transition-probe: FAIL invalid marker\n", stderr);
    }

    bool write(SDL_Window *window) {
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(window, &width, &height);
        // Bound scratch to 16 MiB; the Vita drawable needs about 2 MiB.
        if (width <= 0 || height <= 0 || width > 2048 || height > 2048)
            return false;
        GLint bound = 0, pack = 0;
        gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        if (bound) return false;
        gl.GetIntegerv(GL_PACK_ALIGNMENT, &pack);
        const size_t stride = static_cast<size_t>(width) * 4;
        if (capacity < stride * height) {
            pixels.reset(new (std::nothrow) unsigned char[stride * height]());
            capacity = pixels ? stride * height : 0;
        }
        if (!pixels) return false;
        std::fill(pixels.get(), pixels.get() + stride * height, 0);
        gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());
        gl.PixelStorei(GL_PACK_ALIGNMENT, pack);
        if (gl.GetError() != GL_NO_ERROR) return false;
        char name[40];
        std::snprintf(name, sizeof(name), auxiliary ? "transition_probe_%02u" : "transition_mask_%02u",
                      auxiliary ? frame - 1 : frame);
        const std::string rgba = directory + name + ".rgba";
        const std::string size = directory + name + ".size";
        FILE *out = std::fopen(rgba.c_str(), "wb");
        if (!out) return false;
        bool ok = true;
        for (int y = height - 1; y >= 0 && ok; --y)
            ok = std::fwrite(pixels.get() + static_cast<size_t>(y) * stride, 1, stride, out) == stride;
        if (std::fclose(out)) ok = false;
        if (ok) {
            out = std::fopen(size.c_str(), "w");
            if (!out) ok = false;
            else {
                ok = std::fprintf(out, "%d %d\n", width, height) >= 0;
                if (std::fclose(out)) ok = false;
            }
        }
        if (!ok) {
            std::remove(rgba.c_str());
            std::remove(size.c_str());
        }
        return ok;
    }

    void observe(SDL_Window *window) {
        if (directory.empty() || frame >= 8) return;
        if (!write(window)) {
            std::fputs("reference-pixels: FAIL native transition swap capture\n", stderr);
            directory.clear();
        }
        if (!auxiliary) ++frame;
    }

    // Called only between the selected normal swap and the next composition.
    bool presentProbe(SDL_Window *window, TEXFBO &source) {
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        if (w != 960 || h != 544 || source.width != 544 || source.height != 416 || source.selfHires) {
            std::fputs("transition-probe: FAIL geometry\n", stderr);
            return false;
        }
        const GLenum before = gl.GetError();
        if (before != GL_NO_ERROR) {
            std::fprintf(stderr, "transition-probe: FAIL pre GL=%x\n", before);
            return false;
        }
        // GLES2 queries, resolved only for this diagnostic; no new GL resource.
        typedef void (APIENTRY *GetAttrib)(GLuint, GLenum, GLint *);
        typedef void (APIENTRY *GetPointer)(GLuint, GLenum, void **);
        typedef void (APIENTRY *GetUniform)(GLuint, GLint, GLfloat *);
        GetAttrib getAttrib = (GetAttrib) SDL_GL_GetProcAddress("glGetVertexAttribiv");
        GetPointer getPointer = (GetPointer) SDL_GL_GetProcAddress("glGetVertexAttribPointerv");
        GetUniform getUniform = (GetUniform) SDL_GL_GetProcAddress("glGetUniformfv");
        if (!getAttrib || !getPointer || !getUniform) {
            std::fputs("transition-probe: FAIL state queries unavailable\n", stderr);
            return false;
        }
        GLStateGuard state(glState);
        GLint active, texture, array, element, read = 0;
        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &array);
        gl.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element);
        if (gl.BlitFramebuffer) gl.GetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read);
        const GLenum fields[] = {GL_VERTEX_ATTRIB_ARRAY_ENABLED, GL_VERTEX_ATTRIB_ARRAY_SIZE,
            GL_VERTEX_ATTRIB_ARRAY_TYPE, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
            GL_VERTEX_ATTRIB_ARRAY_STRIDE, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING};
        GLint attrs[3][6]; void *pointers[3];
        for (unsigned a = 0; a < 3; ++a) {
            for (unsigned f = 0; f < 6; ++f) getAttrib(a, fields[f], &attrs[a][f]);
            getPointer(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointers[a]);
        }
        SimpleShader &shader = shState->shaders().simple;
        shader.bind();
        const GLuint program = glState.program.get();
        const char *names[] = {"projMat", "texSizeInv", "translation"};
        GLint locations[3]; GLfloat uniforms[3][16] = {};
        for (unsigned u = 0; u < 3; ++u) {
            locations[u] = gl.GetUniformLocation(program, names[u]);
            getUniform(program, locations[u], uniforms[u]);
        }
        GLProperty<Vec2i>::Guard projection(shader.projMat);
        Quad &quad = shState->gpQuad();
        Vertex vertices[4];
        for (unsigned v = 0; v < 4; ++v) vertices[v] = quad.vert[v];
        FBO::unbind();
        glState.scissorTest.set(false);
        glState.blend.set(false);
        glState.clearColor.set(Vec4(0, 0, 0, 1));
        FBO::clear();
        glState.viewport.set(IntRect(0, 0, w, h));
        shader.applyViewportProj();
        shader.setTranslation(Vec2i());
        shader.setTexSize(Vec2i(544, 416));
        TEX::bind(source.tex);
        // Same nearest simple-shader path and positive destination as the port blit.
        quad.setTexPosRect(IntRect(0, 416, 544, -416), IntRect(208, 64, 544, 416));
        quad.draw();
        for (unsigned v = 0; v < 4; ++v) quad.vert[v] = vertices[v];
        quad.markDirty();
        projection.restore();
        gl.UniformMatrix4fv(locations[0], 1, GL_FALSE, uniforms[0]);
        gl.Uniform2f(locations[1], uniforms[1][0], uniforms[1][1]);
        gl.Uniform2f(locations[2], uniforms[2][0], uniforms[2][1]);
        for (unsigned a = 0; a < 3; ++a) {
            VBO::bind(VBO::ID(attrs[a][5]));
            gl.VertexAttribPointer(a, attrs[a][1], attrs[a][2], attrs[a][3], attrs[a][4], pointers[a]);
            if (attrs[a][0]) gl.EnableVertexAttribArray(a);
            else gl.DisableVertexAttribArray(a);
        }
        VBO::bind(VBO::ID(array));
        IBO::bind(IBO::ID(element));
        TEX::bind(TEX::ID(texture));
        gl.ActiveTexture(active);
        if (gl.BlitFramebuffer) gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read);
        state.restore();
        const GLenum after = gl.GetError();
        std::fprintf(stderr, "transition-probe: frame=%02u geometry=208,64,544,416 preGL=0 postGL=%x\n",
                     frame - 1, after);
        return after == GL_NO_ERROR;
    }
};

struct GraphicsPrivate {
    /* Screen resolution, ie. the resolution at which
     * RGSS renders at (settable with Graphics.resize_screen).
     * Can only be changed from within RGSS */
    Vec2i scRes;
    Vec2i scResLores;
    
    /* Screen size, to which the rendered frames are scaled up.
     * This can be smaller than the window size when fixed aspect
     * ratio is enforced */
    Vec2i scSize;
    
    /* Actual physical size of the game window */
    Vec2i winSize;
    
    /* Offset in the game window at which the scaled game screen
     * is blitted inside the game window */
    Vec2i scOffset;
    
    // Scaling factor, used to display the screen properly
    // on Retina displays
    int scalingFactor;
    
    ScreenScene screen;
    RGSSThreadData *threadData;
    SDL_GLContext glCtx;
    
    int frameRate;
    int frameCount;
    int brightness;

    /* Consecutive Graphics.update frames skipped for a refused upload. */
    unsigned uploadSkips = 0;
    static const unsigned uploadSkipLimit = 120;
    
    double last_update;
    
    
    FPSLimiter fpsLimiter;
    
    // Can be set from Ruby. Takes priority over config setting.
    bool useFrameSkip;
    
    bool frozen;
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* References into GPUBudget's registry, which owns
     * these for the life of the process. Copying them made the registry and
     * the engine disagree about a surface's size after every
     * reallocChecked, and made ~GraphicsPrivate delete them. */
    TEXFBO &frozenScene;
#else
    TEXFBO frozenScene;
#endif
    Quad screenQuad;

    float backingScaleFactor;

    Vec2i integerScaleFactor;
#ifdef MKXPZ_SOFTWARE_BITMAPS
    TEXFBO &integerScaleBuffer;
#else
    TEXFBO integerScaleBuffer;
#endif
    bool integerScaleActive;
    bool integerLastMileScaling;

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* ---- The on-screen overlay ---------------------
     * The one texture GPUBudget::ScreenOverlayTexture stands for, taken in
     * the constructor and held for the life of the process, plus the text
     * the consumers own. No pointers, no vectors: one slot per line, so
     * nothing here can allocate or be reallocated after boot, and rewriting
     * one line cannot disturb another's bytes. */
    TEX::ID overlayTex;
    bool overlaySettings = false;
    bool overlayVisible;
    int overlayLineCount;
    char overlayLines[OVERLAY_MAX_LINES][OVERLAY_MAX_CHARS + 1];
    /* The box the last compose painted, always anchored at (0, 0). Every
     * compose writes inside its own box and nowhere else, so clearing this
     * one is exactly enough to erase the previous image -- which is what
     * keeps the CPU cost proportional to the text instead of to all 65 536
     * pixels of the surface. */
    int overlayPaintedW, overlayPaintedH;
    /* The single-writer rule, in code. The texture, the CPU
     * surface behind it, the line slots and overlayVisible are one object,
     * and the only thread that may write them is the one that owns the GL
     * context and took the boot texture -- the thread running this
     * constructor, which reads glCtx on the same line. The owner therefore
     * never changes, so the rule is one comparison and spends no kernel
     * object. */
    SDL_threadID overlayOwner;
    /* Raised with the first refused call: a per-frame offender then costs
     * one comparison and nothing else. */
    bool overlayForeignReported = false;
#endif

    std::vector<double> avgFPSData;
    double last_avg_update;
    SDL_mutex *avgFPSLock;
    
    SDL_mutex *glResourceLock;
    bool multithreadedMode;
    
    /* Global list of all live Disposables
     * (disposed on reset) */
    IntruList<Disposable> dispList;
    
    GraphicsPrivate(RGSSThreadData *rtData)
    :
#ifdef MKXPZ_SOFTWARE_BITMAPS
    scRes(GPUBudget::bootScreenSize(rtData->config, rgssVer, true)),
    scResLores(GPUBudget::bootScreenSize(rtData->config, rgssVer, false)),
#else
    scResLores(DEF_SCREEN_W, DEF_SCREEN_H),
    scRes(rtData->config.enableHires ? (int)lround(rtData->config.framebufferScalingFactor * DEF_SCREEN_W) : DEF_SCREEN_W,
        rtData->config.enableHires ? (int)lround(rtData->config.framebufferScalingFactor * DEF_SCREEN_H) : DEF_SCREEN_H),
#endif
    scSize(scRes),
    winSize(rtData->config.defScreenW, rtData->config.defScreenH),
    screen(scRes.x, scRes.y), threadData(rtData),
    glCtx(SDL_GL_GetCurrentContext()), multithreadedMode(true),
    frameRate(DEF_FRAMERATE), frameCount(0), brightness(255),
    fpsLimiter(frameRate), useFrameSkip(rtData->config.frameSkip), frozen(false),
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* Bound before the body runs: rebuildIntegerScaleBuffer() is called from
     * it. reservedMutable() does not require the slot to have been reserved,
     * which is what lets integerScaleBuffer be bound unconditionally -- an
     * unreserved slot is a zeroed TEXFBO and every read of it is behind
     * integerScaleStepApplicable(). */
    frozenScene(GPUBudget::reservedMutable(GPUBudget::FrozenScene)),
    integerScaleBuffer(GPUBudget::reservedMutable(GPUBudget::IntegerScale)),
#endif
    last_update(0), last_avg_update(0), backingScaleFactor(1), integerScaleFactor(0, 0),
    integerScaleActive(rtData->config.integerScaling.active),
    integerLastMileScaling(rtData->config.integerScaling.lastMileScaling)
#ifdef MKXPZ_SOFTWARE_BITMAPS
    , overlayTex(0), overlayVisible(false), overlayLineCount(0)
    , overlayPaintedW(0), overlayPaintedH(0)
    /* Fixed here, before anything else can reach this object. */
    , overlayOwner(SDL_ThreadID())
#endif
    {
        avgFPSData = std::vector<double>();
        avgFPSLock = SDL_CreateMutex();
        glResourceLock = SDL_CreateMutex();
        
        if (integerScaleActive) {
            integerScaleFactor = Vec2i(0, 0);
            rebuildIntegerScaleBuffer();
        }
        
        recalculateScreenSize(rtData->config.fixedAspectRatio);
        updateScreenResoRatio(rtData);

#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Already reserved at scRes; this is normally a no-op. */
        TEXFBO::reallocChecked(frozenScene, scRes.x, scRes.y, "frozenScene");

        /* The other boot reservation: one texture for the on-screen overlay,
         * taken here because here is still inside boot. */
        reserveOverlayTexture();
#else
        TEXFBO::init(frozenScene);
        TEXFBO::allocEmpty(frozenScene, scRes.x, scRes.y);
        TEXFBO::linkFBO(frozenScene);
#endif

        FloatRect screenRect(0, 0, scRes.x, scRes.y);
        screenQuad.setTexPosRect(screenRect, screenRect);
        
        fpsLimiter.resetFrameAdjust();
    }
    
    ~GraphicsPrivate() {
        /* The overlay texture is deliberately NOT deleted, for the same
         * reason the reserved surfaces are not: it was taken at boot and the
         * process owns it until it exits. Deleting it here would queue a
         * deferred release after GPUBudget::deferDrain("shutdown") has
         * already run (Graphics::shutdown), so nothing would ever retire it
         * -- and a Graphics rebuilt afterwards would have to take a texture
         * out of a sealed, possibly empty pool.
         *
         * Both of the following are guarded inside TEXFBO::fini() under
         * the bounded-GPU backend -- a reserved surface belongs to the
         * registry and must outlive every engine object that points at it. */
        TEXFBO::fini(frozenScene);
        TEXFBO::fini(integerScaleBuffer);
        SDL_DestroyMutex(avgFPSLock);
        SDL_DestroyMutex(glResourceLock);
    }
    
    void updateScreenResoRatio(RGSSThreadData *rtData) {
        if (scSize.x <= 0 || scSize.y <= 0 ||
            !std::isfinite(backingScaleFactor) || backingScaleFactor <= 0)
            return;

        Vec2 &ratio = rtData->sizeResoRatio;
        ratio.x = (float)scRes.x / scSize.x * backingScaleFactor;
        ratio.y = (float)scRes.y / scSize.y * backingScaleFactor;
        
        rtData->screenOffset = scOffset / backingScaleFactor;
    }
    
    /* Enforces fixed aspect ratio, if desired */
    void recalculateScreenSize(bool fixedAspectRatio) {
        scSize = winSize;
        
        if (!fixedAspectRatio) {
            if (!integerScaleActive || (integerScaleActive && integerLastMileScaling)) {
                scOffset = Vec2i(0, 0);
                return;
            }
        }
        
        if (integerScaleActive && !integerLastMileScaling) {
            scOffset.x = ((winSize.x / 2) - (scRes.x / 2) * integerScaleFactor.x);
            scOffset.y = ((winSize.y / 2) - (scRes.y / 2) * integerScaleFactor.y);
            
            scSize = Vec2i(scRes.x * integerScaleFactor.x, scRes.y * integerScaleFactor.y);
            return;
        }
        
        float resRatio = (float)scRes.x / scRes.y;
        float winRatio = (float)winSize.x / winSize.y;
        
        if (resRatio > winRatio)
            scSize.y = scSize.x / resRatio;
        else if (resRatio < winRatio)
            scSize.x = scSize.y * resRatio;
        
        scOffset.x = (winSize.x - scSize.x) / 2.f;
        scOffset.y = (winSize.y - scSize.y) / 2.f;
    }
    
    static int findHighestFittingScale(int base, int target) {
        int scale = 1;
        
        while (base * scale <= target)
            scale++;
        
        return std::max(scale - 1, 1);
    }
    
    /* Returns whether a new scale was found */
    bool findHighestIntegerScale()
    {
        Vec2i newScale(findHighestFittingScale(scRes.x, winSize.x),
                       findHighestFittingScale(scRes.y, winSize.y));
        
        if (threadData->config.fixedAspectRatio)
        {
            /* Limit both factors to the smaller of the two */
            newScale.x = newScale.y = std::min(newScale.x, newScale.y);
        }
        
        if (newScale == integerScaleFactor)
            return false;
        
        integerScaleFactor = newScale;
        return true;
    }
    
    void rebuildIntegerScaleBuffer()
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* This reserved target keeps its GL names, but re-specification
         * recreates the firmware surface and requires pool headroom.
         * integerScaleBuffer is a reference to the registry entry.
         *
         * The constructor calls this once with a zero scale factor, before
         * findHighestIntegerScale() has run; stock happily allocates a 0x0
         * texture, which is an incomplete framebuffer. Leave the surface at
         * its reserved size until there is a real factor -- every path that
         * reads integerScaleBuffer is gated on integerScaleStepApplicable(),
         * which is false while the factor is < 1. */
        if (integerScaleFactor.x >= 1 && integerScaleFactor.y >= 1)
            TEXFBO::reallocChecked(integerScaleBuffer,
                                   scRes.x * integerScaleFactor.x,
                                   scRes.y * integerScaleFactor.y,
                                   "integerScaleBuffer");
#else
        TEXFBO::fini(integerScaleBuffer);
        TEXFBO::init(integerScaleBuffer);
        TEXFBO::allocEmpty(integerScaleBuffer, scRes.x * integerScaleFactor.x,
                           scRes.y * integerScaleFactor.y);
        TEXFBO::linkFBO(integerScaleBuffer);
#endif
    }
    
    bool integerScaleStepApplicable() const
    {
        if (!integerScaleActive)
            return false;
        
        if (integerScaleFactor.x < 1 || integerScaleFactor.y < 1) // XXX should be < 2, this is for testing only
            return false;
        
        return true;
    }
    
    void checkResize(bool skipIntScaleBuffer = false) {
        Vec2i windowSize;
        if (threadData->windowSizeMsg.poll(windowSize)) {
            /* Query the actual size in pixels, not units */
            Vec2i drawableSize(windowSize);
            threadData->drawableSizeMsg.poll(drawableSize);
            
            if (windowSize.x <= 0 || windowSize.y <= 0 ||
                drawableSize.x <= 0 || drawableSize.y <= 0)
                return;

            backingScaleFactor = (float)drawableSize.x / windowSize.x;
            winSize = drawableSize;
            
            /* Make sure integer buffers are rebuilt before screen offsets are
             * calculated so we have the final allocated buffer size ready */
            if (integerScaleActive && findHighestIntegerScale() && !skipIntScaleBuffer)
                rebuildIntegerScaleBuffer();
            
            /* some GL drivers change the viewport on window resize */
            glState.viewport.refresh();
            recalculateScreenSize(threadData->config.fixedAspectRatio);
            updateScreenResoRatio(threadData);
            
            SDL_Rect screen = {scOffset.x, scOffset.y, scSize.x, scSize.y};
            threadData->ethread->notifyGameScreenChange(screen);
        }
    }
    
    void checkShutDownReset() {
        shState->checkShutdown();
        shState->checkReset();
    }
    
    void shutdown() {
        threadData->rqTermAck.set();
        shState->texPool().disable();

#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Last drain point: no more swaps are coming, so anything still
         * held would be released by process teardown with the GPU possibly
         * mid-render. Wait, then hand it all back while the context is
         * still current. The recycle pool's parked names
         * join the same drain, so they are released only after the wait. */
        GPUBudget::texRecycleDrain("shutdown");
        GPUBudget::deferDrain("shutdown", true);
#endif

        scriptBinding->terminate();
    }
    
    void swapGLBuffer(TransitionSwapCapture *capture = 0, bool auxiliary = false) {
        {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileIdle(FrameProfile::Idle);
#endif
            fpsLimiter.delay();
        }

#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* The swap-completion watchdog, and it is the only GPU-stall
         * canary reachable from userland: the flip chain is strictly
         * one-in-flight, and the swap blocks until the previous flip is
         * handed over before the next one, so
         * the wall-clock time of SDL_GL_SwapWindow IS a measurement of
         * whether the PREVIOUS frame's render completed. No driver call, no
         * privilege, no cost. */
        GPUBudget::swapBegin();
#endif

        {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileSwap(FrameProfile::Swap);
#endif
            if (capture) capture->observe(threadData->window);
            SDL_GL_SwapWindow(threadData->window);
            BootProfile::firstFrame();
        }

#ifdef MKXPZ_SOFTWARE_BITMAPS
        GPUBudget::swapEnd();

        /* The one REAL flip in the engine -- redrawScreen, the transition
         * loop and both fades all come through here. It is what retires the
         * deferred deletions: after two more frames have been handed to the
         * display, the render that was in flight when they were queued is
         * known complete. */
        GPUBudget::frameAdvance();
#endif

        if (!auxiliary) {
            ++frameCount;
            threadData->ethread->notifyFrame();
        }
    }
    
    void compositeToBuffer(TEXFBO &buffer) {
        compositeToBufferScaled(buffer, scRes.x, scRes.y);
    }

    void compositeToBufferScaled(TEXFBO &buffer, int destWidth, int destHeight) {
        GLStateGuard state(glState);
        screen.composite();
        
        int scaleIsSpecial = GLMeta::blitScaleIsSpecial(buffer, false, IntRect(0, 0, destWidth, destHeight), screen.getPP().frontBuffer(), IntRect(0, 0, scRes.x, scRes.y));

        GLMeta::blitBegin(buffer, false, scaleIsSpecial);
        GLMeta::blitSource(screen.getPP().frontBuffer(), scaleIsSpecial);
        GLMeta::blitRectangle(IntRect(0, 0, scRes.x, scRes.y), IntRect(0, 0, destWidth, destHeight));
        GLMeta::blitEnd();
        state.release();
    }
    
    void metaBlitBufferFlippedScaled(int scaleIsSpecial) {
        metaBlitBufferFlippedScaled(scRes, scaleIsSpecial);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        /* Vita: flip source tex coords instead of dest rect so the
         * present blit uses positive dst height with top-left origin. */
        GLMeta::blitRectangle(
                              IntRect(0, scRes.y, scRes.x, -scRes.y),
                              IntRect(scOffset.x,
                                      scOffset.y,
                                      scSize.x,
                                      scSize.y),
                              GLMeta::smoothScalingMethod(scaleIsSpecial) == Bilinear);
#else
        GLMeta::blitRectangle(
                              IntRect(0, 0, scRes.x, scRes.y),
                              IntRect(scOffset.x,
                                      (scSize.y + scOffset.y),
                                      scSize.x,
                                      -scSize.y),
                              GLMeta::smoothScalingMethod(scaleIsSpecial) == Bilinear);
#endif
    }

    void metaBlitBufferFlippedScaled(const Vec2i &sourceSize, int scaleIsSpecial, bool forceNearestNeighbor=false) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        /* Vita GLES software-blit path: the upstream negative-height
         * dest rect (y-flip convention) produces a black screen on this driver.
         * Flip the source tex coords instead — dest keeps positive
         * height and top-left origin, winding stays CCW. */
        GLMeta::blitRectangle(IntRect(0, sourceSize.y, sourceSize.x, -sourceSize.y),
                              IntRect(scOffset.x, scOffset.y, scSize.x, scSize.y),
                              !forceNearestNeighbor && GLMeta::smoothScalingMethod(scaleIsSpecial) == Bilinear);
#else
        GLMeta::blitRectangle(IntRect(0, 0, sourceSize.x, sourceSize.y),
                              IntRect(scOffset.x, scSize.y+scOffset.y, scSize.x, -scSize.y),
                              !forceNearestNeighbor && GLMeta::smoothScalingMethod(scaleIsSpecial) == Bilinear);
#endif
    }
    
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* ==================================================================
     * The persistent on-screen overlay.
     *
     * Three entry points, in the order they run: reserve once at boot,
     * compose when a consumer changes the text, draw once per frame.
     * ================================================================== */

    /* EXACTLY one texture, and it is taken HERE -- inside
     * SharedStatePrivate's construction, which sharedstate.cpp runs after
     * GPUBudget::reserveFixedSurfaces() and before GPUBudget::warmUpPrograms()
     * and GPUBudget::seal(). That ordering is the whole reservation: after the
     * seal the firmware sync pool may be empty, a texture created out of an
     * empty pool gets a NULL sync object, and GPUBudget::creationSite() would
     * (rightly) report it as a seal violation.
     *
     * So this is UNCONDITIONAL. It runs whether or not anything ever asks for
     * the overlay to be shown, because the alternative -- reserving lazily on
     * the first overlaySetVisible(true) -- is a post-seal texture, which is
     * the one thing the budget forbids. The cost of never using it is 256 KiB
     * of GPU-visible memory and one sync object. */
    void reserveOverlayTexture() {
        /* A second boot-reserved texture is another 256 KiB and another sync
         * object out of a pool of about 32: a budget decision, not a
         * detail. Keep this at one. */
        static_assert(GPUBudget::ReservedTextureCount == 1,
                      "the boot texture budget is exactly one texture");

        memset(overlayLines, 0, sizeof(overlayLines));
        overlay_init();

        overlayTex = TEX::gen();
        TEX::bind(overlayTex);
        /* Whole-level storage NOW, at the final size and format, so every
         * later upload is a same-size same-format whole-level refresh: then
         * TEX::uploadImage's GPUBudget::respecify() is the documented no-op
         * and the driver never has to free and re-take this texture's storage
         * underneath a frame that may still be sampling it. */
        TEX::allocEmpty(OVERLAY_W, OVERLAY_H);
        /* Nearest and clamped: the overlay is drawn 1:1, and an 8x8 face
         * filtered bilinearly is mud. setSmooth(false) does not touch
         * shState (it reads the config only when smoothing is ON), which
         * matters because shState is still null this early in boot -- the
         * same reason TEXFBO::init can call it from reserveFixedSurfaces. */
        TEX::setRepeat(false);
        TEX::setSmooth(false);
        TEX::unbind();

        char line[96];
        snprintf(line, sizeof(line),
                 "vita-gpu: overlay texture reserved tex=%u %dx%d (%u KiB, "
                 "pre-seal)", (unsigned)overlayTex.gl, OVERLAY_W, OVERLAY_H,
                 (unsigned)(OVERLAY_PIXELS * 4 / 1024));
        Debug() << line;
    }

    /* The whole single-writer rule, in one place. A call from a thread that
     * does not own the GL context is refused, not raced: it would interleave
     * a CPU compose with the upload of the frame it belongs to, and the two
     * would disagree about which image the texture holds.
     *
     * Refusal, not abort, because the player ships with asserts live (see
     * GPUBudget::creationSite): killing the game is a worse answer than
     * dropping a line some other thread had no business writing. The
     * diagnostic build (-Dvita_frame_trace=true) does abort, where a bisect
     * can afford it. */
    bool overlayWritableHere(const char *what) {
        const SDL_threadID self = SDL_ThreadID();
        if (self == overlayOwner)
            return true;

        if (!overlayForeignReported) {
            overlayForeignReported = true;
            char line[160];
            snprintf(line, sizeof(line),
                     "vita-overlay: BUG: %s from thread %u, owner %u; refused",
                     what, (unsigned)self, (unsigned)overlayOwner);
            GPUBudget::syncedLine(line);
        }
        /* The diagnostic build only. The Vita meson option
         * -Dvita_frame_trace=true defines VITA_GLUE_FRAME_TRACE
         * (meson.build:85); every other build leaves it undefined, so this
         * is compiled out and the refusal above is the whole behaviour. */
#if VITA_GLUE_FRAME_TRACE
        assert(false && "overlay written from a thread that does not own the GL context");
#endif
        return false;
    }

    /* Replace the overlay's text. Cheap and idempotent: identical lines are
     * recognised here and cost nothing at all -- no compose, no upload, no
     * GL -- which is what lets the overlay code call this every frame with the
     * same FPS string.
     *
     * Composition is vita/overlay's, not this file's: a box for the backdrop
     * and one overlay_text() per line. The dirty flag the upload reads is
     * maintained by the overlay itself, per pixel, so a rebuild that
     * reproduces the same image leaves the texture alone. */
    void overlaySetLines(const char *const *lines, int count) {
        if (!overlayWritableHere("overlaySetLines"))
            return;

        if (!lines)
            count = 0;
        if (count < 0)
            count = 0;
        if (count > OVERLAY_MAX_LINES)
            count = OVERLAY_MAX_LINES;

        bool changed = (count != overlayLineCount);

        for (int i = 0; i < count; ++i) {
            const char *src = lines[i] ? lines[i] : "";
            char *dst = overlayLines[i];

            if (!changed && strncmp(dst, src, OVERLAY_MAX_CHARS) == 0)
                continue;

            changed = true;
            /* Truncate rather than wrap: one slot is one line, and a line
             * wider than the surface is a caller's bug, not a layout problem
             * for the HUD to solve mid-frame. Bounded copy by length, not
             * strncpy: the slot always ends up NUL-terminated. */
            size_t n = strlen(src);
            if (n > OVERLAY_MAX_CHARS)
                n = OVERLAY_MAX_CHARS;
            memcpy(dst, src, n);
            dst[n] = '\0';
        }

        if (!changed)
            return;

        overlayLineCount = count;
        composeOverlay();
    }

    /* Paint the current lines into vita/overlay's surface.
     *
     * Every compose writes inside one box anchored at (0, 0), so erasing the
     * PREVIOUS box is exactly enough to start clean -- there can be no ink
     * anywhere else. That is what keeps this proportional to the text: an FPS
     * line is a box of a few thousand pixels, not a pass over all 65 536. */
    void composeOverlay() {
        /* overlay_box() with a transparent colour REPLACES (it writes the
         * alpha rather than blending it), so this really does erase. */
        overlay_box(0, 0, overlayPaintedW, overlayPaintedH,
                    OVERLAY_TRANSPARENT);
        overlayPaintedW = 0;
        overlayPaintedH = 0;

        if (overlayLineCount <= 0)
            return;

        int widest = 0;
        for (int i = 0; i < overlayLineCount; ++i) {
            const int w = overlay_text_width(overlayLines[i]);
            if (w > widest)
                widest = w;
        }

        const int padX = overlaySettings ? 16 : OVERLAY_PAD_X;
        int boxW = overlaySettings ? 480 : widest + padX * 2;
        const int boxH = overlaySettings ? OVERLAY_H :
            overlayLineCount * OVERLAY_LINE_H + OVERLAY_PAD_Y * 2;

        if (boxW > OVERLAY_W)
            boxW = OVERLAY_W;

        overlay_box(0, 0, boxW, boxH, OVERLAY_BACKDROP);
        for (int i = 0; i < overlayLineCount; ++i)
            overlay_text(padX,
                         OVERLAY_PAD_Y + i * OVERLAY_LINE_H,
                         overlayLines[i]);

        overlayPaintedW = boxW;
        overlayPaintedH = boxH;
    }

    /* One quad over the finished frame, on the default framebuffer, after the
     * screen blit and before the swap.
     *
     * Costs when hidden: nothing. Not a bind, not a uniform, not a state
     * push -- the frame's GL call sequence is the one the unpatched engine
     * issues, which is what makes the overlay free for every game that never
     * opens it.
     *
     * Costs when visible: one glTexImage2D on a text change, and one
     * six-index glDrawElements otherwise. No new GL object of any kind: the
     * texture was reserved at boot, the program is the simple shader the
     * screen blit just used, and the geometry is shState->gpQuad(), the
     * engine's scratch quad, which under this backend draws straight out of
     * client memory and so owns no buffer either. */
    void drawOverlay() {
        if (!overlayVisible || overlayTex == TEX::ID(0))
            return;

        TEX::bind(overlayTex);

        /* Upload ONLY when the composed pixels differ from what the texture
         * already holds. overlay_dirty() tracks content, not writes, so N
         * frames of unchanged text cost exactly one upload -- the first one
         * -- and a whole-level 256 KiB refresh never happens twice for the
         * same image. Whole level, never a sub-rect: a sub-rect upload on
         * this driver is the one transfer path with no sync object behind it
         */
        if (overlay_dirty()) {
            overlay_copy_rgba8888(overlayStaging, OVERLAY_W * 4);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileUpload(FrameProfile::Upload, 0, false, false);
#endif
            TEX::uploadImage(OVERLAY_W, OVERLAY_H, overlayStaging, GL_RGBA);
            overlay_mark_uploaded();
        }

        /* blitEnd() popped the window-sized viewport that blitBeginScreen()
         * pushed, and applyViewportProj() reads the viewport to build the
         * projection, so push it back for the draw. FBO 0 is already bound:
         * blitBeginScreen bound it and blitEnd does not unbind. */
        glState.viewport.pushSet(IntRect(0, 0, winSize.x, winSize.y));
        /* BLENDING ON, always. The overlay's backdrop is 3/4 opaque and its
         * glyphs are ink on transparent, so blending is what makes it an
         * overlay rather than a 512x128 hole in the frame. It is also the
         * only state the driver has a compiled variant of for this program
         * and this target: GPUBudget::warmUpPrograms() draws one pixel per
         * (program x target x blend state) at boot, and a variant first
         * needed mid-game wants a code-heap segment out of a pool that by
         * then has nothing left. BlendNormal matches the
         * non-premultiplied ARGB vita/overlay composes. */
        glState.blend.pushSet(true);
        glState.blendMode.pushSet(BlendNormal);

        SimpleShader &shader = shState->shaders().simple;
        shader.bind();
        shader.applyViewportProj();
        shader.setTranslation(Vec2i());
        shader.setTexSize(Vec2i(OVERLAY_W, OVERLAY_H));

        Quad &quad = shState->gpQuad();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        /* Same convention as the present blit above (metaBlitBufferFlippedScaled):
         * flip the SOURCE tex coords and keep a positive destination height.
         * That is what puts row 0 of a texture where the game frame's row 0
         * lands -- the top of the screen -- and it is the form proven to
         * present correctly here, where a negative dest height gives a
         * black screen. The overlay sits at the very top left
         * of the window, over the letterbox bar when there is one. */
        if (overlaySettings) {
            // Reuse one backdrop texel to dim the retained frame; no new GL objects.
            quad.setTexPosRect(IntRect(0, 0, 1, 1), IntRect(0, 0, winSize.x, winSize.y));
            quad.draw();
            quad.setTexPosRect(IntRect(0, OVERLAY_H, 480, -OVERLAY_H),
                IntRect((winSize.x - 960) / 2, (winSize.y - 256) / 2, 960, 256));
        } else {
            quad.setTexPosRect(IntRect(0, OVERLAY_H, OVERLAY_W, -OVERLAY_H),
                               IntRect(0, 0, OVERLAY_W, OVERLAY_H));
        }
#else
        /* Desktop: the stock convention, a negative destination height. */
        quad.setTexPosRect(IntRect(0, 0, OVERLAY_W, OVERLAY_H),
                           IntRect(0, winSize.y, OVERLAY_W, -OVERLAY_H));
#endif
        quad.draw();

        glState.blendMode.pop();
        glState.blend.pop();
        glState.viewport.pop();

        GPUBudget::pollGLError("overlay");
    }
#endif

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The freeze contract for the per-frame composite: a refused upload
     * retires the deferred queue and retries once; a second one skips the
     * frame (the CPU pixels are intact and the next update draws the scene
     * again) instead of raising into the script, like freeze and transition.
     * A skipped frame presents nothing but still counts and paces. A refusal
     * that outlasts uploadSkipLimit consecutive frames is not transient (a
     * leaked pool, a scene too big for the card): it is raised so the normal
     * error report runs instead of a frozen screen that never explains itself. */
    bool compositeOrSkip() {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            try {
                screen.composite();
                uploadSkips = 0;
                return true;
            } catch (const TEX::UploadError &) {
                if (attempt == 0)
                    GPUBudget::deferDrain("frame-retry", true);
            }
        }
        if (++uploadSkips >= uploadSkipLimit) {
            uploadSkips = 0;
            std::fputs("vita-gpu: Graphics.update upload failed on every frame "
                       "for too long; raising\n", stderr);
            throw TEX::UploadError();
        }
        static Uint32 lastSkip = 0;
        static bool skipLogged = false;
        const Uint32 now = SDL_GetTicks();
        if (!skipLogged || now - lastSkip >= 1000) {
            std::fputs("vita-gpu: Graphics.update upload failed; skipping the frame\n", stderr);
            lastSkip = now;
            skipLogged = true;
        }
        fpsLimiter.delay();
        ++frameCount;
        threadData->ethread->notifyFrame();
        return false;
    }
#endif

    /* False when the frame was skipped: it is already counted and paced, so
     * the caller must do nothing more for it. */
    bool redrawScreen() {
        GLStateGuard state(glState);
        resetTargetCounters();
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
        vita_glue_trace("trace: redrawScreen enter");
#endif
#ifdef MKXPZ_SOFTWARE_BITMAPS
        if (!compositeOrSkip())
            return false;
#else
        screen.composite();
#endif
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
        vita_glue_trace("trace: redrawScreen composite done");
#endif
        
        // maybe unspaghetti this later
        if (integerScaleStepApplicable() && !integerLastMileScaling)
        {
            int scaleIsSpecial = GLMeta::blitScaleIsSpecial(integerScaleBuffer, false, IntRect(0, 0, scSize.x, scSize.y), screen.getPP().frontBuffer(), IntRect(0, 0, scRes.x, scRes.y));

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileBlit1(FrameProfile::Blit);
#endif
            { FBO::ID before = FBO::boundFramebufferID;
              GLMeta::blitBeginScreen(winSize, scaleIsSpecial);
              noteTargetSwitch(before); }
            GLMeta::blitSource(screen.getPP().frontBuffer(), scaleIsSpecial);
            
            FBO::clear();
            metaBlitBufferFlippedScaled(scRes, scaleIsSpecial, true);
            GLMeta::blitEnd();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            profileBlit1.stop();
#endif
            logTargetCounters();

#ifdef MKXPZ_SOFTWARE_BITMAPS
            /* Over the finished frame, before the flip. Both of redrawScreen's
             * exits get it, or the HUD would blink off whenever integer
             * last-mile scaling took this branch. */
            drawOverlay();
#endif

            swapGLBuffer();
            updateAvgFPS();
            state.release();
            return true;
        }
        
        if (integerScaleStepApplicable())
        {
            int scaleIsSpecial = GLMeta::blitScaleIsSpecial(integerScaleBuffer, false, IntRect(0, 0, integerScaleBuffer.width, integerScaleBuffer.height), screen.getPP().frontBuffer(), IntRect(0, 0, scRes.x, scRes.y));

            assert(integerScaleBuffer.tex != TEX::ID(0));
            { FBO::ID before = FBO::boundFramebufferID;
              GLMeta::blitBegin(integerScaleBuffer, false, scaleIsSpecial);
              noteTargetSwitch(before); }
            GLMeta::blitSource(screen.getPP().frontBuffer(), scaleIsSpecial);
            
            GLMeta::blitRectangle(IntRect(0, 0, scRes.x, scRes.y),
                                  IntRect(0, 0, integerScaleBuffer.width, integerScaleBuffer.height),
                                  false);
            
            GLMeta::blitEnd();
        }
        

        Vec2i sourceSize;

        if (integerScaleActive)
        {
            sourceSize = Vec2i(integerScaleBuffer.width, integerScaleBuffer.height);
        }
        else
        {
            sourceSize = scRes;
        }

        int scaleIsSpecial = GLMeta::blitScaleIsSpecial(integerScaleBuffer, false, IntRect(0, 0, scSize.x, scSize.y), integerScaleActive ? integerScaleBuffer : screen.getPP().frontBuffer(), IntRect(0, 0, sourceSize.x, sourceSize.y));

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileBlit2(FrameProfile::Blit);
#endif
        { FBO::ID before = FBO::boundFramebufferID;
          GLMeta::blitBeginScreen(winSize, scaleIsSpecial);
          noteTargetSwitch(before); }
        //GLMeta::blitSource(screen.getPP().frontBuffer(), scaleIsSpecial);

        if (integerScaleActive)
        {
            GLMeta::blitSource(integerScaleBuffer, scaleIsSpecial);
        }
        else
        {
            GLMeta::blitSource(screen.getPP().frontBuffer(), scaleIsSpecial);
        }

        FBO::clear();
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
        {
            char tb[160];
            snprintf(tb, sizeof(tb),
                     "trace: present dst=%d,%d %dx%d scRes=%dx%d win=%dx%d",
                     scOffset.x, scOffset.y, scSize.x, scSize.y,
                     scRes.x, scRes.y, winSize.x, winSize.y);
            vita_glue_trace(tb);
        }
#endif
        metaBlitBufferFlippedScaled(sourceSize, scaleIsSpecial);

        GLMeta::blitEnd();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileBlit2.stop();
#endif
        logTargetCounters();

#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* The overlay goes on the finished frame: after the screen blit, on
         * the default framebuffer, and before the swap hands the frame to the
         * display. */
        drawOverlay();
#endif

        swapGLBuffer();
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
        vita_glue_trace("trace: redrawScreen swap done");
#endif

        updateAvgFPS();
        state.release();
        return true;
    }
    
    void checkSyncLock() {
        if (!threadData->syncPoint.mainSyncLocked())
            return;
        
        /* Releasing the GL context before sleeping and making it
         * current again on wakeup seems to avoid the context loss
         * when the app moves into the background on Android */
        SDL_GL_MakeCurrent(threadData->window, 0);
        threadData->syncPoint.waitMainSync();
        SDL_GL_MakeCurrent(threadData->window, glCtx);
        
        fpsLimiter.resetFrameAdjust();
    }
    
    double averageFPS() {
        double ret = 0;
        SDL_LockMutex(avgFPSLock);
        for (double times : avgFPSData)
            ret += times;
        
        ret = avgFPSData.empty() ? 0.0 : 1 / (ret / avgFPSData.size());
        SDL_UnlockMutex(avgFPSLock);
        return ret;
    }
    
    void setLock(bool force = false) {
        if (!(force || multithreadedMode)) return;
        
        SDL_LockMutex(glResourceLock);
        SDL_GL_MakeCurrent(threadData->window, threadData->glContext);
    }
    
    void releaseLock(bool force = false) {
        if (!(force || multithreadedMode)) return;
        
        SDL_UnlockMutex(glResourceLock);
    }

    void updateAvgFPS() {
        SDL_LockMutex(avgFPSLock);
        if (avgFPSData.size() > 40)
            avgFPSData.erase(avgFPSData.begin());
        
        double time = shState->runTime();
        avgFPSData.push_back(time - last_avg_update);
        last_avg_update = time;
        SDL_UnlockMutex(avgFPSLock);
    }
};

Graphics::Graphics(RGSSThreadData *data) {
    p = new GraphicsPrivate(data);
    transitionCaptureDiagnostics = diagnosticPayloadInstalled(data->config.customScript);
    if (data->config.syncToRefreshrate) {
        p->frameRate = data->refreshRate;
        p->fpsLimiter.disabled = true;
    } else if (data->config.fixedFramerate > 0) {
        p->fpsLimiter.setDesiredFPS(data->config.fixedFramerate);
    } else if (data->config.fixedFramerate < 0) {
        p->fpsLimiter.disabled = true;
    }
}

Graphics::~Graphics() { delete p; }

double Graphics::getDelta() {
    return shState->runTime() - p->last_update;
}

double Graphics::lastUpdate() {
    return p->last_update;
}

void Graphics::update(bool checkForShutdown) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    const double profileTarget = !vita_glue_frame_profile_interval ? 0 :
        p->threadData->config.syncToRefreshrate ? 1000000.0 / p->frameRate :
        p->fpsLimiter.disabled ? 0 : p->fpsLimiter.tpf * (1000000.0 / p->fpsLimiter.tickFreq);
    FrameProfile::Frame profileFrame(profileTarget, !checkForShutdown);
#endif
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    vita_glue_trace("trace: Graphics::update enter");
#endif
    p->threadData->rqWindowAdjust.wait();
    p->last_update = shState->runTime();
    /* A resume or clock gap the event thread saw. */
    if (p->threadData->rqFrameReset) {
        p->threadData->rqFrameReset.clear();
        p->fpsLimiter.resetFrameAdjust();
    }
    
    // update Input.repeat timing, rounding the framerate to the nearest 2
    {
        static const double mult = 2.0;
        double afr = std::abs(averageFrameRate()); // abs shouldn't be necessary but that's ok
        afr += mult / 2;
        afr -= std::fmod(afr, mult);
        shState->input().recalcRepeat(std::floor(afr));
    }
    
    if (checkForShutdown)
        p->checkShutDownReset();
    
    p->checkSyncLock();
    
    
#ifdef MKXPZ_STEAM
    if (STEAMSHIM_alive())
        STEAMSHIM_pump();
#endif
    
    requireRenderTargets();
    if (p->frozen) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileFrame.frozen = true;
#endif
        return;
    }
    
    if (p->fpsLimiter.frameSkipRequired()) {
        if (p->useFrameSkip) {
            /* Skip frame */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            profileFrame.skipped = true;
#endif
            {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                FrameProfile::Scope profileIdle(FrameProfile::Idle);
#endif
                p->fpsLimiter.delay();
            }
            ++p->frameCount;
            p->threadData->ethread->notifyFrame();
            
            return;
        } else {
            /* Just reset frame adjust counter */
            p->fpsLimiter.resetFrameAdjust();
        }
    }
    
    p->checkResize();
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    vita_glue_trace("trace: Graphics::update redrawScreen next");
#endif
    if (!p->redrawScreen()) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileFrame.skipped = true;
#endif
        return;
    }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    // Only a normal, non-skipped frame can enter; movie/internal updates defer.
    VitaSettingsInput initial;
    if (checkForShutdown && p->threadData->ethread->settingsMenu.begin(initial))
        runSettingsMenu(initial);
#endif
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* One drain per real frame, after the swap. A dropped draw on this
     * driver is silent -- GL_OUT_OF_MEMORY and nothing rendered
     * -- so without this the first sign of trouble
     * is a render that never completes. */
    GPUBudget::pollGLError("update");
#endif
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    vita_glue_trace("trace: Graphics::update leave");
#endif
}

void Graphics::freeze() {
    p->checkShutDownReset();
    p->checkResize();
    requireRenderTargets();

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* Every RGSS scene change runs Graphics.freeze before Graphics.transition,
     * which makes this the engine's one reliable "a scene just ended" hook --
     * the right place to sample the GPU-object budget, because a leak shows up
     * as a count that climbs scene after scene. Off unless
     * the gpu-telemetry marker is present.
     *
     * logSceneChange also runs the headroom canary, which is the only
     * userland view of the firmware pool. */
    GPUBudget::pollGLError("freeze-before-composite");
    GPUBudget::logSceneChange();
    GPUBudget::breadcrumb("BEGIN", "freeze-composite");
#endif

    /* Publish frozen only after capture and its failure-prone work finish. */
#ifdef MKXPZ_SOFTWARE_BITMAPS
    GLStateGuard state(glState);
    GLRenderErrorScope errors;
#endif
    p->compositeToBuffer(p->frozenScene);
#ifdef MKXPZ_SOFTWARE_BITMAPS
    try {
        errors.check("Graphics.freeze composition");
    } catch (const Exception &) {
        // The failed capture may have overwritten an earlier frozen scene.
        p->frozen = false;
        static Uint32 lastRefusal = 0;
        static bool refusalLogged = false;
        const Uint32 now = SDL_GetTicks();
        if (!refusalLogged || now - lastRefusal >= 1000) {
            std::fputs("vita-gpu: Graphics.freeze composition failed; skipping frozen frame\n", stderr);
            lastRefusal = now;
            refusalLogged = true;
        }
        return;
    }
#endif

#ifdef MKXPZ_SOFTWARE_BITMAPS
    GPUBudget::breadcrumb("END", "freeze-composite");
    GPUBudget::pollGLError("freeze-after-composite");

    /* From here p->frozen makes Graphics.update a no-op, so the engine
     * issues no further frames at all -- only texture and VBO traffic for
     * the next scene -- and nothing would retire the deferred queue, kick a
     * scene or reclaim a driver ghost until the transition. That is exactly
     * the window an early crash was found in.
     *
     * The drain is also the single highest-value instrument here: it calls
     * into WaitForRender, which retries up to 10000 x 100 ms, so a render
     * that never completes turns a silent process stop into a log that ends
     * at "BEGIN freeze-drain". */
    GPUBudget::breadcrumb("BEGIN", "freeze-drain");
    GPUBudget::deferDrain("freeze", true);
    GPUBudget::breadcrumb("END", "freeze-drain");

    GPUBudget::headroomCanary("freeze");
    state.release();
#endif
    p->frozen = true;
}

void Graphics::transition(int duration, const char *filename, int vague) {
    p->checkSyncLock();

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && defined(MKXPZ_SOFTWARE_BITMAPS)
    if (vita_glue_memory_ledger_enabled) GPUBudget::logMemoryLedger();
#endif
    if (!p->frozen)
        return;
    requireRenderTargets();
    GLStateGuard state(glState);

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The transition is the first thing that renders after a freeze, and
     * the last frame it flipped can be the render that never completes.
     * Bracket it. */
    GPUBudget::pollGLError("transition-begin");
    GPUBudget::breadcrumb("BEGIN", "transition");
#endif

    vague = clamp(vague, 1, 256);
    /* Owned for the whole body, which is ~40 lines of GL work plus a
     * duration-long loop that composites, blits and swaps. Every one of those
     * can throw -- composite() recomposes CPU window bases under
     * MKXPZ_SOFTWARE_BITMAPS, and checkResize() re-specifies surfaces through
     * reallocChecked -- and a raw pointer lost a whole transition bitmap on
     * each. The two early returns below keep their explicit reset() so the
     * texture still goes back before shutdown() drains the GPU. */
    std::unique_ptr<Bitmap> transMap(*filename ? new Bitmap(filename) : 0);

    setBrightness(255);
    
    /* Capture new scene */
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The freeze contract, extended to the scene being changed TO. Both
     * uploads this call can reach are inside the guard: the new scene's
     * Bitmaps, sampled by the composite, and the transition bitmap. A
     * recoverable failure retires the deferred queue and retries once; a
     * second one cuts to the new scene instead of raising into the script,
     * because the CPU pixels are intact and the next Graphics.update draws
     * the scene the game has already switched to. */
    TEX::ID transTex(0);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        try {
            p->screen.composite();
            if (transMap)
                transTex = transMap->getGLTypes().tex;
            break;
        } catch (const TEX::UploadError &) {
            if (attempt == 0) {
                GPUBudget::deferDrain("transition-retry", true);
                continue;
            }
            std::fputs("vita-gpu: Graphics.transition upload failed; "
                       "cutting to the new scene\n", stderr);
            p->frozen = false;
            GPUBudget::breadcrumb("END", "transition/cut");
            return;
        }
    }
#else
    p->screen.composite();
    TEX::ID transTex = transMap ? transMap->getGLTypes().tex : TEX::ID(0);
#endif
    
    /* The PP frontbuffer will hold the current scene after the
     * composition step. Since the backbuffer is unused during
     * the transition, we can reuse it as the target buffer for
     * the final rendered image. */
    TEXFBO &currentScene = p->screen.getPP().frontBuffer();
    TEXFBO &transBuffer = p->screen.getPP().backBuffer();
    
    /* If no transition bitmap is provided,
     * we can use a simplified shader */
    TransShader &transShader = shState->shaders().trans;
    SimpleTransShader &simpleShader = shState->shaders().simpleTrans;
    
    // Handle high-res.
    Vec2i transSize(p->scResLores.x, p->scResLores.y);

    if (transMap) {
        TransShader &shader = transShader;
        shader.bind();
        shader.applyViewportProj();
        shader.setFrozenScene(p->frozenScene.tex);
        shader.setCurrentScene(currentScene.tex);
        if (transMap->hasHires()) {
            Debug() << "BUG: High-res Graphics transMap not implemented";
        }
        shader.setTransMap(transTex);
        shader.setVague(vague / 256.0f);
        shader.setTexSize(transSize);
    } else {
        SimpleTransShader &shader = simpleShader;
        shader.bind();
        shader.applyViewportProj();
        shader.setFrozenScene(p->frozenScene.tex);
        shader.setCurrentScene(currentScene.tex);
        shader.setTexSize(transSize);
    }
    
    GLProperty<bool>::Guard blend(glState.blend);
    glState.blend.set(false);
    
    TransitionSwapCapture capture(filename);
    TransitionSwapCapture *observer = capture.directory.empty() ? 0 : &capture;

    for (int i = 0; i < duration; ++i) {
        /* We need to clean up transMap properly before
         * a possible longjmp, so we manually test for
         * shutdown/reset here */
        if (p->threadData->rqTerm) {
            blend.restore();
            state.restore();
            transMap.reset();
            std::string().swap(capture.directory);
            capture.pixels.reset();
#ifdef MKXPZ_SOFTWARE_BITMAPS
            GPUBudget::breadcrumb("END", "transition/term");
#endif
            p->shutdown();
            return;
        }

        if (p->threadData->rqReset) {
            blend.restore();
            state.restore();
            transMap.reset();
            std::string().swap(capture.directory);
            capture.pixels.reset();
#ifdef MKXPZ_SOFTWARE_BITMAPS
            GPUBudget::breadcrumb("END", "transition/reset");
#endif
            scriptBinding->reset();
            return;
        }
        
        p->checkSyncLock();
        
        const float prog = i * (1.0f / duration);
        
        if (transMap) {
            transShader.bind();
            transShader.setProg(prog);
        } else {
            simpleShader.bind();
            simpleShader.setProg(prog);
        }
        
        resetTargetCounters();

        /* Draw the composed frame to a buffer first
         * (we need this because we're skipping PingPong) */
        { FBO::ID before = FBO::boundFramebufferID;
          FBO::bind(transBuffer.fbo);
          noteTargetSwitch(before); }
        FBO::clear();
        p->screenQuad.draw();
        
        p->checkResize();
        
        /* Then blit it flipped and scaled to the screen */
        { FBO::ID before = FBO::boundFramebufferID;
          FBO::unbind();
          noteTargetSwitch(before); }
        FBO::clear();
        
        int scaleIsSpecial = GLMeta::blitScaleIsSpecial(p->integerScaleBuffer, false, IntRect(0, 0, p->scSize.x, p->scSize.y), transBuffer, IntRect(0, 0, p->scRes.x, p->scRes.y));

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileBlit3(FrameProfile::Blit);
#endif
        { FBO::ID before = FBO::boundFramebufferID;
          GLMeta::blitBeginScreen(Vec2i(p->winSize), scaleIsSpecial);
          noteTargetSwitch(before); }
        GLMeta::blitSource(transBuffer, scaleIsSpecial);
        p->metaBlitBufferFlippedScaled(scaleIsSpecial);
        GLMeta::blitEnd();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileBlit3.stop();
#endif
        logTargetCounters();
        
        p->swapGLBuffer(observer);
        if (observer && !capture.directory.empty() && i == capture.probeFrame && duration == 8) {
            if (capture.presentProbe(p->threadData->window, transBuffer)) {
                capture.auxiliary = true;
                // Default read target only; preserve the desktop split read binding.
                GLint read = 0;
                if (gl.BlitFramebuffer) {
                    gl.GetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read);
                    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                }
                p->swapGLBuffer(observer, true);
                if (gl.BlitFramebuffer) gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read);
                capture.auxiliary = false;
                std::fprintf(stderr, "transition-probe: frame=%02d extra-real-swap=1 logical-frame=%d capture=%s\n",
                             i, p->frameCount, capture.directory.empty() ? "FAIL" : "OK");
            }
        }
        /* Call this manually, as redrawScreen() is not called during this loop. */
        p->updateAvgFPS();
    }
    
    blend.restore();

    transMap.reset();

    p->frozen = false;
    state.release();

#ifdef MKXPZ_SOFTWARE_BITMAPS
    GPUBudget::breadcrumb("END", "transition");
    GPUBudget::pollGLError("transition-end");
#endif
}

void Graphics::frameReset() {p->fpsLimiter.resetFrameAdjust();}

static void guardDisposed() {}

DEF_ATTR_RD_SIMPLE(Graphics, FrameRate, int, p->frameRate)

DEF_ATTR_SIMPLE(Graphics, FrameCount, int, p->frameCount)

void Graphics::setFrameRate(int value) {
    p->frameRate = std::max(10, std::min(value, 120));
    
    if (p->threadData->config.syncToRefreshrate)
        return;
    
    if (p->threadData->config.fixedFramerate > 0)
        return;
    
    p->fpsLimiter.setDesiredFPS(p->frameRate);
    //shState->input().recalcRepeat((unsigned int)p->frameRate);
}

double Graphics::averageFrameRate() {
    return p->averageFPS();
}

void Graphics::wait(int duration) {
    for (int i = 0; i < duration; ++i) {
        p->checkShutDownReset();
        p->redrawScreen();
    }
}

void Graphics::fadeout(int duration) {
    FBO::unbind();
    
    float curr = p->brightness;
    float diff = 255.0f - curr;
    
    for (int i = duration - 1; i > -1; --i) {
        setBrightness(diff + (curr / duration) * i);
        
        if (p->frozen) {
            int scaleIsSpecial = GLMeta::blitScaleIsSpecial(p->integerScaleBuffer, false, IntRect(0, 0, p->scSize.x, p->scSize.y), p->frozenScene, IntRect(0, 0, p->scRes.x, p->scRes.y));

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileBlit4(FrameProfile::Blit);
#endif
            GLMeta::blitBeginScreen(p->scSize, scaleIsSpecial);
            GLMeta::blitSource(p->frozenScene, scaleIsSpecial);
            
            FBO::clear();
            p->metaBlitBufferFlippedScaled(scaleIsSpecial);
            
            GLMeta::blitEnd();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            profileBlit4.stop();
#endif
            
            p->swapGLBuffer();
        } else {
            update();
        }
    }
}

void Graphics::fadein(int duration) {
    FBO::unbind();
    
    float curr = p->brightness;
    float diff = 255.0f - curr;
    
    for (int i = 1; i <= duration; ++i) {
        setBrightness(curr + (diff / duration) * i);
        
        if (p->frozen) {
            int scaleIsSpecial = GLMeta::blitScaleIsSpecial(p->integerScaleBuffer, false, IntRect(0, 0, p->scSize.x, p->scSize.y), p->frozenScene, IntRect(0, 0, p->scRes.x, p->scRes.y));

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileBlit5(FrameProfile::Blit);
#endif
            GLMeta::blitBeginScreen(p->scSize, scaleIsSpecial);
            GLMeta::blitSource(p->frozenScene, scaleIsSpecial);
            
            FBO::clear();
            p->metaBlitBufferFlippedScaled(scaleIsSpecial);
            
            GLMeta::blitEnd();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            profileBlit5.stop();
#endif
            
            p->swapGLBuffer();
        } else {
            update();
        }
    }
}

Bitmap *Graphics::snapToBitmap() {
    requireRenderTargets();
    GLStateGuard state(glState);
#ifdef MKXPZ_SOFTWARE_BITMAPS
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        {
            GLRenderErrorScope compositionErrors;
            p->screen.composite();
            try {
                compositionErrors.check("Bitmap snapshot composition");
                break;
            } catch (const Exception &) {
                if (attempt != 0)
                    throw;
            }
        }
        // End the failed scope before draining and checking a fresh attempt.
        GPUBudget::deferDrain("snapshot-retry", true);
    }
    GLRenderErrorScope errors;
#else
    p->screen.composite();
#endif

    if (shState->config().enableHires) {
        // TODO: Maybe don't reconstruct this struct every time?
        TEXFBO tf;
        tf.width = width();
        tf.height = height();
        tf.selfHires = &p->screen.getPP().frontBuffer();

        std::unique_ptr<Bitmap> result(new Bitmap(tf));
#ifdef MKXPZ_SOFTWARE_BITMAPS
        errors.check("Bitmap snapshot readback");
#endif
        state.release();
        return result.release();
    }

    std::unique_ptr<Bitmap> result(new Bitmap(p->screen.getPP().frontBuffer()));
#ifdef MKXPZ_SOFTWARE_BITMAPS
    errors.check("Bitmap snapshot readback");
#endif
    state.release();
    return result.release();
}

int Graphics::width() const { return p->scResLores.x; }

int Graphics::height() const { return p->scResLores.y; }

int Graphics::widthHires() const { return p->scRes.x; }

int Graphics::heightHires() const { return p->scRes.y; }

bool Graphics::isPingPongFramebufferActive() const {
    return p->screen.getPP().frontBuffer().fbo == FBO::boundFramebufferID || p->screen.getPP().backBuffer().fbo == FBO::boundFramebufferID;
}

int Graphics::displayContentWidth() const {
    return p->scSize.x;
}

int Graphics::displayContentHeight() const {
    return p->scSize.y;
}

int Graphics::displayWidth() const {
    SDL_DisplayMode dm{};
    SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(shState->sdlWindow()), &dm);
    return dm.w / p->backingScaleFactor;
}

int Graphics::displayHeight() const {
    SDL_DisplayMode dm{};
    SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(shState->sdlWindow()), &dm);
    return dm.h / p->backingScaleFactor;
}

void Graphics::resizeScreen(int width, int height) {
#ifndef MKXPZ_SOFTWARE_BITMAPS
    p->threadData->rqWindowAdjust.wait();
    p->checkResize(true);
#endif
    Vec2i sizeLores(width, height);
#ifdef MKXPZ_SOFTWARE_BITMAPS
    const Vec2i size = GPUBudget::renderScreenSize(shState->config(), width, height);
    width = size.x;
    height = size.y;
#else
    if (shState->config().enableHires) {
        double framebufferScalingFactor = shState->config().framebufferScalingFactor;
        width = (int)lround(framebufferScalingFactor * width);
        height = (int)lround(framebufferScalingFactor * height);
    }

    Vec2i size(width, height);
#endif
    
    if (p->scRes == size && p->scResLores == sizeLores
#ifdef MKXPZ_SOFTWARE_BITMAPS
        && GPUBudget::fixedSurfacesValid()
#endif
        )
        return;

#ifdef MKXPZ_SOFTWARE_BITMAPS
    p->threadData->rqWindowAdjust.wait();
    Vec2i scale = p->integerScaleFactor;
    Vec2i integerSize(p->integerScaleBuffer.width, p->integerScaleBuffer.height);
    if (p->integerScaleActive) {
        scale = Vec2i(p->findHighestFittingScale(width, p->winSize.x),
                      p->findHighestFittingScale(height, p->winSize.y));
        if (p->threadData->config.fixedAspectRatio)
            scale.x = scale.y = std::min(scale.x, scale.y);
        integerSize = Vec2i(width * scale.x, height * scale.y);
    }
    /* Complete every allocation before publishing dimensions or geometry.
     * Pending window messages are handled by the normal update path. */
    GPUBudget::resizeFixedSurfaces(size, integerSize);
    p->integerScaleFactor = scale;
#endif
    
    p->scRes = size;
    p->scResLores = sizeLores;

    p->screen.setResolution(width, height);

    /* Recalculate letterbox now that scRes changed. Without this the
     * scSize/scOffset from the constructor (computed against the RGSS
     * default resolution, e.g. 640x480) go stale and the present blit
     * uses the wrong dst rect. requestWindowResize alone is not enough:
     * when the window is already the target size, SDL fires no
     * SIZE_CHANGED and checkResize never runs. */
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);

    if (p->integerScaleActive)
        p->rebuildIntegerScaleBuffer();

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The transaction above already sized this target. */
    TEXFBO::reallocChecked(p->frozenScene, width, height, "frozenScene");
#else
    TEXFBO::allocEmpty(p->frozenScene, width, height);
#endif

    FloatRect screenRect(0, 0, width, height);
    p->screenQuad.setTexPosRect(screenRect, screenRect);
    
    glState.scissorBox.set(IntRect(0, 0, p->scRes.x, p->scRes.y));
    
    shState->eThread().requestWindowResize(width, height);
}

void Graphics::resizeWindow(int width, int height, bool center) {
    p->threadData->rqWindowAdjust.wait();
    p->checkResize();
    
    if (width == p->winSize.x / p->backingScaleFactor &&
        height == p->winSize.y / p->backingScaleFactor)
            return;

    shState->eThread().requestWindowResize(width, height);
    
    if (center)
        this->center();
}

bool Graphics::updateMovieInput(Movie *movie) {
    return  p->threadData->rqTerm || p->threadData->rqReset;
}

void Graphics::playMovie(const char *filename, int volume_, bool skippable) {
    if (shState->config().enableHires) {
        Debug() << "BUG: High-res Graphics playMovie not implemented";
    }

    /* Owned for every exit: a missing file throws out of
     * openRead, and the letterbox or play() can throw too; ~Movie stops the
     * audio thread and releases the decoder and plane texture. */
    std::unique_ptr<Movie> movie(new Movie(skippable));
    MovieOpenHandler handler(movie->source.ops);
    shState->fileSystem().openRead(handler, filename);
    float volume = volume_ * 0.01f;
    
    if (movie->preparePlayback()) {        
        // Currently this stretches to fit the screen. VX Ace behavior is to center it and let the edges run off
        double ratio = std::min((double)width() / movie->videoW, (double)height() / movie->videoH);
        MovieFrame movieFrame(*movie, FloatRect((int) ((width() / 2) - (movie->videoW * ratio / 2)),
                                                (int) ((height() / 2) - (movie->videoH * ratio / 2)),
                                                movie->videoW * ratio, movie->videoH * ratio));
        
        Sprite letterboxSprite;
        Bitmap letterbox(width(), height());
        letterbox.fillRect(0, 0, width(), height(), Vec4(0,0,0,255));
        letterboxSprite.setBitmap(&letterbox);
        
        letterboxSprite.setZ(4999);

        /* update() early-returns while p->frozen, so a movie played in a
         * frozen screen state would spin out with audio only and a stale,
         * often black, frame on display. Present the
         * movie live; frozenScene stays captured, so a pending transition
         * still starts from the frame Graphics.freeze saved. */
        /* Frame skip would drop every redraw after the first: the movie loop
         * paces itself and, running a little behind 60 fps, always looks late
         * to the limiter, so only frame one reached the screen on device
         * Present every decoded frame, then restore --
         * on an exception too. */
        struct Restore
        {
            GraphicsPrivate *p;
            const bool frozen, skipping;
            ~Restore()
            {
                p->fpsLimiter.resetFrameAdjust();
                p->useFrameSkip = skipping;
                p->frozen = frozen;
            }
        } restore = { p, p->frozen, p->useFrameSkip };
        p->frozen = false;
        p->useFrameSkip = false;
        p->fpsLimiter.resetFrameAdjust();
        movie->play(volume);
    }
}

void Graphics::screenshot(const char *filename) {
    p->threadData->rqWindowAdjust.wait();
    Bitmap *ss = snapToBitmap();
    ss->saveToFile(filename);
    ss->dispose();
    delete ss;
}

DEF_ATTR_RD_SIMPLE(Graphics, Brightness, int, p->brightness)

void Graphics::setBrightness(int value) {
    value = clamp(value, 0, 255);
    
    if (p->brightness == value)
        return;
    
    p->brightness = value;
    p->screen.setBrightness(value / 255.0);
}

void Graphics::reset() {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    if (vita_measure_mode) vita_measure_reset();
    if (vita_glue_frame_profile_interval) FrameProfile::state.restart();
#endif
    /* Dispose all live Disposables */
    IntruListLink<Disposable> *iter;
    
    for (iter = p->dispList.begin(); iter != p->dispList.end();
         iter = iter->next) {
        iter->data->dispose();
    }
    
    p->dispList.clear();
    
    /* Reset attributes (frame count not included) */
    p->fpsLimiter.resetFrameAdjust();
    p->frozen = false;
    p->screen.getPP().clearBuffers();
    
    setFrameRate(DEF_FRAMERATE);
    setBrightness(255);
    
    // Always update at least once to clear the screen
    if (p->threadData->rqResetFinish)
        update();
    else
        repaintWait(p->threadData->rqResetFinish, false);
    p->threadData->rqReset.clear();
}

void Graphics::center() {
    p->threadData->rqWindowAdjust.wait();
    if (getFullscreen())
        return;
    
    p->threadData->ethread->requestWindowCenter();
}

bool Graphics::getFullscreen() const {
    return p->threadData->ethread->getFullscreen();
}

void Graphics::setFullscreen(bool value) {
    p->threadData->ethread->requestFullscreenMode(value);
}

bool Graphics::getShowCursor() const {
    return p->threadData->ethread->getShowCursor();
}

void Graphics::setShowCursor(bool value) {
    p->threadData->ethread->requestShowCursor(value);
}

bool Graphics::getFixedAspectRatio() const
{
    // It's a bit hacky to expose config values as a Graphics
    // attribute, but there's really no point in state duplication
    return shState->config().fixedAspectRatio;
}

void Graphics::setFixedAspectRatio(bool value)
{
    shState->config().fixedAspectRatio = value;
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->findHighestIntegerScale();
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);
}

int Graphics::getSmoothScaling() const
{
    // Same deal as with fixed aspect ratio
    return shState->config().smoothScaling;
}

void Graphics::setSmoothScaling(int value)
{
    shState->config().smoothScaling = value;
}

bool Graphics::getIntegerScaling() const
{
    return p->integerScaleActive;
}

void Graphics::setIntegerScaling(bool value)
{
    p->integerScaleActive = value;
    p->findHighestIntegerScale();
    p->rebuildIntegerScaleBuffer();
    
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);
}

bool Graphics::getLastMileScaling() const
{
    return p->integerLastMileScaling;
}

void Graphics::setLastMileScaling(bool value)
{
    p->integerLastMileScaling = value;
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);
}

bool Graphics::getThreadsafe() const
{
    return p->multithreadedMode;
}

void Graphics::setThreadsafe(bool value)
{
    p->multithreadedMode = value;
}

double Graphics::getScale() const {
    p->checkResize();
    return (double)(p->winSize.y / p->backingScaleFactor) / p->scRes.y;
    
}

void Graphics::setScale(double factor) {
    p->threadData->rqWindowAdjust.wait();
    factor = clamp(factor, 0.5, 4.0);
    
    if (factor == getScale())
        return;
    
    int widthpx = p->scRes.x * factor;
    int heightpx = p->scRes.y * factor;
    
    shState->eThread().requestWindowResize(widthpx, heightpx);
}

bool Graphics::getFrameskip() const { return p->useFrameSkip; }

void Graphics::setFrameskip(bool value) { p->useFrameSkip = value; }

Scene *Graphics::getScreen() const { return &p->screen; }

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
void Graphics::repaintSettings() {
    p->checkShutDownReset();
    p->checkSyncLock();
    TEXFBO &lastFrame = p->screen.getPP().frontBuffer();
    int scaling = GLMeta::blitScaleIsSpecial(p->integerScaleBuffer, false,
        IntRect(0, 0, p->scSize.x, p->scSize.y), lastFrame,
        IntRect(0, 0, p->scRes.x, p->scRes.y));
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileBlit6(FrameProfile::Blit);
#endif
    GLMeta::blitBeginScreen(p->winSize, scaling);
    GLMeta::blitSource(lastFrame, scaling);
    FBO::clear();
    p->metaBlitBufferFlippedScaled(scaling);
    GLMeta::blitEnd();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileBlit6.stop();
#endif
    p->drawOverlay();
    // Real swaps retire resources, but paused time is not RGSS frameCount/FPS.
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileIdle(FrameProfile::Idle);
#endif
        SDL_Delay(16);
    }
    GPUBudget::swapBegin();
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileSwap(FrameProfile::Swap);
#endif
        SDL_GL_SwapWindow(p->threadData->window);
    }
    GPUBudget::swapEnd();
    GPUBudget::frameAdvance();
}

void Graphics::runSettingsMenu(const VitaSettingsInput &initial) {
    VitaSettingsHandoff &handoff = p->threadData->ethread->settingsMenu;
    const bool visible = p->overlayVisible;
    const int lineCount = p->overlayLineCount;
    char saved[OVERLAY_MAX_LINES][OVERLAY_MAX_CHARS + 1];
    memcpy(saved, p->overlayLines, sizeof(saved));
    auto restore = [&]() {
        p->overlaySettings = false;
        const char *lines[OVERLAY_MAX_LINES];
        for (int i = 0; i < OVERLAY_MAX_LINES; ++i) lines[i] = saved[i];
        overlaySetLines(lines, lineCount);
        overlaySetVisible(visible);
        handoff.finish();
        // resetFrameAdjust alone still leaves the old skip debt until delay().
        p->fpsLimiter.lastTickCount = p->fpsLimiter.adj.last = SDL_GetPerformanceCounter();
        p->fpsLimiter.adj.idealDiff = 0;
        p->fpsLimiter.resetFrameAdjust();
        p->last_update = p->last_avg_update = shState->runTime();
    };
    try {
        p->overlaySettings = true;
        BDescVec bindings;
        p->threadData->bindingUpdateMsg.get(bindings);
        VitaSettingsMenu menu(bindings, initial);
        VitaSettingsInput input = initial, next;
        while (!menu.done()) {
            p->checkShutDownReset();
            for (int i = 0; i < 32 && handoff.next(next); ++i) {
                if (!next.acceptRequest) input = next;
                menu.sample(next, p->threadData->config);
            }
            if (menu.wantsSave() && menu.persist(p->threadData->config))
                p->threadData->bindingUpdateMsg.post(menu.bindings());
            overlaySetLines(menu.render(), 12);
            overlaySetVisible(true);
            repaintSettings();
        }
        // Consume the dismissal release; bound a stuck controller without starving reset/quit.
        const Uint64 until = SDL_GetTicks64() + 750;
        while (!input.neutral(JAXIS_THRESHOLD) && SDL_GetTicks64() < until) {
            for (int i = 0; i < 32 && handoff.next(next); ++i)
                if (!next.acceptRequest) input = next;
            repaintSettings();
        }
    } catch (...) {
        restore();
        throw;
    }
    restore();
}
#endif

void Graphics::repaintWait(const AtomicFlag &exitCond, bool checkReset) {
    if (exitCond)
        return;
    
    /* Repaint the screen with the last good frame we drew */
    TEXFBO &lastFrame = p->screen.getPP().frontBuffer();

    int scaleIsSpecial = GLMeta::blitScaleIsSpecial(p->integerScaleBuffer, false, IntRect(0, 0, p->scSize.x, p->scSize.y), lastFrame, IntRect(0, 0, p->scRes.x, p->scRes.y));

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileBlit7(FrameProfile::Blit);
#endif
    GLMeta::blitBeginScreen(p->winSize, scaleIsSpecial);
    GLMeta::blitSource(lastFrame, scaleIsSpecial);
    
    while (!exitCond) {
        shState->checkShutdown();
        
        if (checkReset)
            shState->checkReset();
        
        FBO::clear();
        p->metaBlitBufferFlippedScaled(scaleIsSpecial);
        {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileSwap(FrameProfile::Swap);
#endif
            SDL_GL_SwapWindow(p->threadData->window);
        }
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* A real flip that does not go through swapGLBuffer(). */
        GPUBudget::frameAdvance();
#endif
        {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileIdle(FrameProfile::Idle);
#endif
            p->fpsLimiter.delay();
        }

        p->threadData->ethread->notifyFrame();
    }
    
    GLMeta::blitEnd();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileBlit7.stop();
#endif
}

void Graphics::lock(bool force) {
    p->setLock(force);
}

void Graphics::unlock(bool force) {
    p->releaseLock(force);
}

void Graphics::addDisposable(Disposable *d) { p->dispList.append(d->link); }

void Graphics::remDisposable(Disposable *d) { p->dispList.remove(d->link); }

/* ---- The persistent on-screen overlay ------------------
 *
 * The whole public surface: three calls, no GL object, no allocation. See
 * graphics.h for the contract and GraphicsPrivate::drawOverlay for what a
 * frame actually costs.
 *
 * Without MKXPZ_SOFTWARE_BITMAPS there is no boot-reserved texture to draw
 * into, so these are no-ops -- declared unconditionally so their callers
 * need no #ifdef around their own code. */

void Graphics::overlaySetLines(const char *const *lines, int count)
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    p->overlaySetLines(lines, count);
#else
    (void)lines;
    (void)count;
#endif
}

void Graphics::overlaySetVisible(bool visible)
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* Visibility is part of the same one-writer object as the lines; the
     * rule itself lives in GraphicsPrivate::overlayWritableHere. */
    if (p->overlayWritableHere("overlaySetVisible"))
        p->overlayVisible = visible;
#else
    (void)visible;
#endif
}

bool Graphics::overlayIsVisible() const
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    return p->overlayVisible;
#else
    return false;
#endif
}

#undef GRAPHICS_THREAD_LOCK
