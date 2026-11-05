/*
 * media.c -- libmediandk stubs for 60 Parsecs! (Unity VideoPlayer / AMediaCodec).
 *
 * libunity imports AMediaCodec/AMediaExtractor as undefined symbols. Leaving
 * them NULL makes the PLT slot 0, and the prologue/camp cutscene jumps to
 * PC=0.  These stubs report a successful decoder that immediately reaches
 * end-of-stream so the player finishes (black frames) instead of crashing.
 *
 * The AMEDIAFORMAT_KEY_* entries are DATA symbols: libmediandk exports each
 * as a `const char *` global variable, and libunity loads it through a
 * GLOB_DAT slot.  A zero slot is what faulted at libunity+0x8d7484 right
 * after AMediaExtractor_getTrackFormat (x25 read from the key slot).  The
 * loader fills the slot with whatever address we register, so we register
 * the address of a pointer whose value is the string -- exactly the shape
 * the real library exports.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "media.h"

/* ---- fake native window / SurfaceTexture ------------------------------- */

typedef struct {
    int refs;
    int w, h;
    void *listener;
    void *st_object;
    int64_t ts;
} sf_window;

static sf_window root_window = { 1, 1280, 720, NULL, NULL, 0 };

int sf_media_is_window(void *window) { return window == &root_window; }
void sf_media_window_acquire(void *window) { (void)window; }
void sf_media_window_release(void *window) { (void)window; }
int sf_media_window_width(void *window) { (void)window; return root_window.w; }
int sf_media_window_height(void *window) { (void)window; return root_window.h; }
int sf_media_window_set_geometry(void *window, int width, int height, int fmt)
{
    (void)window;
    (void)fmt;
    if (width > 0 && height > 0) {
        root_window.w = width;
        root_window.h = height;
    }
    return 0;
}

void *sf_media_surface_texture_create(unsigned texture)
{
    (void)texture;
    return &root_window;
}
void *sf_media_surface_from_texture(void *st)
{
    (void)st;
    return &root_window;
}
void sf_media_surface_set_size(void *s, int w, int h)
{
    sf_media_window_set_geometry(s, w, h, 0);
}
void sf_media_surface_set_java_listener(void *s, void *listener, void *sto)
{
    (void)s;
    root_window.listener = listener;
    root_window.st_object = sto;
}
int64_t sf_media_surface_timestamp(void *s)
{
    (void)s;
    root_window.ts += 33333333LL; /* ~30fps */
    return root_window.ts;
}
int sf_media_surface_texture_update(void *s)
{
    (void)s;
    return 1;
}
void sf_media_update_textures(void) { }

void *sf_media_egl_get_native_client_buffer(void *hb) { (void)hb; return NULL; }
void *sf_media_egl_create_image(unsigned target, void *buf)
{
    (void)target;
    (void)buf;
    return NULL;
}
int sf_media_egl_destroy_image(void *image) { (void)image; return 0; }
int sf_media_egl_image_rgba(void *image, const unsigned char **pixels,
                            int *width, int *height, int *stride)
{
    (void)image;
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    return 0;
}

/* ---- AMediaCodec / AMediaExtractor stubs -------------------------------- */

#define AMEDIACODEC_INFO_TRY_AGAIN_LATER        (-1)
#define AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED  (-2)
#define AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED (-3)
#define AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM   (4)

/* AMediaCodecBufferInfo -- REAL NDK layout (media/NdkMediaCodec.h):
 * offset and size are int32_t, so pts sits at +8 and flags at +16.
 * With size_t here, pts landed at +16 (Unity read it as flags: pts=33333
 * has bit2 set -> frame 2 misread as EOS) and flags at +24 (our EOS flag
 * was never seen).  The oversized memset also wiped 8 bytes past the info
 * -- a pending-frames vector pointer on Unity's stack -- leaving
 * data=NULL with size=1 -> SIGSEGV libunity+0x8d6314 (BENDER's
 * BenderMediaCodecBufferInfo at unity_probe.c:7257 is the reference). */
typedef struct {
    int32_t offset;
    int32_t size;
    int64_t presentation_time_us;
    uint32_t flags;
} m_buffer_info;

typedef struct {
    int started;
    int eos_sent;
    int out_count;
    int64_t pts;
    /* BENDER-style synthetic EOS handshake (fix75/76/184):
     * start() arms format_change_pending; the first output dequeue reports
     * INFO_OUTPUT_FORMAT_CHANGED; a couple of synthetic frames go out with
     * distinct PTS; EOS is only delivered after Unity queues the input EOS
     * (or a bounded poll fallback), and the onFrameAvailable listener is
     * signalled on release.  Early EOS leaves MoviePlayer coroutines stuck
     * on a black screen ("routine is null"). */
    int format_change_pending;
    int synth_sent;
    int synth_released;
    int synth_wait_polls;
    int eos_queued;
    int eos_from_guest;
    int eos_output_sent;
    unsigned output_polls;
    unsigned long generation;
} fake_codec;

typedef struct {
    int track;
} fake_extractor;

/* Source generation (fix79): each setDataSource* bumps it; after a video
 * completed, re-opening the SAME source must not create a new decoder, or
 * Unity loops EOS -> delete -> create forever and the coroutine never ends.
 * One bounded reopen is allowed (fix183 pattern) for legitimate retries. */
static unsigned long g_media_generation;
static unsigned long g_media_completed;
static unsigned long g_media_replay_done;

#define MEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED (-2)
#define SYNTH_FRAME_STEP_US 33333LL

static int synth_frame_target(void)
{
    const char *v = getenv("SF_VIDEO_SYNTH_FRAMES");
    if (v && *v && strcmp(v, "0"))
        return atoi(v);
    return 2; /* BENDER fix184 default */
}

static void media_dispatch_frame_available(const char *reason)
{
    if (root_window.listener) {
        fprintf(stderr, "[sf/media] onFrameAvailable dispatch (%s)\n",
                reason);
        sf_jni_surface_frame_available(root_window.listener,
                                       root_window.st_object);
    }
}

static fake_codec g_codec;
static fake_extractor g_ex;

/* NDK signature: AMediaCodec_createDecoderByType(const char *mime_type) --
 * ONE argument (x0).  Reading mime from x1 dereferenced register garbage
 * (0x10) inside printf's strlen and killed the loader. */
static void *m_codec_create(const char *mime)
{
    fprintf(stderr, "[sf/media] AMediaCodec_createDecoderByType(%s) -> stub\n",
            mime ? mime : "?");
    /* fix79 restart guard: same source after EOS must not loop decoder
     * create/delete.  Allow exactly one bounded reopen, then NULL. */
    if (g_media_completed && g_media_completed == g_media_generation &&
        mime && !strncmp(mime, "video/", 6)) {
        if (!g_media_replay_done) {
            g_media_replay_done = 1;
            fprintf(stderr, "[sf/media] bounded reopen allowed (gen %lu)\n",
                    g_media_generation);
        } else {
            fprintf(stderr,
                    "[sf/media] restart guard: reject same-source reopen "
                    "(gen %lu) -> NULL\n",
                    g_media_generation);
            return NULL;
        }
    }
    memset(&g_codec, 0, sizeof g_codec);
    g_codec.generation = g_media_generation;
    return &g_codec;
}
static int m_codec_delete(void *c) { (void)c; return 0; }
/* ---- call tracing --------------------------------------------------------
 * Unconditional but rate-limited: Unity polls dequeue paths in a loop, so
 * each trace point emits at most TRACE_CAP lines before going quiet.  This
 * is what tells a stuck video coroutine from a finished one on device logs.
 */
#define TRACE_CAP 24
static unsigned m_trace_serial;
static void m_trace(const char *what, long detail)
{
    if (m_trace_serial < TRACE_CAP) {
        m_trace_serial++;
        fprintf(stderr, "[sf/media] #%u %s(%ld)\n", m_trace_serial, what,
                detail);
    }
}

static int m_codec_configure(void *c, void *fmt, void *surface, void *crypto,
                             uint32_t flags)
{
    (void)c;
    (void)fmt;
    (void)crypto;
    (void)flags;
    m_trace("AMediaCodec_configure surface", surface ? 1 : 0);
    return 0;
}
static int m_codec_start(void *c)
{
    if (c) {
        fake_codec *fc = c;
        fc->started = 1;
        fc->format_change_pending = 1;
        fc->synth_sent = fc->synth_released = fc->synth_wait_polls = 0;
        fc->eos_queued = fc->eos_from_guest = fc->eos_output_sent = 0;
        fc->output_polls = 0;
    }
    m_trace("AMediaCodec_start", 0);
    return 0;
}
static int m_codec_stop(void *c)
{
    if (c)
        ((fake_codec *)c)->started = 0;
    m_trace("AMediaCodec_stop", 0);
    return 0;
}
static int m_codec_flush(void *c)
{
    if (c) {
        fake_codec *fc = c;
        fc->format_change_pending = 1;
        fc->synth_sent = fc->synth_released = fc->synth_wait_polls = 0;
        fc->eos_queued = fc->eos_from_guest = fc->eos_output_sent = 0;
        fc->output_polls = 0;
    }
    m_trace("AMediaCodec_flush", 0);
    return 0;
}
static int m_codec_dequeue_in(void *c, int64_t timeoutUs)
{
    (void)c;
    (void)timeoutUs;
    /* Feed one empty input so the pipeline can close with EOS. */
    m_trace("AMediaCodec_dequeueInputBuffer", (long)timeoutUs);
    return 0;
}
static void *m_codec_get_in_buf(void *c, size_t idx, size_t *out_size)
{
    (void)c;
    m_trace("AMediaCodec_getInputBuffer", (long)idx);
    static char dummy[16];
    if (out_size)
        *out_size = sizeof dummy;
    return dummy;
}
static int m_codec_queue_in(void *c, size_t idx, size_t offset, size_t size,
                            uint64_t time, uint32_t flags)
{
    (void)c;
    (void)idx;
    (void)offset;
    (void)time;
    m_trace("AMediaCodec_queueInputBuffer flags", (long)flags);
    if (c && (size == 0 || (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM))) {
        fake_codec *fc = c;
        fc->eos_queued = 1;
        fc->eos_from_guest = 1;
        fprintf(stderr, "[sf/media] EOS handshake: input queued\n");
    }
    return 0;
}
static int m_codec_dequeue_out(void *c, void *info, int64_t timeoutUs)
{
    static unsigned try_again_polls;
    fake_codec *fc = c;
    if (info)
        memset(info, 0, sizeof(m_buffer_info));
    if (!fc || !fc->started)
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;

    /* fix75: the first dequeue after start() reports a format change. */
    if (fc->format_change_pending) {
        fc->format_change_pending = 0;
        fprintf(stderr, "[sf/media] FORMAT_CHANGED reported\n");
        return MEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
    }
    fc->output_polls++;

    int target = synth_frame_target();
    /* Never hand out the next synthetic frame before the previous one was
     * released; after 8 stalled polls force-release and fire the listener
     * (fix76 fallback). */
    if (fc->synth_sent > fc->synth_released) {
        fc->synth_wait_polls++;
        if (fc->synth_wait_polls >= 8) {
            fc->synth_released = fc->synth_sent;
            media_dispatch_frame_available("release-timeout");
        } else {
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        }
    }
    /* fix184: distinct-PTS synthetic frames keep Unity's frame counter
     * moving without fabricating real H.264 output. */
    if (fc->synth_sent < target) {
        int seq = fc->synth_sent;
        int64_t pts = (int64_t)seq * SYNTH_FRAME_STEP_US;
        fc->synth_sent++;
        fc->synth_wait_polls = 0;
        if (info) {
            m_buffer_info bi;
            bi.offset = 0;
            bi.size = 1;
            bi.presentation_time_us = pts;
            bi.flags = 0;
            memcpy(info, &bi, sizeof bi);
        }
        fprintf(stderr, "[sf/media] synth frame %d/%d pts=%lld\n", seq + 1,
                target, (long long)pts);
        return seq;
    }
    /* fix75: EOS only after Unity queued the input EOS -- or a bounded
     * fallback when the guest never does. */
    if (!fc->eos_queued && fc->output_polls >= 24) {
        fc->eos_queued = 1;
        fc->eos_from_guest = 0;
        fprintf(stderr, "[sf/media] EOS fallback (guest never queued)\n");
    }
    if (fc->eos_queued && !fc->eos_output_sent) {
        fc->eos_output_sent = 1;
        if (info) {
            m_buffer_info bi;
            bi.offset = 0;
            bi.size = 0;
            bi.presentation_time_us =
                (int64_t)target * SYNTH_FRAME_STEP_US;
            bi.flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
            memcpy(info, &bi, sizeof bi);
        }
        if (fc->generation) {
            g_media_completed = fc->generation;
            fprintf(stderr,
                    "[sf/media] EOS out (origin=%s) gen=%lu armed\n",
                    fc->eos_from_guest ? "guest-input" : "fallback",
                    fc->generation);
        }
        return 0;
    }
    try_again_polls++;
    if (try_again_polls == 1 || try_again_polls % 600 == 0)
        fprintf(stderr, "[sf/media] dequeueOutput TRY_AGAIN x%u\n",
                try_again_polls);
    return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}
static void *m_codec_get_out_buf(void *c, size_t idx, size_t *out_size)
{
    (void)c;
    m_trace("AMediaCodec_getOutputBuffer", (long)idx);
    /* BENDER fix195: Unity 2022 runs its BufferInfo-PTS frame-rate probe
     * only when getOutputBuffer returns non-NULL.  A dummy pointer sends
     * MoviePlayer down the byte-buffer decode path whose buffer record
     * array is never allocated in synthetic mode -> SIGSEGV at
     * libunity+0x8d6314 (ldr x1, [x8, x25] with x8=NULL).  Match BENDER's
     * synthetic contract: NULL buffer, size 0. */
    if (out_size)
        *out_size = 0;
    return NULL;
}
static void *m_codec_get_out_fmt(void *c)
{
    (void)c;
    m_trace("AMediaCodec_getOutputFormat", 0);
    static int fmt[16];
    return fmt;
}
static int m_codec_release_out(void *c, size_t idx, int render)
{
    (void)idx;
    m_trace("AMediaCodec_releaseOutputBuffer render", (long)render);
    if (c) {
        fake_codec *fc = c;
        if (render) {
            /* fix76: rendering the synthetic frame completes the
             * SurfaceTexture handshake -- fire onFrameAvailable. */
            fc->synth_released = fc->synth_sent;
            media_dispatch_frame_available("release");
        } else if (fc->synth_released < fc->synth_sent) {
            fc->synth_released = fc->synth_sent;
        }
    }
    return 0;
}
static int m_codec_set_surface(void *c, void *surface)
{
    (void)c;
    (void)surface;
    return 0;
}

/* NDK: AMediaExtractor_new(void) takes no arguments. */
static void *m_ext_new(void)
{
    return &g_ex;
}
static int m_ext_delete(void *e) { (void)e; return 0; }
static int m_ext_advance(void *e)
{
    (void)e;
    m_trace("AMediaExtractor_advance", 0);
    return 0;
}
static int m_ext_track_count(void *e)
{
    (void)e;
    m_trace("AMediaExtractor_getTrackCount", 1);
    return 1;
}
static void *m_ext_track_fmt(void *e, size_t idx)
{
    (void)e;
    m_trace("AMediaExtractor_getTrackFormat", (long)idx);
    static int fmt[32];
    return fmt;
}
static int m_ext_sample_track(void *e) { (void)e; return 0; }
static int64_t m_ext_sample_time(void *e) { (void)e; return 0; }
static int m_ext_set_data(void *e, const char *url)
{
    (void)e;
    fprintf(stderr, "[sf/media] AMediaExtractor_setDataSource(%s)\n",
            url ? url : "?");
    g_media_generation++;
    g_media_completed = 0;
    g_media_replay_done = 0;
    return 0;
}
static int m_ext_set_data_fd(void *e, int fd, int64_t off, int64_t len)
{
    (void)e;
    (void)off;
    (void)len;
    fprintf(stderr, "[sf/media] AMediaExtractor_setDataSourceFd(%d)\n", fd);
    g_media_generation++;
    g_media_completed = 0;
    g_media_replay_done = 0;
    return 0;
}
static int m_ext_read_sample(void *e, uint8_t *dst)
{
    (void)e;
    (void)dst;
    m_trace("AMediaExtractor_readSampleData -> EOF", 0);
    return -1; /* no more samples */
}
static int m_ext_seek(void *e, int64_t time, int mode)
{
    (void)e;
    (void)time;
    (void)mode;
    return 0;
}
static int m_ext_select(void *e, size_t idx)
{
    (void)e;
    m_trace("AMediaExtractor_selectTrack", (long)idx);
    return 0;
}

static int m_ext_set_data_custom(void *e, void *ds)
{
    (void)e;
    fprintf(stderr, "[sf/media] AMediaExtractor_setDataSourceCustom(%p)\n",
            ds);
    g_media_generation++;
    g_media_completed = 0;
    g_media_replay_done = 0;
    return 0; /* AMEDIA_OK -- our extractor answers EOF everywhere */
}

/* AMediaDataSource (API 30): Unity can hand the extractor a custom stream.
 * Our extractor stub already answers EOF everywhere, so the callbacks the
 * player registers are never invoked; keep them only for later real-decode
 * work. */
typedef struct {
    void *userdata;
    void *read_at;
    void *get_size;
    void *close;
} fake_datasource;

static fake_datasource g_ds;
static void *m_ds_new(void) { return &g_ds; }
static int m_ds_delete(void *d) { (void)d; return 0; }
static void m_ds_set_userdata(void *d, void *u)
{
    if (d)
        ((fake_datasource *)d)->userdata = u;
}
static void m_ds_set_read_at(void *d, void *fn)
{
    if (d)
        ((fake_datasource *)d)->read_at = fn;
}
static void m_ds_set_get_size(void *d, void *fn)
{
    if (d)
        ((fake_datasource *)d)->get_size = fn;
}
static void m_ds_set_close(void *d, void *fn)
{
    if (d)
        ((fake_datasource *)d)->close = fn;
}

/* AMediaFormat -- enough getters so Unity can parse our dummy track. */
static void m_fmt_delete(void *f) { (void)f; }
static int m_fmt_get_int32(void *f, const char *name, int32_t *out)
{
    (void)f;
    if (out)
        *out = 0;
    if (!name)
        return 0;
    if (strstr(name, "color-format"))
        *out = 19; /* COLOR_FormatYUV420Planar */
    else if (strstr(name, "width") || strstr(name, "stride"))
        *out = 1280;
    else if (strstr(name, "height") || strstr(name, "slice-height"))
        *out = 720;
    else if (strstr(name, "frame-rate"))
        *out = 30;
    else if (strstr(name, "channel-count"))
        *out = 2;
    else if (strstr(name, "sample-rate"))
        *out = 48000;
    else if (strstr(name, "duration"))
        *out = 1000000;
    return 1;
}
static int m_fmt_get_int64(void *f, const char *name, int64_t *out)
{
    (void)f;
    (void)name;
    if (out)
        *out = 1000000;
    return 1;
}
static int m_fmt_get_float(void *f, const char *name, float *out)
{
    (void)f;
    (void)name;
    if (out)
        *out = 30.0f;
    return 1;
}
static int m_fmt_get_string(void *f, const char *name, const char **out)
{
    (void)f;
    if (out)
        *out = (name && strstr(name, "mime")) ? "video/avc" : "";
    return 1;
}
static void m_fmt_set_int32(void *f, const char *name, int32_t v)
{
    (void)f;
    (void)name;
    (void)v;
}

/* ANativeWindow */
static void m_win_acquire(void *w) { (void)w; }
static void m_win_release(void *w) { (void)w; }
static void *m_win_from_surface(void *jni, void *surface)
{
    (void)jni;
    (void)surface;
    return &root_window;
}
static int m_win_width(void *w) { (void)w; return root_window.w; }
static int m_win_height(void *w) { (void)w; return root_window.h; }
static int m_win_set_geom(void *w, int32_t w2, int32_t h2, int32_t fmt)
{
    (void)w;
    (void)fmt;
    if (w2 > 0 && h2 > 0) {
        root_window.w = w2;
        root_window.h = h2;
    }
    return 0;
}
static void *m_win_to_surface(void *w)
{
    /* Our window token doubles as the surface token everywhere else. */
    return w ? w : &root_window;
}

/* ---- AMEDIAFORMAT_KEY_* data symbols ------------------------------------ */
/* libmediandk exports each key as a `const char *` global whose value is the
 * canonical string (frameworks/av NdkMediaFormat.cpp).  Values must match
 * exactly: Unity strcmp()s them. */
#define SF_MEDIA_KEY(id, val)                                               \
    static const char sf_key_##id[] = val;                                  \
    static const char *const sf_keyp_##id = sf_key_##id

SF_MEDIA_KEY(aac_profile, "aac-profile");
SF_MEDIA_KEY(bit_rate, "bitrate");
SF_MEDIA_KEY(caption_service_number, "caption-service-number");
SF_MEDIA_KEY(channel_count, "channel-count");
SF_MEDIA_KEY(channel_mask, "channel-mask");
SF_MEDIA_KEY(color_format, "color-format");
SF_MEDIA_KEY(color_range, "color-range");
SF_MEDIA_KEY(color_standard, "color-standard");
SF_MEDIA_KEY(duration, "durationUs");
SF_MEDIA_KEY(encoder_delay, "encoder-delay");
SF_MEDIA_KEY(frame_rate, "frame-rate");
SF_MEDIA_KEY(height, "height");
SF_MEDIA_KEY(is_autoselect, "is-autoselect");
SF_MEDIA_KEY(is_default, "is-default");
SF_MEDIA_KEY(is_forced_subtitle, "is-forced");
SF_MEDIA_KEY(language, "language");
SF_MEDIA_KEY(max_input_size, "max-input-size");
SF_MEDIA_KEY(mime, "mime");
SF_MEDIA_KEY(operating_rate, "operating-rate");
SF_MEDIA_KEY(pcm_encoding, "pcm-encoding");
SF_MEDIA_KEY(profile, "profile");
SF_MEDIA_KEY(push_blank_buffers_on_stop, "push-blank-buffers-on-shutdown");
SF_MEDIA_KEY(repeat_previous_frame_after, "repeat-previous-frame-after");
SF_MEDIA_KEY(rotation, "rotation-degrees");
SF_MEDIA_KEY(sample_rate, "sample-rate");
SF_MEDIA_KEY(slice_height, "slice-height");
SF_MEDIA_KEY(stride, "stride");
SF_MEDIA_KEY(target_time, "target-time");
SF_MEDIA_KEY(temporal_layering, "temporal-layering");
SF_MEDIA_KEY(track_index, "track-index");
SF_MEDIA_KEY(width, "width");

static const nx_import SF_MEDIA_TABLE[] = {
    { "AMediaCodec_createDecoderByType", (void *)m_codec_create },
    { "AMediaCodec_delete", (void *)m_codec_delete },
    { "AMediaCodec_configure", (void *)m_codec_configure },
    { "AMediaCodec_start", (void *)m_codec_start },
    { "AMediaCodec_stop", (void *)m_codec_stop },
    { "AMediaCodec_flush", (void *)m_codec_flush },
    { "AMediaCodec_dequeueInputBuffer", (void *)m_codec_dequeue_in },
    { "AMediaCodec_getInputBuffer", (void *)m_codec_get_in_buf },
    { "AMediaCodec_queueInputBuffer", (void *)m_codec_queue_in },
    { "AMediaCodec_dequeueOutputBuffer", (void *)m_codec_dequeue_out },
    { "AMediaCodec_getOutputBuffer", (void *)m_codec_get_out_buf },
    { "AMediaCodec_getOutputFormat", (void *)m_codec_get_out_fmt },
    { "AMediaCodec_releaseOutputBuffer", (void *)m_codec_release_out },
    { "AMediaCodec_setOutputSurface", (void *)m_codec_set_surface },
    { "AMediaExtractor_new", (void *)m_ext_new },
    { "AMediaExtractor_delete", (void *)m_ext_delete },
    { "AMediaExtractor_advance", (void *)m_ext_advance },
    { "AMediaExtractor_getTrackCount", (void *)m_ext_track_count },
    { "AMediaExtractor_getTrackFormat", (void *)m_ext_track_fmt },
    { "AMediaExtractor_getSampleTrackIndex", (void *)m_ext_sample_track },
    { "AMediaExtractor_getSampleTime", (void *)m_ext_sample_time },
    { "AMediaExtractor_setDataSource", (void *)m_ext_set_data },
    { "AMediaExtractor_setDataSourceCustom", (void *)m_ext_set_data_custom },
    { "AMediaExtractor_setDataSourceFd", (void *)m_ext_set_data_fd },
    { "AMediaExtractor_readSampleData", (void *)m_ext_read_sample },
    { "AMediaExtractor_seekTo", (void *)m_ext_seek },
    { "AMediaExtractor_selectTrack", (void *)m_ext_select },
    { "AMediaDataSource_new", (void *)m_ds_new },
    { "AMediaDataSource_delete", (void *)m_ds_delete },
    { "AMediaDataSource_setUserdata", (void *)m_ds_set_userdata },
    { "AMediaDataSource_setReadAt", (void *)m_ds_set_read_at },
    { "AMediaDataSource_setGetSize", (void *)m_ds_set_get_size },
    { "AMediaDataSource_setClose", (void *)m_ds_set_close },
    { "AMediaFormat_delete", (void *)m_fmt_delete },
    { "AMediaFormat_getInt32", (void *)m_fmt_get_int32 },
    { "AMediaFormat_getInt64", (void *)m_fmt_get_int64 },
    { "AMediaFormat_getFloat", (void *)m_fmt_get_float },
    { "AMediaFormat_getString", (void *)m_fmt_get_string },
    { "AMediaFormat_setInt32", (void *)m_fmt_set_int32 },
    { "ANativeWindow_acquire", (void *)m_win_acquire },
    { "ANativeWindow_release", (void *)m_win_release },
    { "ANativeWindow_fromSurface", (void *)m_win_from_surface },
    { "ANativeWindow_toSurface", (void *)m_win_to_surface },
    { "ANativeWindow_getWidth", (void *)m_win_width },
    { "ANativeWindow_getHeight", (void *)m_win_height },
    { "ANativeWindow_setBuffersGeometry", (void *)m_win_set_geom },
    /* Data symbols: register the address of the pointer, matching the real
     * libmediandk export shape. */
    { "AMEDIAFORMAT_KEY_AAC_PROFILE", (void *)&sf_keyp_aac_profile },
    { "AMEDIAFORMAT_KEY_BIT_RATE", (void *)&sf_keyp_bit_rate },
    { "AMEDIAFORMAT_KEY_CAPTION_SERVICE_NUMBER",
      (void *)&sf_keyp_caption_service_number },
    { "AMEDIAFORMAT_KEY_CHANNEL_COUNT", (void *)&sf_keyp_channel_count },
    { "AMEDIAFORMAT_KEY_CHANNEL_MASK", (void *)&sf_keyp_channel_mask },
    { "AMEDIAFORMAT_KEY_COLOR_FORMAT", (void *)&sf_keyp_color_format },
    { "AMEDIAFORMAT_KEY_COLOR_RANGE", (void *)&sf_keyp_color_range },
    { "AMEDIAFORMAT_KEY_COLOR_STANDARD", (void *)&sf_keyp_color_standard },
    { "AMEDIAFORMAT_KEY_DURATION", (void *)&sf_keyp_duration },
    { "AMEDIAFORMAT_KEY_ENCODER_DELAY", (void *)&sf_keyp_encoder_delay },
    { "AMEDIAFORMAT_KEY_FRAME_RATE", (void *)&sf_keyp_frame_rate },
    { "AMEDIAFORMAT_KEY_HEIGHT", (void *)&sf_keyp_height },
    { "AMEDIAFORMAT_KEY_IS_AUTOSELECT", (void *)&sf_keyp_is_autoselect },
    { "AMEDIAFORMAT_KEY_IS_DEFAULT", (void *)&sf_keyp_is_default },
    { "AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE",
      (void *)&sf_keyp_is_forced_subtitle },
    { "AMEDIAFORMAT_KEY_LANGUAGE", (void *)&sf_keyp_language },
    { "AMEDIAFORMAT_KEY_MAX_INPUT_SIZE", (void *)&sf_keyp_max_input_size },
    { "AMEDIAFORMAT_KEY_MIME", (void *)&sf_keyp_mime },
    { "AMEDIAFORMAT_KEY_OPERATING_RATE", (void *)&sf_keyp_operating_rate },
    { "AMEDIAFORMAT_KEY_PCM_ENCODING", (void *)&sf_keyp_pcm_encoding },
    { "AMEDIAFORMAT_KEY_PROFILE", (void *)&sf_keyp_profile },
    { "AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP",
      (void *)&sf_keyp_push_blank_buffers_on_stop },
    { "AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER",
      (void *)&sf_keyp_repeat_previous_frame_after },
    { "AMEDIAFORMAT_KEY_ROTATION", (void *)&sf_keyp_rotation },
    { "AMEDIAFORMAT_KEY_SAMPLE_RATE", (void *)&sf_keyp_sample_rate },
    { "AMEDIAFORMAT_KEY_SLICE_HEIGHT", (void *)&sf_keyp_slice_height },
    { "AMEDIAFORMAT_KEY_STRIDE", (void *)&sf_keyp_stride },
    { "AMEDIAFORMAT_KEY_TARGET_TIME", (void *)&sf_keyp_target_time },
    { "AMEDIAFORMAT_KEY_TEMPORAL_LAYERING",
      (void *)&sf_keyp_temporal_layering },
    { "AMEDIAFORMAT_KEY_TRACK_INDEX", (void *)&sf_keyp_track_index },
    { "AMEDIAFORMAT_KEY_WIDTH", (void *)&sf_keyp_width },
};

const nx_import *sf_media_table(size_t *count)
{
    if (count)
        *count = sizeof SF_MEDIA_TABLE / sizeof SF_MEDIA_TABLE[0];
    return SF_MEDIA_TABLE;
}

void *sf_media_sym(const char *name)
{
    for (size_t i = 0; i < sizeof SF_MEDIA_TABLE / sizeof SF_MEDIA_TABLE[0]; i++)
        if (name && strcmp(name, SF_MEDIA_TABLE[i].name) == 0)
            return SF_MEDIA_TABLE[i].addr;
    return NULL;
}
