/**
 * TheoraPlay; multithreaded Ogg Theora/Ogg Vorbis decoding.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

// I wrote this with a lot of peeking at the Theora example code in
//  libtheora-1.1.1/examples/player_example.c, but this is all my own
//  code.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
#include <windows.h>
#define THEORAPLAY_THREAD_T    HANDLE
#define THEORAPLAY_MUTEX_T     HANDLE
#define sleepms(x) Sleep(x)
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
extern int pte_osThreadGetPriority(SceUID handle);
#endif
#define sleepms(x) usleep((x) * 1000)
#define THEORAPLAY_THREAD_T    pthread_t
#define THEORAPLAY_MUTEX_T     pthread_mutex_t
#endif

#include "theoraplay.h"
#include "theora/theoradec.h"
#include "vorbis/codec.h"

#define THEORAPLAY_INTERNAL 1

// Audio is decoded until it is this far past the newest frame, reading
//  pages ahead of the video (past a full video queue too) in bursts that
//  start below half of, and stop at, this much buffered Theora data.
#define THEORAPLAY_AUDIO_AHEAD_MS 1000
#define THEORAPLAY_READ_AHEAD_BYTES (4L << 20)
// mkxp-z: every queue shares the heap with the game. Queued
//  frames are held to this many bytes (maxframes is only a ceiling), a coded
//  frame over this many pixels is refused before anything is allocated, and
//  decoded audio nobody has taken yet stops growing past this many bytes.
#define THEORAPLAY_VIDEO_QUEUE_BYTES (16UL << 20)
#define THEORAPLAY_MAX_FRAME_PIXELS (1920UL * 1088UL)
#define THEORAPLAY_AUDIO_QUEUE_BYTES (4UL << 20)
#define THEORAPLAY_WORKER_STACK_BYTES (256 * 1024)

typedef THEORAPLAY_VideoFrame VideoFrame;
typedef THEORAPLAY_AudioPacket AudioPacket;

// !!! FIXME: these all count on the pixel format being TH_PF_420 for now.

typedef unsigned char *(*ConvertVideoFrameFn)(const th_info *tinfo,
                                              const th_ycbcr_buffer ycbcr);

static unsigned char *ConvertVideoFrame420ToYUVPlanar(
                            const th_info *tinfo, const th_ycbcr_buffer ycbcr,
                            const int p0, const int p1, const int p2)
{
    int i;
    const int w = tinfo->pic_width;
    const int h = tinfo->pic_height;
    const int yoff = (tinfo->pic_x & ~1) + ycbcr[0].stride * (tinfo->pic_y & ~1);
    const int uvoff = (tinfo->pic_x / 2) + (ycbcr[1].stride) * (tinfo->pic_y / 2);
    unsigned char *yuv = (unsigned char *) malloc(w * h * 2);
    const unsigned char *p0data = ycbcr[p0].data + yoff;
    const int p0stride = ycbcr[p0].stride;
    const unsigned char *p1data = ycbcr[p1].data + uvoff;
    const int p1stride = ycbcr[p1].stride;
    const unsigned char *p2data = ycbcr[p2].data + uvoff;
    const int p2stride = ycbcr[p2].stride;

    if (yuv)
    {
        unsigned char *dst = yuv;
        for (i = 0; i < h; i++, dst += w)
            memcpy(dst, p0data + (p0stride * i), w);
        for (i = 0; i < (h / 2); i++, dst += w/2)
            memcpy(dst, p1data + (p1stride * i), w / 2);
        for (i = 0; i < (h / 2); i++, dst += w/2)
            memcpy(dst, p2data + (p2stride * i), w / 2);
    } // if

    return yuv;
} // ConvertVideoFrame420ToYUVPlanar


static unsigned char *ConvertVideoFrame420ToYV12(const th_info *tinfo,
                                                 const th_ycbcr_buffer ycbcr)
{
    return ConvertVideoFrame420ToYUVPlanar(tinfo, ycbcr, 0, 2, 1);
} // ConvertVideoFrame420ToYV12


static unsigned char *ConvertVideoFrame420ToIYUV(const th_info *tinfo,
                                                 const th_ycbcr_buffer ycbcr)
{
    return ConvertVideoFrame420ToYUVPlanar(tinfo, ycbcr, 0, 1, 2);
} // ConvertVideoFrame420ToIYUV


// mkxp-z: the planes as one RGBA texture wants them (four samples a texel),
//  so the renderer uploads the frame as-is and converts it on the GPU.
static unsigned char *ConvertVideoFrame420ToYUVTEX(const th_info *tinfo,
                                                   const th_ycbcr_buffer ycbcr)
{
    int i;
    const int w = tinfo->pic_width;
    const int h = tinfo->pic_height;
    const int halfw = w / 2;
    const int pitch = (int) THEORAPLAY_YUVTEX_PITCH(w);
    const int halfpitch = pitch / 2;
    const int yoff = (tinfo->pic_x & ~1) + ycbcr[0].stride * (tinfo->pic_y & ~1);
    const int uvoff = (tinfo->pic_x / 2) + (ycbcr[1].stride) * (tinfo->pic_y / 2);
    unsigned char *yuv = (unsigned char *) malloc((size_t) pitch * (size_t) (h + h / 2));

    if (yuv)
    {
        unsigned char *dst = yuv;
        for (i = 0; i < h; i++, dst += pitch)
        {
            memcpy(dst, ycbcr[0].data + yoff + (ycbcr[0].stride * i), w);
            memset(dst + w, 0, pitch - w);
        } // for
        for (i = 0; i < (h / 2); i++, dst += pitch)
        {
            memcpy(dst, ycbcr[1].data + uvoff + (ycbcr[1].stride * i), halfw);
            memset(dst + halfw, 0, halfpitch - halfw);
            memcpy(dst + halfpitch, ycbcr[2].data + uvoff + (ycbcr[2].stride * i), halfw);
            memset(dst + halfpitch + halfw, 0, halfpitch - halfw);
        } // for
    } // if

    return yuv;
} // ConvertVideoFrame420ToYUVTEX


// RGB
#define THEORAPLAY_CVT_FNNAME_420 ConvertVideoFrame420ToRGB
#define THEORAPLAY_CVT_RGB_ALPHA 0
#include "theoraplay_cvtrgb.h"
#undef THEORAPLAY_CVT_RGB_ALPHA
#undef THEORAPLAY_CVT_FNNAME_420

// RGBA
#define THEORAPLAY_CVT_FNNAME_420 ConvertVideoFrame420ToRGBA
#define THEORAPLAY_CVT_RGB_ALPHA 1
#include "theoraplay_cvtrgb.h"
#undef THEORAPLAY_CVT_RGB_ALPHA
#undef THEORAPLAY_CVT_FNNAME_420


typedef struct TheoraDecoder
{
    // Thread wrangling...
    int thread_created;
    THEORAPLAY_MUTEX_T lock;
    volatile int halt;
    int thread_done;
    THEORAPLAY_THREAD_T worker;

    // API state...
    THEORAPLAY_Io *io;
    unsigned int maxframes;  // Max video frames to buffer.
    volatile unsigned int prepped;
    volatile unsigned int videocount;  // currently buffered frames.
    volatile unsigned int audioms;  // currently buffered audio samples.
    volatile unsigned long audiobytes;  // mkxp-z: sample bytes queued.
    volatile int hasvideo;
    volatile int hasaudio;
    volatile int decode_error;

    THEORAPLAY_VideoFormat vidfmt;
    ConvertVideoFrameFn vidcvt;

    VideoFrame *videolist;
    VideoFrame *videolisttail;

    AudioPacket *audiolist;
    AudioPacket *audiolisttail;
} TheoraDecoder;


#ifdef _WIN32
static inline int Thread_Create(TheoraDecoder *ctx, void *(*routine) (void*))
{
    ctx->worker = CreateThread(
        NULL,
        0,
        (LPTHREAD_START_ROUTINE) routine,
        (LPVOID) ctx,
        0,
        NULL
    );
    return (ctx->worker == NULL);
}
static inline void Thread_Join(THEORAPLAY_THREAD_T thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
static inline int Mutex_Create(TheoraDecoder *ctx)
{
    ctx->lock = CreateMutex(NULL, FALSE, NULL);
    return (ctx->lock == NULL);
}
static inline void Mutex_Destroy(THEORAPLAY_MUTEX_T mutex)
{
    CloseHandle(mutex);
}
static inline void Mutex_Lock(THEORAPLAY_MUTEX_T mutex)
{
    WaitForSingleObject(mutex, INFINITE);
}
static inline void Mutex_Unlock(THEORAPLAY_MUTEX_T mutex)
{
    ReleaseMutex(mutex);
}
#else
// mkxp-z: an explicit stack. On Vita the priority is set
//  explicitly too: pthread-embedded resolves INHERIT_SCHED through a record
//  that holds 0 for the SDL-started rgss thread.
static inline int Thread_Create(TheoraDecoder *ctx, void *(*routine) (void*))
{
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0)
        return rc;
    rc = pthread_attr_setstacksize(&attr, THEORAPLAY_WORKER_STACK_BYTES);
#ifdef __vita__
    if (rc == 0)
    {
        struct sched_param param;
        const int low = sched_get_priority_min(SCHED_OTHER);
        const int high = sched_get_priority_max(SCHED_OTHER);
        int priority = pte_osThreadGetPriority(sceKernelGetThreadId());
        if (priority < low) priority = low;
        if (priority > high) priority = high;
        param.sched_priority = priority;
        rc = pthread_attr_setschedparam(&attr, &param);
        if (rc == 0)
            rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    } // if
#endif
    if (rc == 0)
        rc = pthread_create(&ctx->worker, &attr, routine, ctx);
    pthread_attr_destroy(&attr);
    return rc;
}
static inline void Thread_Join(THEORAPLAY_THREAD_T thread)
{
    pthread_join(thread, NULL);
}
static inline int Mutex_Create(TheoraDecoder *ctx)
{
    return pthread_mutex_init(&ctx->lock, NULL);
}
static inline void Mutex_Destroy(THEORAPLAY_MUTEX_T mutex)
{
    pthread_mutex_destroy(&mutex);
}
static inline void Mutex_Lock(THEORAPLAY_MUTEX_T mutex)
{
    pthread_mutex_lock(&mutex);
}
static inline void Mutex_Unlock(THEORAPLAY_MUTEX_T mutex)
{
    pthread_mutex_unlock(&mutex);
}
#endif


static int FeedMoreOggData(THEORAPLAY_Io *io, ogg_sync_state *sync)
{
    long buflen = 4096;
    char *buffer = ogg_sync_buffer(sync, buflen);
    if (buffer == NULL)
        return -1;

    buflen = io->read(io, buffer, buflen);
    if (buflen <= 0)
        return 0;

    return (ogg_sync_wrote(sync, buflen) == 0) ? 1 : -1;
} // FeedMoreOggData


// This massive function is where all the effort happens.
static void WorkerThread(TheoraDecoder *ctx)
{
    // make sure we initialized the stream before using pagein, but the stream
    //  will know to ignore pages that aren't meant for it, so pass to both.
    #define queue_ogg_page(ctx) do { \
        if (tpackets) ogg_stream_pagein(&tstream, &page); \
        if (vpackets) ogg_stream_pagein(&vstream, &page); \
    } while (0)
    // Audio short of its lead past the newest frame; Theora bytes read ahead.
    #define audio_behind() (vpackets && !audio_eos && \
        (unsigned long long) audioframes * 1000 < \
        (unsigned long long) (videoms + THEORAPLAY_AUDIO_AHEAD_MS) * vinfo.rate)
    #define theora_buffered() (tstream.body_fill - tstream.body_returned)
    #define vorbis_buffered() (vstream.body_fill - vstream.body_returned)
    // Decoded audio still to come: PCM in the DSP or a packet in the stream.
    #define audio_left() (vpackets && (vorbis_synthesis_pcmout(&vdsp, NULL) > 0 || \
        ogg_stream_packetpeek(&vstream, NULL) == 1))

    unsigned long audioframes = 0;
    double fps = 0.0;
    int was_error = 1;  // resets to 0 at the end.
    int eos = 0;  // end of stream flag.
    int eof = 0, audio_eos = 0;  // end of file / of the Vorbis stream.
    int video_eos = 0;  // the Theora stream's last packet was decoded.
    unsigned long videoms = 0;  // newest decoded frame.

    // Too much Ogg/Vorbis/Theora state...
    ogg_packet packet;
    ogg_sync_state sync;
    ogg_page page;
    int vpackets = 0;
    vorbis_info vinfo;
    vorbis_comment vcomment;
    ogg_stream_state vstream;
    int vdsp_init = 0;
    vorbis_dsp_state vdsp;
    int tpackets = 0;
    th_info tinfo;
    th_comment tcomment;
    ogg_stream_state tstream;
    int vblock_init = 0;
    vorbis_block vblock;
    th_dec_ctx *tdec = NULL;
    th_setup_info *tsetup = NULL;

    ogg_sync_init(&sync);
    vorbis_info_init(&vinfo);
    vorbis_comment_init(&vcomment);
    th_comment_init(&tcomment);
    th_info_init(&tinfo);

    int bos = 1;
    while (!ctx->halt && bos)
    {
        if (FeedMoreOggData(ctx->io, &sync) <= 0)
            goto cleanup;

        // parse out the initial header.
        while ( (!ctx->halt) && (ogg_sync_pageout(&sync, &page) > 0) )
        {
            ogg_stream_state test;

            if (!ogg_page_bos(&page))  // not a header.
            {
                queue_ogg_page(ctx);
                bos = 0;
                break;
            } // if

            ogg_stream_init(&test, ogg_page_serialno(&page));
            ogg_stream_pagein(&test, &page);
            ogg_stream_packetout(&test, &packet);

            if (!tpackets && (th_decode_headerin(&tinfo, &tcomment, &tsetup, &packet) >= 0))
            {
                memcpy(&tstream, &test, sizeof (test));
                tpackets = 1;
            } // if
            else if (!vpackets && (vorbis_synthesis_headerin(&vinfo, &vcomment, &packet) >= 0))
            {
                memcpy(&vstream, &test, sizeof (test));
                vpackets = 1;
            } // else if
            else
            {
                // whatever it is, we don't care about it
                ogg_stream_clear(&test);
            } // else
        } // while
    } // while

    // no audio OR video?
    if (ctx->halt || (!vpackets && !tpackets))
        goto cleanup;

    // apparently there are two more theora and two more vorbis headers next.
    while ((!ctx->halt) && ((tpackets && (tpackets < 3)) || (vpackets && (vpackets < 3))))
    {
        while (!ctx->halt && tpackets && (tpackets < 3))
        {
            if (ogg_stream_packetout(&tstream, &packet) != 1)
                break; // get more data?
            if (!th_decode_headerin(&tinfo, &tcomment, &tsetup, &packet))
                goto cleanup;
            tpackets++;
        } // while

        while (!ctx->halt && vpackets && (vpackets < 3))
        {
            if (ogg_stream_packetout(&vstream, &packet) != 1)
                break;  // get more data?
            if (vorbis_synthesis_headerin(&vinfo, &vcomment, &packet))
                goto cleanup;
            vpackets++;
        } // while

        // get another page, try again?
        if (ogg_sync_pageout(&sync, &page) > 0)
            queue_ogg_page(ctx);
        else if (FeedMoreOggData(ctx->io, &sync) <= 0)
            goto cleanup;
    } // while

    // okay, now we have our streams, ready to set up decoding.
    if (!ctx->halt && tpackets)
    {
        // th_decode_alloc() docs say to check for insanely large frames yourself.
        if ((tinfo.frame_width > 99999) || (tinfo.frame_height > 99999))
            goto cleanup;

        // We treat "unspecified" as NTSC. *shrug*
        // mkxp-z: asserts are live in the player, so an unsupported stream
        //  ends the decode (the movie is skipped) instead of the process.
        if ( (tinfo.colorspace != TH_CS_UNSPECIFIED) &&
             (tinfo.colorspace != TH_CS_ITU_REC_470M) &&
             (tinfo.colorspace != TH_CS_ITU_REC_470BG) )
            goto cleanup;  // !!! FIXME: unsupported colorspace.

        if (tinfo.pixel_fmt != TH_PF_420) goto cleanup; // !!! FIXME

        if ((unsigned long long) tinfo.frame_width * tinfo.frame_height >
                THEORAPLAY_MAX_FRAME_PIXELS)
            goto cleanup;

        {
            const unsigned long long w = tinfo.pic_width, h = tinfo.pic_height;
            unsigned long long frame_bytes = w * h * 2;  // YV12/IYUV
            if (ctx->vidfmt == THEORAPLAY_VIDFMT_YUVTEX)
                frame_bytes = THEORAPLAY_YUVTEX_PITCH(w) * (h + h / 2);
            else if (ctx->vidfmt == THEORAPLAY_VIDFMT_RGB)
                frame_bytes = w * h * 3;
            else if (ctx->vidfmt == THEORAPLAY_VIDFMT_RGBA)
                frame_bytes = w * h * 4;
            if (frame_bytes && ctx->maxframes * frame_bytes > THEORAPLAY_VIDEO_QUEUE_BYTES)
            {
                const unsigned long long fit = THEORAPLAY_VIDEO_QUEUE_BYTES / frame_bytes;
                ctx->maxframes = fit < 2 ? 2 : (unsigned int) fit;
            } // if
        }

        if (tinfo.fps_denominator != 0)
            fps = ((double) tinfo.fps_numerator) / ((double) tinfo.fps_denominator);

        tdec = th_decode_alloc(&tinfo, tsetup);
        if (!tdec) goto cleanup;

        // Set decoder to maximum post-processing level.
        //  Theoretically we could try dropping this level if we're not keeping up.
        int pp_level_max = 0;
        // !!! FIXME: maybe an API to set this?
        //th_decode_ctl(tdec, TH_DECCTL_GET_PPLEVEL_MAX, &pp_level_max, sizeof(pp_level_max));
        th_decode_ctl(tdec, TH_DECCTL_SET_PPLEVEL, &pp_level_max, sizeof(pp_level_max));
    } // if

    // Done with this now.
    if (tsetup != NULL)
    {
        th_setup_free(tsetup);
        tsetup = NULL;
    } // if

    if (!ctx->halt && vpackets)
    {
        vdsp_init = (vorbis_synthesis_init(&vdsp, &vinfo) == 0);
        if (!vdsp_init)
            goto cleanup;
        vblock_init = (vorbis_block_init(&vdsp, &vblock) == 0);
        if (!vblock_init)
            goto cleanup;
    } // if

    // Now we can start the actual decoding!
    // Note that audio and video don't _HAVE_ to start simultaneously.

    Mutex_Lock(ctx->lock);
    ctx->prepped = 1;
    ctx->hasvideo = (tpackets != 0);
    ctx->hasaudio = (vpackets != 0);
    Mutex_Unlock(ctx->lock);

    while (!ctx->halt && !eos)
    {
        int need_pages = 0;  // need more Ogg pages?
        int video_dry = !tpackets;  // no Theora packet left to decode?

        // Audio does not wait for the video queue: muxers write each audio
        //  page up to ~1 s after the video it covers.
        Mutex_Lock(ctx->lock);
        const int video_full = tpackets && (ctx->videocount >= ctx->maxframes);
        const int audio_full = vpackets && (ctx->audiobytes >= THEORAPLAY_AUDIO_QUEUE_BYTES);
        Mutex_Unlock(ctx->lock);
        const int audio_short = tpackets && !eof && !audio_full && audio_behind() &&
            (theora_buffered() < THEORAPLAY_READ_AHEAD_BYTES / 2);
        // mkxp-z: with the decoded audio queue full, undecoded audio waits
        //  in the Vorbis stream; stop reading once nothing else needs pages.
        if (audio_full && (video_full || video_eos || !tpackets ||
                           vorbis_buffered() >= THEORAPLAY_READ_AHEAD_BYTES))
        {
            sleepms(10);
            continue;
        } // if
        if (video_full && !audio_short)
        {
            sleepms(10);
            continue;
        } // if

        // Try to read as much audio as we can at once. We limit the outer
        //  loop to one video frame and as much audio as we can eat.
        while (!ctx->halt && vpackets)
        {
            float **pcm = NULL;
            const int frames = vorbis_synthesis_pcmout(&vdsp, &pcm);
            if (frames > 0)
            {
                Mutex_Lock(ctx->lock);
                const int full = ctx->audiobytes >= THEORAPLAY_AUDIO_QUEUE_BYTES;
                Mutex_Unlock(ctx->lock);
                if (full)
                    break;  // the PCM stays in the DSP until there is room.

                const int channels = vinfo.channels;
                int chanidx, frameidx;
                float *samples;
                AudioPacket *item = (AudioPacket *) malloc(sizeof (AudioPacket));
                if (item == NULL) goto cleanup;
                item->playms = (unsigned long) ((((double) audioframes) / ((double) vinfo.rate)) * 1000.0);
                item->channels = channels;
                item->freq = vinfo.rate;
                item->frames = frames;
                item->samples = (float *) malloc(sizeof (float) * frames * channels);
                item->next = NULL;

                if (item->samples == NULL)
                {
                    free(item);
                    goto cleanup;
                } // if

                // I bet this beats the crap out of the CPU cache...
                samples = item->samples;
                for (frameidx = 0; frameidx < frames; frameidx++)
                {
                    for (chanidx = 0; chanidx < channels; chanidx++)
                        *(samples++) = pcm[chanidx][frameidx];
                } // for

                vorbis_synthesis_read(&vdsp, frames);  // we ate everything.
                audioframes += frames;

                //printf("Decoded %d frames of audio.\n", (int) frames);
                Mutex_Lock(ctx->lock);
                ctx->audioms += item->playms;
                ctx->audiobytes += sizeof (float) * frames * channels;
                if (ctx->audiolisttail)
                {
                    assert(ctx->audiolist);
                    ctx->audiolisttail->next = item;
                } // if
                else
                {
                    assert(!ctx->audiolist);
                    ctx->audiolist = item;
                } // else
                ctx->audiolisttail = item;
                Mutex_Unlock(ctx->lock);
            } // if

            else  // no audio available left in current packet?
            {
                // try to feed another packet to the Vorbis stream...
                if (ogg_stream_packetout(&vstream, &packet) <= 0)
                {
                    if (!tpackets)
                        need_pages = 1; // no video, get more pages now.
                    break;  // we'll get more pages when the video catches up.
                } // if
                else
                {
                    audio_eos |= packet.e_o_s;
                    if (vorbis_synthesis(&vblock, &packet) == 0)
                        vorbis_synthesis_blockin(&vdsp, &vblock);
                } // else
            } // else
        } // while

        if (!ctx->halt && tpackets && !video_full)
        {
            // Theora, according to example_player.c, is
            //  "one [packet] in, one [frame] out."
            if (ogg_stream_packetout(&tstream, &packet) <= 0)
                need_pages = video_dry = 1;
            else
            {
                ogg_int64_t granulepos = 0;
                video_eos |= packet.e_o_s;

                // you have to guide the Theora decoder to get meaningful timestamps, apparently.  :/
                if (packet.granulepos >= 0)
                    th_decode_ctl(tdec, TH_DECCTL_SET_GRANPOS, &packet.granulepos, sizeof(packet.granulepos));

                if (th_decode_packetin(tdec, &packet, &granulepos) == 0)  // new frame!
                {
                    th_ycbcr_buffer ycbcr;
                    if (th_decode_ycbcr_out(tdec, ycbcr) == 0)
                    {
                        const double videotime = th_granule_time(tdec, granulepos);
                        VideoFrame *item = (VideoFrame *) malloc(sizeof (VideoFrame));
                        if (item == NULL) goto cleanup;
                        item->playms = (unsigned int) (videotime * 1000.0);
                        item->fps = fps;
                        item->width = tinfo.pic_width;
                        item->height = tinfo.pic_height;
                        item->format = ctx->vidfmt;
                        item->pixels = ctx->vidcvt(&tinfo, ycbcr);
                        item->next = NULL;

                        if (item->pixels == NULL)
                        {
                            free(item);
                            goto cleanup;
                        } // if

                        videoms = item->playms;  // before the consumer can free it
                        //printf("Decoded another video frame.\n");
                        Mutex_Lock(ctx->lock);
                        if (ctx->videolisttail)
                        {
                            assert(ctx->videolist);
                            ctx->videolisttail->next = item;
                        } // if
                        else
                        {
                            assert(!ctx->videolist);
                            ctx->videolist = item;
                        } // else
                        ctx->videolisttail = item;
                        ctx->videocount++;
                        Mutex_Unlock(ctx->lock);
                    } // if
                } // if
            } // else
        } // if

        if (!ctx->halt && need_pages)
        {
            const int rc = FeedMoreOggData(ctx->io, &sync);
            if (rc == 0)
            {
                eof = 1;
                // end of stream once video has drained too, and audio held
                //  back by a full queue has been decoded.
                eos = video_dry && !audio_left();
            } // if
            else if (rc < 0)
                goto cleanup;  // i/o error, etc.
            else
            {
                while (!ctx->halt && (ogg_sync_pageout(&sync, &page) > 0))
                    queue_ogg_page(ctx);
            } // else
        } // if

        // One burst up to the next audio packet: libogg moves the buffered
        //  Theora data on each page after a decode, so pages are not
        //  trickled in between frames.
        if (audio_short && audio_behind())
        {
            while (!ctx->halt && !eof && (ogg_stream_packetpeek(&vstream, NULL) != 1) &&
                   (theora_buffered() < THEORAPLAY_READ_AHEAD_BYTES))
            {
                const int rc = FeedMoreOggData(ctx->io, &sync);
                if (rc == 0)
                    eof = 1;
                else if (rc < 0)
                    goto cleanup;  // i/o error, etc.
                else
                {
                    while (!ctx->halt && (ogg_sync_pageout(&sync, &page) > 0))
                        queue_ogg_page(ctx);
                } // else
            } // while
        } // if
    } // while

    was_error = 0;

cleanup:
    ctx->decode_error = (!ctx->halt && was_error);
    if (tdec != NULL) th_decode_free(tdec);
    if (tsetup != NULL) th_setup_free(tsetup);
    if (vblock_init) vorbis_block_clear(&vblock);
    if (vdsp_init) vorbis_dsp_clear(&vdsp);
    if (tpackets) ogg_stream_clear(&tstream);
    if (vpackets) ogg_stream_clear(&vstream);
    th_info_clear(&tinfo);
    th_comment_clear(&tcomment);
    vorbis_comment_clear(&vcomment);
    vorbis_info_clear(&vinfo);
    ogg_sync_clear(&sync);
    ctx->io->close(ctx->io);
    ctx->thread_done = 1;
} // WorkerThread


static void *WorkerThreadEntry(void *_this)
{
    TheoraDecoder *ctx = (TheoraDecoder *) _this;
    WorkerThread(ctx);
    //printf("Worker thread is done.\n");
    return NULL;
} // WorkerThreadEntry


static long IoFopenRead(THEORAPLAY_Io *io, void *buf, long buflen)
{
    FILE *f = (FILE *) io->userdata;
    const size_t br = fread(buf, 1, buflen, f);
    if ((br == 0) && ferror(f))
        return -1;
    return (long) br;
} // IoFopenRead


static void IoFopenClose(THEORAPLAY_Io *io)
{
    FILE *f = (FILE *) io->userdata;
    fclose(f);
    free(io);
} // IoFopenClose


THEORAPLAY_Decoder *THEORAPLAY_startDecodeFile(const char *fname,
                                               const unsigned int maxframes,
                                               THEORAPLAY_VideoFormat vidfmt)
{
    THEORAPLAY_Io *io = (THEORAPLAY_Io *) malloc(sizeof (THEORAPLAY_Io));
    if (io == NULL)
        return NULL;

    FILE *f = fopen(fname, "rb");
    if (f == NULL)
    {
        free(io);
        return NULL;
    } // if

    io->read = IoFopenRead;
    io->close = IoFopenClose;
    io->userdata = f;
    return THEORAPLAY_startDecode(io, maxframes, vidfmt);
} // THEORAPLAY_startDecodeFile


THEORAPLAY_Decoder *THEORAPLAY_startDecode(THEORAPLAY_Io *io,
                                           const unsigned int maxframes,
                                           THEORAPLAY_VideoFormat vidfmt)
{
    TheoraDecoder *ctx = NULL;
    ConvertVideoFrameFn vidcvt = NULL;

    switch (vidfmt)
    {
        // !!! FIXME: current expects TH_PF_420.
        #define VIDCVT(t) case THEORAPLAY_VIDFMT_##t: vidcvt = ConvertVideoFrame420To##t; break;
        VIDCVT(YV12)
        VIDCVT(IYUV)
        VIDCVT(RGB)
        VIDCVT(RGBA)
        VIDCVT(YUVTEX)
        #undef VIDCVT
        default: goto startdecode_failed;  // invalid/unsupported format.
    } // switch

    ctx = (TheoraDecoder *) malloc(sizeof (TheoraDecoder));
    if (ctx == NULL)
        goto startdecode_failed;

    memset(ctx, '\0', sizeof (TheoraDecoder));
    ctx->maxframes = maxframes;
    ctx->vidfmt = vidfmt;
    ctx->vidcvt = vidcvt;
    ctx->io = io;

    if (Mutex_Create(ctx) == 0)
    {
        ctx->thread_created = (Thread_Create(ctx, WorkerThreadEntry) == 0);
        if (ctx->thread_created)
            return (THEORAPLAY_Decoder *) ctx;
    } // if

    Mutex_Destroy(ctx->lock);

startdecode_failed:
    io->close(io);
    free(ctx);
    return NULL;
} // THEORAPLAY_startDecode


void THEORAPLAY_stopDecode(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    if (!ctx)
        return;

    if (ctx->thread_created)
    {
        ctx->halt = 1;
        Thread_Join(ctx->worker);
        Mutex_Destroy(ctx->lock);
    } // if

    VideoFrame *videolist = ctx->videolist;
    while (videolist)
    {
        VideoFrame *next = videolist->next;
        free(videolist->pixels);
        free(videolist);
        videolist = next;
    } // while

    AudioPacket *audiolist = ctx->audiolist;
    while (audiolist)
    {
        AudioPacket *next = audiolist->next;
        free(audiolist->samples);
        free(audiolist);
        audiolist = next;
    } // while

    free(ctx);
} // THEORAPLAY_stopDecode


int THEORAPLAY_isDecoding(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    int retval = 0;
    if (ctx)
    {
        Mutex_Lock(ctx->lock);
        retval = ( ctx && (ctx->audiolist || ctx->videolist ||
                   (ctx->thread_created && !ctx->thread_done)) );
        Mutex_Unlock(ctx->lock);
    } // if
    return retval;
} // THEORAPLAY_isDecoding


#define GET_SYNCED_VALUE(typ, defval, decoder, member) \
    TheoraDecoder *ctx = (TheoraDecoder *) decoder; \
    typ retval = defval; \
    if (ctx) { \
        Mutex_Lock(ctx->lock); \
        retval = ctx->member; \
        Mutex_Unlock(ctx->lock); \
    } \
    return retval;

int THEORAPLAY_isInitialized(THEORAPLAY_Decoder *decoder)
{
    GET_SYNCED_VALUE(int, 0, decoder, prepped);
} // THEORAPLAY_isInitialized


int THEORAPLAY_hasVideoStream(THEORAPLAY_Decoder *decoder)
{
    GET_SYNCED_VALUE(int, 0, decoder, hasvideo);
} // THEORAPLAY_hasVideoStream


int THEORAPLAY_hasAudioStream(THEORAPLAY_Decoder *decoder)
{
    GET_SYNCED_VALUE(int, 0, decoder, hasaudio);
} // THEORAPLAY_hasAudioStream


unsigned int THEORAPLAY_availableVideo(THEORAPLAY_Decoder *decoder)
{
    GET_SYNCED_VALUE(unsigned int, 0, decoder, videocount);
} // THEORAPLAY_availableVideo


unsigned int THEORAPLAY_availableAudio(THEORAPLAY_Decoder *decoder)
{
    GET_SYNCED_VALUE(unsigned int, 0, decoder, audioms);
} // THEORAPLAY_availableAudio


int THEORAPLAY_decodingError(THEORAPLAY_Decoder *decoder)
{
    GET_SYNCED_VALUE(int, 0, decoder, decode_error);
} // THEORAPLAY_decodingError


const THEORAPLAY_AudioPacket *THEORAPLAY_getAudio(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    AudioPacket *retval;

    Mutex_Lock(ctx->lock);
    retval = ctx->audiolist;
    if (retval)
    {
        ctx->audioms -= retval->playms;
        ctx->audiobytes -= sizeof (float) * retval->frames * retval->channels;
        ctx->audiolist = retval->next;
        retval->next = NULL;
        if (ctx->audiolist == NULL)
            ctx->audiolisttail = NULL;
    } // if
    Mutex_Unlock(ctx->lock);

    return retval;
} // THEORAPLAY_getAudio


void THEORAPLAY_freeAudio(const THEORAPLAY_AudioPacket *_item)
{
    THEORAPLAY_AudioPacket *item = (THEORAPLAY_AudioPacket *) _item;
    if (item != NULL)
    {
        assert(item->next == NULL);
        free(item->samples);
        free(item);
    } // if
} // THEORAPLAY_freeAudio


const THEORAPLAY_VideoFrame *THEORAPLAY_getVideo(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    VideoFrame *retval;

    Mutex_Lock(ctx->lock);
    retval = ctx->videolist;
    if (retval)
    {
        ctx->videolist = retval->next;
        retval->next = NULL;
        if (ctx->videolist == NULL)
            ctx->videolisttail = NULL;
        assert(ctx->videocount > 0);
        ctx->videocount--;
    } // if
    Mutex_Unlock(ctx->lock);

    return retval;
} // THEORAPLAY_getVideo


void THEORAPLAY_freeVideo(const THEORAPLAY_VideoFrame *_item)
{
    THEORAPLAY_VideoFrame *item = (THEORAPLAY_VideoFrame *) _item;
    if (item != NULL)
    {
        assert(item->next == NULL);
        free(item->pixels);
        free(item);
    } // if
} // THEORAPLAY_freeVideo

// end of theoraplay.cpp ...

