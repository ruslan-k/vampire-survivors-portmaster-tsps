/*
 * audio.c -- saida de audio do FMOD Studio nativo em NextOS.
 *
 * Blasphemous nao usa o FMOD embutido da Unity: ele carrega o libfmod.so /
 * libfmodstudio.so proprios e escolhe o output AudioTrack.  Do lado nativo
 * esse output e' inteiramente JNI: o FMOD instancia org/fmod/AudioDevice,
 * chama init(canais, taxa, frames, buffers), e a thread do mixer entrega cada
 * bloco por write(byte[], bytes).  Aqui a mesma sequencia termina numa fila do
 * SDL, sem forcar driver (regra #6): quem escolhe o backend e' o sistema.
 *
 * O write do AudioTrack e' BLOQUEANTE no Android -- e' ele que dita o ritmo da
 * thread de mixagem.  Uma fila que so' cresce faria o FMOD mixar tao rapido
 * quanto a CPU permite e o audio chegaria com segundos de atraso, entao o
 * write daqui tambem segura o chamador enquanto a fila estiver cheia.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>

#include "nx_elf.h"
#include "sf.h"

static SDL_AudioDeviceID output;
static int out_channels;
static int out_rate;
static Uint32 queue_target;
static int audio_subsystem;
static unsigned long blocks_written;
static int trace;

static void device_destroy(void);

static int silent_driver(const char *name)
{
    return !name || strcmp(name, "dummy") == 0 || strcmp(name, "disk") == 0;
}

static int pcm_peak_s16(const int16_t *samples, int count)
{
    int peak = 0;
    for (int i = 0; i < count; i++) {
        int value = samples[i];
        if (value < 0)
            value = value == -32768 ? 32768 : -value;
        if (value > peak)
            peak = value;
    }
    return peak;
}

static int ensure_subsystem(void)
{
    if (audio_subsystem)
        return 1;
    const char *requested = getenv("SF_AUDIO_DRIVER");
    if (requested && *requested)
        setenv("SDL_AUDIODRIVER", requested, 1);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    audio_subsystem = 1;
    trace = getenv("SF_AUDIO_TRACE") != NULL;

    fprintf(stderr, "[audio] SDL drivers:");
    for (int i = 0, n = SDL_GetNumAudioDrivers(); i < n; i++) {
        const char *name = SDL_GetAudioDriver(i);
        if (name)
            fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n[audio] backend selected: %s\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver()
                                        : "(none)");
    return 1;
}

/* org/fmod/AudioDevice.init(int channels, int rate, int frames, int buffers) */
int sf_audio_device_init(int channels, int rate, int frames, int buffers)
{
    if (getenv("SF_NO_AUDIO"))
        return 0;
    if (!ensure_subsystem())
        return 0;
    if (silent_driver(SDL_GetCurrentAudioDriver()))
        return 0;
    if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000) {
        fprintf(stderr, "[audio] formato recusado: %d canais, %d Hz\n",
                channels, rate);
        return 0;
    }
    if (frames < 128)
        frames = 128;
    if (frames > 4096)
        frames = 4096;
    if (buffers < 2)
        buffers = 2;
    if (buffers > 16)
        buffers = 16;

    /* Two independent FMOD instances share this one SDL device (the Unity-side
     * FMODAudioDevice session and the game's own Studio system through the
     * AudioTrack JNI / AAudio facade).  Measured on the box, closing the device
     * at every session boundary made the surviving writer queue into a dead
     * handle -- 5138 "SDL_QueueAudio falhou: Invalid audio device ID" in one
     * manual run, heard as choppy audio.  The device now lives for the whole
     * process: reuse it while the format matches, reopen only on a real change. */
    if (output && out_channels == channels && out_rate == rate) {
        blocks_written = 0;
        queue_target = (Uint32)frames * (Uint32)channels *
                       (Uint32)sizeof(int16_t) * (Uint32)buffers;
        SDL_PauseAudioDevice(output, 0);
        fprintf(stderr,
                "[audio] AudioDevice.init: reutilizando o dispositivo SDL "
                "(%d Hz x %d, frames=%d, buffers=%d)\n",
                rate, channels, frames, buffers);
        return 1;
    }

    device_destroy();

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    memset(&have, 0, sizeof have);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = (Uint16)frames;

    output = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!output) {
        fprintf(stderr, "[audio] SDL_OpenAudioDevice falhou: %s\n",
                SDL_GetError());
        return 0;
    }
    out_channels = channels;
    out_rate = rate;
    blocks_written = 0;
    queue_target = (Uint32)frames * (Uint32)channels *
                   (Uint32)sizeof(int16_t) * (Uint32)buffers;
    SDL_PauseAudioDevice(output, 0);
    fprintf(stderr,
            "[audio] AudioDevice.init: %d Hz, %d canal(is), frames=%d, "
            "buffers=%d (SDL %d Hz/%d, driver=%s)\n",
            rate, channels, frames, buffers, have.freq, have.channels,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return 1;
}

/* org/fmod/AudioDevice.write(byte[] data, int length) */
void sf_audio_device_write(const void *data, int bytes)
{
    if (!output || !data || bytes <= 0)
        return;
    /* Contrapressao com TETO: se a fila nao drenar (device sumindo, backend
     * travado) a thread de mixagem do FMOD nao pode ficar presa para sempre. */
    for (int spin = 0; spin < 500 &&
                       SDL_GetQueuedAudioSize(output) >= queue_target; spin++)
        usleep(1000);
    if (SDL_QueueAudio(output, data, (Uint32)bytes) != 0) {
        fprintf(stderr, "[audio] SDL_QueueAudio falhou: %s\n", SDL_GetError());
        return;
    }
    blocks_written++;
    if (trace && (blocks_written <= 8 || blocks_written % 512 == 0))
        fprintf(stderr, "[audio] bloco %lu (%d bytes) peak=%d queued=%u\n",
                blocks_written, bytes,
                pcm_peak_s16(data, bytes / (int)sizeof(int16_t)),
                (unsigned)SDL_GetQueuedAudioSize(output));
}

/* Real teardown: only for a format change or process exit -- never for a
 * mere session boundary (see sf_audio_device_close). */
static void device_destroy(void)
{
    if (!output)
        return;
    SDL_PauseAudioDevice(output, 1);
    SDL_ClearQueuedAudio(output);
    SDL_CloseAudioDevice(output);
    output = 0;
    blocks_written = 0;
}

/* org/fmod/AudioDevice.close() */
void sf_audio_device_close(void)
{
    if (!output)
        return;
    /* Session boundary only: do NOT destroy the SDL device.  The other FMOD
     * instance keeps writing through it (see the note in sf_audio_device_init
     * -- destroying it here was the choppy-audio root cause).  Draining the
     * queue would flush the survivor's pending audio, so leave it alone too. */
    blocks_written = 0;
    fprintf(stderr, "[audio] AudioDevice.close (sessao encerrada)\n");
}

int sf_audio_started(void)
{
    return output != 0;
}

/* ------------------------------------------------------------------ FMOD */
/*
 * O FMOD EMBUTIDO na Unity (fmod_output_audiotrack.cpp) nao abre saida nenhuma
 * sozinho: quem faz isso, no Android, e' a thread JAVA de
 * org/fmod/FMODAudioDevice.run().  Ela e' que descobre o formato por
 * fmodGetInfo(), cria o AudioTrack, e depois fica pedindo bloco por bloco com
 * fmodProcess(ByteBuffer) e escrevendo no AudioTrack.  O nativo so' expoe
 * fmodGetInfo/fmodProcess e espera ser chamado.
 *
 * Sem essa thread o jogo roda MUDO e nao ha um unico erro no log -- foi
 * exatamente o que aconteceu aqui: `FMODAudioDevice.start` chegava, marcava
 * "rodando", e ninguem mais pedia um bloco. Nenhum descritor de /dev/snd era
 * aberto pelo processo.
 *
 * A sequencia abaixo e' a do proprio jogo, lida do `run()` de
 * org/fmod/FMODAudioDevice no classes.dex desta build -- nao e' invencao nem
 * heranca de outro port.  Os indices do fmodGetInfo sao os que o bytecode usa:
 *
 *     0 = taxa de amostragem      1 = quadros por bloco
 *     2 = numero de blocos        3 = "ha' mixagem para entregar"
 *     4 = canais
 *
 * O AudioTrack vira a fila do SDL (sf_audio_device_*), que ja faz a
 * contrapressao com teto que o write() bloqueante do Android fazia.
 */
enum {
    FMOD_INFO_RATE = 0,
    FMOD_INFO_BLOCK_FRAMES = 1,
    FMOD_INFO_BLOCK_COUNT = 2,
    FMOD_INFO_RUNNING = 3,
    FMOD_INFO_CHANNELS = 4,
};

typedef int (*fmod_get_info_fn)(void *env, void *thiz, int32_t info);
typedef int (*fmod_process_fn)(void *env, void *thiz, void *byte_buffer);

static pthread_t fmod_thread;
static int fmod_thread_live;
static int fmod_shutdown;

static void *fmod_pump(void *unused)
{
    (void)unused;
    /* A thread vive o PROCESSO INTEIRO, nao a chamada de start().
     *
     * Medido nesta build: `FMODAudioDevice.start()` chega no frame zero, quando
     * so' existem 45 nativos registrados e NENHUM deles e' do FMOD -- e logo em
     * seguida vem um `stop()`.  Uma thread que nascesse em start() e morresse
     * quando `should_run` cai encontraria a mesa vazia, desistiria, e o jogo
     * ficaria mudo para sempre sem uma unica linha de erro.  Quem manda aqui e'
     * o par (natives registrados + should_run), consultado sempre, nunca uma
     * unica vez na entrada. */
    fmod_get_info_fn get_info = NULL;
    fmod_process_fn process = NULL;
    void *env = NULL, *thiz = NULL, *buffer = NULL;
    unsigned char *pcm = NULL;
    int opened = 0, attempts = 3, block_bytes = 0, announced = 0;

    while (!__atomic_load_n(&fmod_shutdown, __ATOMIC_ACQUIRE)) {
        if (!sf_jni_fmod_should_run()) {
            if (opened) {
                sf_audio_device_close();
                opened = 0;
            }
            usleep(50000);
            continue;
        }
        if (!get_info || !process) {
            get_info = (fmod_get_info_fn)sf_jni_native(
                "org/fmod/FMODAudioDevice", "fmodGetInfo");
            process = (fmod_process_fn)sf_jni_native(
                "org/fmod/FMODAudioDevice", "fmodProcess");
            if (!get_info || !process) {
                usleep(50000);
                continue;
            }
            env = sf_jni_env();
            thiz = sf_jni_fmod_device();
            buffer = sf_jni_fmod_bytebuffer();
            pcm = sf_jni_fmod_pcm();
            fprintf(stderr, "[audio] fmodGetInfo/fmodProcess resolvidos\n");
        }

        if (!opened) {
            if (attempts <= 0) {
                usleep(200000);
                attempts = 3;
                continue;
            }
            int rate = get_info(env, thiz, FMOD_INFO_RATE);
            int channels = get_info(env, thiz, FMOD_INFO_CHANNELS);
            int frames = get_info(env, thiz, FMOD_INFO_BLOCK_FRAMES);
            int blocks = get_info(env, thiz, FMOD_INFO_BLOCK_COUNT);
            if (rate <= 0 || channels <= 0 || frames <= 0) {
                usleep(50000);
                continue;
            }
            block_bytes = frames * channels * (int)sizeof(int16_t);
            if (block_bytes > sf_jni_fmod_pcm_capacity()) {
                fprintf(stderr,
                        "[audio] bloco do FMOD de %d B nao cabe no buffer "
                        "direto de %d B\n",
                        block_bytes, sf_jni_fmod_pcm_capacity());
                usleep(500000);
                continue;
            }
            sf_jni_fmod_set_buffer_size(block_bytes);
            if (!sf_audio_device_init(channels, rate, frames, blocks)) {
                attempts--;
                usleep(200000);
                continue;
            }
            opened = 1;
            attempts = 3;
            if (!announced) {
                announced = 1;
                fprintf(stderr,
                        "[audio] FMOD da Unity entregando %d Hz x %d canal(is), "
                        "bloco de %d quadros\n", rate, channels, frames);
            }
        }

        /* fmodGetInfo(3) e' o mesmo portao do bytecode: quando o mixer nao tem
         * nada a entregar, o Java derruba o AudioTrack em vez de escrever
         * silencio.  Aqui a saida SDL apenas fecha e reabre no proximo bloco. */
        if (get_info(env, thiz, FMOD_INFO_RUNNING) != 1) {
            sf_audio_device_close();
            opened = 0;
            usleep(20000);
            continue;
        }
        process(env, thiz, buffer);
        sf_audio_device_write(pcm, block_bytes);
    }
    sf_audio_device_close();
    return NULL;
}

int sf_audio_start(void *env)
{
    (void)env;
    if (fmod_thread_live)
        return 1;
    if (getenv("SF_NO_AUDIO"))
        return 0;
    int rc = pthread_create(&fmod_thread, NULL, fmod_pump, NULL);
    if (rc != 0) {
        fprintf(stderr, "[audio] pthread_create(FMOD) falhou: %s\n",
                strerror(rc));
        return 0;
    }
    fmod_thread_live = 1;
    fprintf(stderr, "[audio] thread do FMODAudioDevice de pe'\n");
    return 1;
}

void sf_audio_stop(void)
{
    if (fmod_thread_live) {
        __atomic_store_n(&fmod_shutdown, 1, __ATOMIC_RELEASE);
        pthread_join(fmod_thread, NULL);
        fmod_thread_live = 0;
    }
    device_destroy();
    if (audio_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem = 0;
    }
}

/* ------------------------------------------------------------------- AAudio
 *
 * FMOD Studio chooses its Android output from android.os.Build.VERSION.SDK_INT;
 * this port answers API 33, so FMOD dlopens libaaudio.so and binds all 25 entry
 * points it references.  Getting them to resolve is not enough: measured on the
 * box, a facade whose AAudio_createStreamBuilder returned the builder instead of
 * writing it through the out-parameter made FMOD read a heap address as an
 * aaudio_result_t and report
 *   FMOD failed to initialize the output device.: "Error initializing output
 *   device. " (60)
 *   SystemNotInitializedException: [FMOD] Initialization failed :
 *   FMOD.System.setOutput : ERR_PLUGIN_MISSING
 * thrown out of FMODUnity.RuntimeManager.GetBus -> RG.Parsecs.Menu.PauseMenu
 * .Awake(), which is why the title scene stayed black after the first splash.
 * Signatures and constants below are the NDK's, because FMOD compares against
 * them by value.
 *
 * So AAudio is a contract to satisfy, not a nicety: the box does have working
 * playback, and sf_audio_device_* above already owns the SDL device the
 * AudioTrack path writes to.  Each AAudioStream is therefore a view of that one
 * device, in both of the modes the NDK documents -- a data callback we drive on
 * our own thread, or blocking AAudioStream_write() calls from FMOD's mixer.
 */

/* Values are the ones in the NDK's aaudio.h, not a local invention: FMOD
 * compares what getState()/the result codes return against these numbers. */
enum {
    AA_OK = 0,
    AA_ERROR_BASE = -900,
    AA_ERROR_DISCONNECTED = AA_ERROR_BASE + 1,
    AA_ERROR_ILLEGAL_ARGUMENT = AA_ERROR_BASE + 2,
    AA_ERROR_INTERNAL = AA_ERROR_BASE + 3,
    AA_ERROR_INVALID_STATE = AA_ERROR_BASE + 5,
    AA_ERROR_UNAVAILABLE = AA_ERROR_BASE + 8,
    AA_ERROR_NO_FREE_HANDLES = AA_ERROR_BASE + 9,
    AA_ERROR_NO_MEMORY = AA_ERROR_BASE + 11,
    AA_ERROR_WOULD_BLOCK = AA_ERROR_BASE + 12,
    AA_ERROR_INVALID_HANDLE = AA_ERROR_BASE + 13,
    AA_ERROR_INVALID_RATE = AA_ERROR_BASE + 19,
    AA_ERROR_TIMEOUT = AA_ERROR_BASE + 17,
    AA_FORMAT_UNSPECIFIED = 0,
    AA_FORMAT_PCM_I16 = 1,
    AA_FORMAT_PCM_FLOAT = 2,
    AA_SHARING_MODE_EXCLUSIVE = 0,
    AA_SHARING_MODE_SHARED = 1,
    AA_PERFORMANCE_NONE = 10,
    AA_PERFORMANCE_POWER_SAVING = 11,
    AA_PERFORMANCE_LOW_LATENCY = 12,
    AA_STATE_UNINITIALIZED = 0,
    AA_STATE_UNKNOWN = 1,
    AA_STATE_OPEN = 2,
    AA_STATE_STARTING = 10,
    AA_STATE_RUNNING = 11,          /* AAUDIO_STATE_STARTED */
    AA_STATE_PAUSING = 12,
    AA_STATE_PAUSED = 13,
    AA_STATE_FLUSHING = 14,
    AA_STATE_FLUSHED = 15,
    AA_STATE_STOPPING = 16,
    AA_STATE_STOPPED = 17,
    AA_STATE_CLOSING = 18,
    AA_STATE_CLOSED = 19,
    AA_STATE_DISCONNECTED = 20,
    AA_CALLBACK_RESULT_CONTINUE = 0,
    AA_CALLBACK_RESULT_STOP = 1,
    AA_USAGE_MEDIA = 1,
    AA_CONTENT_TYPE_MUSIC = 2,
    AA_INPUT_PRESET_GENERIC = 1,
    AA_DIRECTION_OUTPUT = 1
};

typedef struct {
    int32_t sample_rate;
    int32_t channel_count;
    int32_t format;
    int32_t performance_mode;
    int32_t sharing_mode;
    int32_t usage;
    int32_t content_type;
    int32_t input_preset;
    int32_t direction;
    int32_t device_id;
    int32_t frames_per_data_callback;
    int32_t buffer_capacity;
    int32_t (*data_callback)(void *stream, void *user, void *audio,
                             int32_t frames);
    void *data_user;
    void (*error_callback)(void *stream, void *user, int32_t error);
    void *error_user;
} aa_builder;

typedef struct {
    int32_t magic;
    aa_builder req;
    int32_t state;
    int32_t burst;
    int32_t buffer_size;
    int64_t frames_written;
    int64_t frames_read;
    int64_t started_at_ns;
    void *callback_buffer;
    pthread_t pump;
    int pump_live;
    volatile int pump_stop;
} aa_stream;

#define AA_STREAM_MAGIC 0x50464141u   /* "AAPF" */
#define AA_MAX_STREAMS 4
static aa_stream aa_streams[AA_MAX_STREAMS];

static int aa_bytes_per_frame(int format)
{
    return format == AA_FORMAT_PCM_FLOAT ? 4 : 2;
}

/* FMOD binds all 25 AAudio entry points and then reports "Error initializing
 * output device (60)" without ever reaching openStream, so the stopping point
 * is invisible from the dlsym trace alone.  SF_AATRACE prints every facade call
 * with its arguments; that is what turns "somewhere in here" into a line. */
static int aa_trace_on(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("SF_AATRACE") != NULL;
    return enabled;
}

#define AAT(...)                                                               \
    do {                                                                       \
        if (aa_trace_on()) {                                                   \
            fprintf(stderr, "[audio/aa] " __VA_ARGS__);                        \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

static int64_t aa_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* "No sound" has two very different causes that look identical from outside: the
 * mixer is not handing us any block at all, or it is handing us blocks full of
 * silence (the game has nothing to play yet).  This counts blocks and the peak
 * s16 amplitude so a device run answers which one it is, every ~5 s and only
 * under SF_AATRACE. */
static void aa_flow_probe(const void *s16_data, size_t bytes)
{
    static unsigned long blocks;
    static int peak;
    if (!aa_trace_on() || !s16_data || bytes < 2)
        return;
    const int16_t *p = s16_data;
    size_t n = bytes / sizeof(int16_t);
    for (size_t i = 0; i < n; i++) {
        int v = p[i] < 0 ? -p[i] : p[i];
        if (v > peak)
            peak = v;
    }
    if (++blocks % 256 != 0)
        return;
    AAT("fluxo: %lu blocos, pico s16=%d (0 = silencio do mixer)", blocks, peak);
    peak = 0;
}

/* Keep the AAudio pump paced like a real block when its silence was dropped. */
static void aa_pace_drop(aa_stream *s, size_t frames)
{
    int32_t rate = s->req.sample_rate;
    if (rate <= 0 || frames == 0)
        return;
    uint64_t us = (uint64_t)frames * 1000000ull / (uint64_t)rate;
    if (us > 0 && us <= 200000)
        usleep((useconds_t)us);
}

/* Hand one block of the stream's format to the shared SDL device, converting
 * float to s16 because sf_audio_device_write() paces the mixer through a device
 * opened as AUDIO_S16SYS.  The length handed on must be the converted size, half
 * the float byte count: measured on the box, passing the float byte count played
 * every block with its second half uninitialized memory, which is what the
 * maintainer heard as constant static.
 *
 * All-silence blocks are NOT queued.  The game's Studio system mixes through
 * the AudioTrack session while the other FMOD instance keeps its AAudio view
 * running on pure silence forever; queueing those blocks into the shared FIFO
 * displaced the audible ones (two writers at ~2x realtime into one drain) and
 * the maintainer heard the audio cutting in and out.  Silence contributes
 * nothing audible, so drop it -- but pace the pump exactly like a queued block,
 * otherwise the callback loop would spin at full speed. */
static void aa_submit(aa_stream *s, const void *data, size_t bytes)
{
    if (!data || bytes == 0)
        return;
    if (s->req.format == AA_FORMAT_PCM_FLOAT) {
        size_t samples = bytes / sizeof(float);
        static int16_t *converted;
        static size_t converted_capacity;
        if (samples > converted_capacity) {
            free(converted);
            converted_capacity = samples;
            converted = malloc(converted_capacity * sizeof *converted);
            if (!converted) {
                converted_capacity = 0;
                return;
            }
        }
        const float *src = data;
        int peak = 0;
        for (size_t i = 0; i < samples; i++) {
            float value = src[i] * 32768.0f;
            if (value > 32767.0f) value = 32767.0f;
            if (value < -32768.0f) value = -32768.0f;
            int16_t out = (int16_t)value;
            converted[i] = out;
            int v = out < 0 ? -out : out;
            if (v > peak)
                peak = v;
        }
        if (peak == 0) {
            aa_pace_drop(s, samples / (size_t)(s->req.channel_count > 0
                                               ? s->req.channel_count : 1));
            return;
        }
        aa_flow_probe(converted, samples * sizeof(int16_t));
        sf_audio_device_write(converted, (int)(samples * sizeof(int16_t)));
        return;
    }
    /* I16 passthrough: cheap silence scan before queueing. */
    const int16_t *p = data;
    size_t n = bytes / sizeof(int16_t);
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int v = p[i] < 0 ? -p[i] : p[i];
        if (v > peak)
            peak = v;
    }
    if (peak == 0) {
        aa_pace_drop(s, n / (size_t)(s->req.channel_count > 0
                                     ? s->req.channel_count : 1));
        return;
    }
    aa_flow_probe(data, bytes);
    sf_audio_device_write(data, (int)bytes);
}

static void *aa_pump(void *opaque)
{
    aa_stream *s = opaque;
    int32_t frames = s->req.frames_per_data_callback > 0
                       ? s->req.frames_per_data_callback : s->burst;
    size_t bytes = (size_t)frames
                   * (size_t)s->req.channel_count
                   * (size_t)aa_bytes_per_frame(s->req.format);
    void *buffer = calloc(1, bytes ? bytes : 1024);
    if (!buffer)
        return NULL;
    while (!s->pump_stop && s->state == AA_STATE_RUNNING) {
        if (!s->req.data_callback)
            break;
        int32_t result = s->req.data_callback(s, s->req.data_user, buffer,
                                             frames);
        if (result == AA_CALLBACK_RESULT_STOP)
            break;
        aa_submit(s, buffer, bytes);
        s->frames_written += frames;
        s->frames_read += frames;
    }
    free(buffer);
    return NULL;
}

static aa_builder *aa_builder_new(void)
{
    aa_builder *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->sample_rate = 48000;
    b->channel_count = 2;
    b->format = AA_FORMAT_PCM_I16;
    b->performance_mode = AA_PERFORMANCE_LOW_LATENCY;
    b->sharing_mode = AA_SHARING_MODE_SHARED;
    b->usage = AA_USAGE_MEDIA;
    b->content_type = AA_CONTENT_TYPE_MUSIC;
    b->direction = AA_DIRECTION_OUTPUT;
    b->input_preset = AA_INPUT_PRESET_GENERIC;
    b->frames_per_data_callback = 0;
    b->buffer_capacity = 0;
    return b;
}

/* NDK signature: the builder comes back through the out-parameter and the
 * return value is an aaudio_result_t.  Measured on the box, returning the
 * pointer itself instead made FMOD call this once, read the heap address as an
 * error code, leave its own builder variable NULL, and report
 * "Error initializing output device. " (60) without ever touching a setter. */
int32_t AAudio_createStreamBuilder(aa_builder **out)
{
    if (!out) {
        AAT("createStreamBuilder(NULL) -> ILLEGAL_ARGUMENT");
        return AA_ERROR_ILLEGAL_ARGUMENT;
    }
    aa_builder *b = aa_builder_new();
    *out = b;
    AAT("createStreamBuilder(out=%p) -> builder %p", (void *)out, (void *)b);
    return b ? AA_OK : AA_ERROR_NO_MEMORY;
}

#define AA_BUILDER_INT(field, member)                                          \
    void AAudioStreamBuilder_set##field(aa_builder *b, int32_t value)           \
    {                                                                           \
        AAT("set" #field "(%p, %d)", (void *)b, value);                         \
        if (b)                                                                  \
            b->member = value;                                                  \
    }

AA_BUILDER_INT(SampleRate, sample_rate)
AA_BUILDER_INT(ChannelCount, channel_count)
AA_BUILDER_INT(Format, format)
AA_BUILDER_INT(PerformanceMode, performance_mode)
AA_BUILDER_INT(SharingMode, sharing_mode)
AA_BUILDER_INT(Usage, usage)
AA_BUILDER_INT(ContentType, content_type)
AA_BUILDER_INT(InputPreset, input_preset)
AA_BUILDER_INT(Direction, direction)
AA_BUILDER_INT(DeviceId, device_id)
AA_BUILDER_INT(FramesPerDataCallback, frames_per_data_callback)
AA_BUILDER_INT(BufferCapacityInFrames, buffer_capacity)

/* Channel mask is the newer way to ask for stereo; the count is what the rest
 * of this implementation needs, so derive it from the mask. */
void AAudioStreamBuilder_setChannelMask(aa_builder *b, int32_t mask)
{
    AAT("setChannelMask(%p, %d)", (void *)b, mask);
    if (!b)
        return;
    b->channel_count = mask >= 3 && mask <= 5 ? 2 : (mask == 1 ? 1 : 2);
}

void AAudioStreamBuilder_setSessionId(aa_builder *b, int32_t session_id)
{
    /* No AudioFlinger sessions here; FMOD binds every entry point it references
     * and treats a NULL dlsym as an incomplete output backend. */
    AAT("setSessionId(%p, %d)", (void *)b, session_id);
    (void)b;
    (void)session_id;
}

int32_t AAudioStreamBuilder_setDataCallback(aa_builder *b, void *callback,
                                            void *user)
{
    AAT("setDataCallback(%p, cb=%p, user=%p)", (void *)b, callback, user);
    if (!b)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    b->data_callback = (int32_t (*)(void *, void *, void *, int32_t))callback;
    b->data_user = user;
    return AA_OK;
}

int32_t AAudioStreamBuilder_setErrorCallback(aa_builder *b, void *callback,
                                             void *user)
{
    AAT("setErrorCallback(%p, cb=%p, user=%p)", (void *)b, callback, user);
    if (!b)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    b->error_callback = (void (*)(void *, void *, int32_t))callback;
    b->error_user = user;
    return AA_OK;
}

int32_t AAudioStreamBuilder_openStream(aa_builder *b, void **out_stream)
{
    AAT("openStream(%p, out=%p) req: rate=%d ch=%d fmt=%d perf=%d share=%d "
        "frames_cb=%d cap=%d", (void *)b, (void *)out_stream,
        b ? b->sample_rate : -1, b ? b->channel_count : -1, b ? b->format : -1,
        b ? b->performance_mode : -1, b ? b->sharing_mode : -1,
        b ? b->frames_per_data_callback : -1, b ? b->buffer_capacity : -1);
    if (!b || !out_stream)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    aa_stream *s = NULL;
    for (int i = 0; i < AA_MAX_STREAMS; i++) {
        if (aa_streams[i].magic != AA_STREAM_MAGIC) {
            s = &aa_streams[i];
            break;
        }
    }
    if (!s) {
        fprintf(stderr, "[audio] AAudio: sem slot de stream livre\n");
        return AA_ERROR_NO_MEMORY;
    }
    memset(s, 0, sizeof *s);
    s->magic = AA_STREAM_MAGIC;
    s->req = *b;
    if (s->req.sample_rate < 8000 || s->req.sample_rate > 192000)
        s->req.sample_rate = 48000;
    if (s->req.channel_count < 1 || s->req.channel_count > 2)
        s->req.channel_count = 2;
    if (s->req.format != AA_FORMAT_PCM_FLOAT)
        s->req.format = AA_FORMAT_PCM_I16;
    s->burst = s->req.sample_rate / 60;
    if (s->burst < 128)
        s->burst = 128;
    s->buffer_size = s->burst * 4;
    s->state = AA_STATE_OPEN;
    s->started_at_ns = 0;
    int frames = s->req.frames_per_data_callback > 0
                   ? s->req.frames_per_data_callback : s->burst;
    int buffers = s->req.buffer_capacity > 0 && frames > 0
                    ? s->req.buffer_capacity / frames : 4;
    *out_stream = s;
    fprintf(stderr,
            "[audio] AAudio.openStream: %d Hz, %d canais, formato %s, "
            "callback=%s burst=%d frames_cb=%d buffers=%d\n",
            s->req.sample_rate, s->req.channel_count,
            s->req.format == AA_FORMAT_PCM_FLOAT ? "float" : "s16",
            s->req.data_callback ? "sim" : "nao", s->burst, frames, buffers);
    if (!sf_audio_device_init(s->req.channel_count, s->req.sample_rate, frames,
                              buffers)) {
        AAT("openStream: SDL device init FAILED -> AAUDIO_ERROR_DISCONNECTED");
        s->magic = 0;
        *out_stream = NULL;
        return AA_ERROR_DISCONNECTED;
    }
    AAT("openStream -> stream %p (AAUDIO_OK)", (void *)s);
    return AA_OK;
}

int32_t AAudioStreamBuilder_delete(aa_builder *b)
{
    AAT("builder delete(%p)", (void *)b);
    free(b);
    return AA_OK;
}

#define AA_STREAM_INT(field, expr)                                             \
    int32_t AAudioStream_get##field(void *stream)                              \
    {                                                                          \
        aa_stream *s = stream;                                                 \
        int32_t value =                                                        \
            s && s->magic == AA_STREAM_MAGIC ? (int32_t)(expr) : 0;            \
        AAT("get" #field "(%p) -> %d", stream, value);                          \
        return value;                                                          \
    }

AA_STREAM_INT(SampleRate, s->req.sample_rate)
AA_STREAM_INT(ChannelCount, s->req.channel_count)
AA_STREAM_INT(Format, s->req.format)
AA_STREAM_INT(FramesPerBurst, s->burst)
AA_STREAM_INT(FramesPerDataCallback, s->req.frames_per_data_callback)
AA_STREAM_INT(BufferCapacityInFrames, s->burst * 16)
AA_STREAM_INT(BufferSizeInFrames, s->buffer_size)
AA_STREAM_INT(PerformanceMode, s->req.performance_mode)
AA_STREAM_INT(SharingMode, s->req.sharing_mode)
AA_STREAM_INT(Usage, s->req.usage)
AA_STREAM_INT(ContentType, s->req.content_type)
AA_STREAM_INT(InputPreset, s->req.input_preset)
AA_STREAM_INT(Direction, s->req.direction)
AA_STREAM_INT(DeviceId, s->req.device_id)
AA_STREAM_INT(XRunCount, 0)
AA_STREAM_INT(AudioApiVersion, 8)
AA_STREAM_INT(State, s->state)
AA_STREAM_INT(SessionId, 1)

int32_t AAudioStream_setBufferSizeInFrames(void *stream, int32_t frames)
{
    aa_stream *s = stream;
    AAT("setBufferSizeInFrames(%p, %d)", stream, frames);
    if (!s || s->magic != AA_STREAM_MAGIC)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    s->buffer_size = frames;
    return frames;
}

int64_t AAudioStream_getFramesWritten(void *stream)
{
    aa_stream *s = stream;
    return s && s->magic == AA_STREAM_MAGIC ? s->frames_written : 0;
}

int64_t AAudioStream_getFramesRead(void *stream)
{
    aa_stream *s = stream;
    return s && s->magic == AA_STREAM_MAGIC ? s->frames_read : 0;
}

static int32_t aa_state_request(aa_stream *s, int32_t target)
{
    if (!s || s->magic != AA_STREAM_MAGIC)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    s->state = target;
    if (target == AA_STATE_RUNNING && s->req.data_callback && !s->pump_live) {
        s->pump_stop = 0;
        s->started_at_ns = aa_now_ns();
        if (pthread_create(&s->pump, NULL, aa_pump, s) == 0)
            s->pump_live = 1;
        else
            fprintf(stderr, "[audio] AAudio: nao deu thread de callback\n");
    }
    if (target != AA_STATE_RUNNING && s->pump_live) {
        s->pump_stop = 1;
        pthread_join(s->pump, NULL);
        s->pump_live = 0;
    }
    return AA_OK;
}

int32_t AAudioStream_requestStart(void *stream)
{
    AAT("requestStart(%p)", stream);
    return aa_state_request(stream, AA_STATE_RUNNING);
}
int32_t AAudioStream_requestPause(void *stream)
{
    AAT("requestPause(%p)", stream);
    return aa_state_request(stream, AA_STATE_PAUSED);
}
int32_t AAudioStream_requestStop(void *stream)
{
    AAT("requestStop(%p)", stream);
    return aa_state_request(stream, AA_STATE_STOPPED);
}
int32_t AAudioStream_requestFlush(void *stream)
{
    AAT("requestFlush(%p)", stream);
    return aa_state_request(stream, AA_STATE_FLUSHED);
}

int32_t AAudioStream_write(void *stream, const void *data, int32_t frames,
                           int64_t timeout_ns)
{
    aa_stream *s = stream;
    (void)timeout_ns;
    if (!s || s->magic != AA_STREAM_MAGIC)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    if (s->state != AA_STATE_RUNNING)
        return AA_ERROR_INVALID_STATE;
    size_t bytes = (size_t)frames * (size_t)s->req.channel_count
                   * (size_t)aa_bytes_per_frame(s->req.format);
    aa_submit(s, data, bytes);
    s->frames_written += frames;
    return frames;
}

int32_t AAudioStream_read(void *stream, void *data, int32_t frames,
                          int64_t timeout_ns)
{
    (void)stream; (void)data; (void)frames; (void)timeout_ns;
    /* Output-only device: nothing to capture. */
    return AA_ERROR_INVALID_STATE;
}

/* NDK form: the two outputs are separate int64 pointers, not a struct. */
int32_t AAudioStream_getTimestamp(void *stream, int32_t clock_id,
                                  int64_t *frame_position, int64_t *nano_time)
{
    aa_stream *s = stream;
    (void)clock_id;
    if (!s || s->magic != AA_STREAM_MAGIC || !frame_position || !nano_time)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    if (s->state != AA_STATE_RUNNING)
        return AA_ERROR_INVALID_STATE;
    *frame_position = s->frames_read;
    *nano_time = s->started_at_ns ? aa_now_ns() : 0;
    return AA_OK;
}

/* Every state change here is synchronous, so the answer is the current state at
 * once; when the stream is still in the state the caller waits to leave, take a
 * short nap instead of spinning the caller's loop. */
int32_t AAudioStream_waitForStateChange(void *stream, int32_t input_state,
                                        int32_t *next_state,
                                        int64_t timeout_ns)
{
    aa_stream *s = stream;
    AAT("waitForStateChange(%p, in=%d, timeout=%lld)", stream, input_state,
        (long long)timeout_ns);
    if (!s || s->magic != AA_STREAM_MAGIC)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    if (next_state && s->state != input_state) {
        *next_state = s->state;
        AAT("waitForStateChange -> immediate, state=%d", s->state);
        return AA_OK;
    }
    int64_t ns = timeout_ns;
    if (ns > 5000000LL)
        ns = 5000000LL;
    if (ns > 0) {
        struct timespec ts = { ns / 1000000000LL, ns % 1000000000LL };
        nanosleep(&ts, NULL);
    }
    if (next_state)
        *next_state = s->state;
    AAT("waitForStateChange -> timed out, state=%d", s->state);
    return AA_ERROR_TIMEOUT;
}

/* MMap is AAudio's exclusive-mode fast path and this bridge has none, so the
 * answer is "not used".  Measured on the box: FMOD calls this with the stream as
 * its only argument and the second register carries a leftover value, so writing
 * through a two-parameter "bool *" crashed on it (signal 11, fault address
 * 0x50464141 = this bridge's own stream magic).  ports/fp2-nextos, a Unity 6 +
 * FMOD port that plays audio on device, implements it single-argument too. */
int32_t AAudioStream_isMMapUsed(void *stream)
{
    AAT("isMMapUsed(%p) -> 0", stream);
    return 0;
}

int32_t AAudioStream_close(void *stream)
{
    aa_stream *s = stream;
    AAT("close(%p)", stream);
    if (!s || s->magic != AA_STREAM_MAGIC)
        return AA_ERROR_ILLEGAL_ARGUMENT;
    aa_state_request(s, AA_STATE_STOPPED);
    free(s->callback_buffer);
    memset(s, 0, sizeof *s);
    /* The SDL device is shared with the AudioTrack path, so closing a stream
     * releases the output the same way org/fmod/AudioDevice.close() does. */
    sf_audio_device_close();
    return AA_OK;
}

int32_t AAudio_getLibraryVersion(void) { return 0x00010000; }

static const char *const aa_result_text[] = {
    "AAUDIO_OK", "AAUDIO_ERROR_DISCONNECTED", "AAUDIO_ERROR_ILLEGAL_ARGUMENT",
    "AAUDIO_ERROR_INTERNAL", "AAUDIO_ERROR_INVALID_STATE"
};

const char *AAudio_convertResultToText(int32_t result)
{
    if (result == AA_OK)
        return aa_result_text[0];
    if (result == AA_ERROR_DISCONNECTED)
        return aa_result_text[1];
    if (result == AA_ERROR_ILLEGAL_ARGUMENT)
        return aa_result_text[2];
    if (result == AA_ERROR_INVALID_STATE)
        return aa_result_text[4];
    return "AAUDIO_ERROR_UNKNOWN";
}

static const nx_import aa_tab[] = {
    { "AAudio_createStreamBuilder",        (void *)AAudio_createStreamBuilder },
    { "AAudioStreamBuilder_setSampleRate",
      (void *)AAudioStreamBuilder_setSampleRate },
    { "AAudioStreamBuilder_setChannelCount",
      (void *)AAudioStreamBuilder_setChannelCount },
    { "AAudioStreamBuilder_setChannelMask",
      (void *)AAudioStreamBuilder_setChannelMask },
    { "AAudioStreamBuilder_setSessionId",
      (void *)AAudioStreamBuilder_setSessionId },
    { "AAudioStreamBuilder_setFormat",     (void *)AAudioStreamBuilder_setFormat },
    { "AAudioStreamBuilder_setPerformanceMode",
      (void *)AAudioStreamBuilder_setPerformanceMode },
    { "AAudioStreamBuilder_setSharingMode",
      (void *)AAudioStreamBuilder_setSharingMode },
    { "AAudioStreamBuilder_setUsage",      (void *)AAudioStreamBuilder_setUsage },
    { "AAudioStreamBuilder_setContentType",
      (void *)AAudioStreamBuilder_setContentType },
    { "AAudioStreamBuilder_setInputPreset",
      (void *)AAudioStreamBuilder_setInputPreset },
    { "AAudioStreamBuilder_setDirection",
      (void *)AAudioStreamBuilder_setDirection },
    { "AAudioStreamBuilder_setDeviceId",   (void *)AAudioStreamBuilder_setDeviceId },
    { "AAudioStreamBuilder_setFramesPerDataCallback",
      (void *)AAudioStreamBuilder_setFramesPerDataCallback },
    { "AAudioStreamBuilder_setBufferCapacityInFrames",
      (void *)AAudioStreamBuilder_setBufferCapacityInFrames },
    { "AAudioStreamBuilder_setDataCallback",
      (void *)AAudioStreamBuilder_setDataCallback },
    { "AAudioStreamBuilder_setErrorCallback",
      (void *)AAudioStreamBuilder_setErrorCallback },
    { "AAudioStreamBuilder_openStream",    (void *)AAudioStreamBuilder_openStream },
    { "AAudioStreamBuilder_delete",        (void *)AAudioStreamBuilder_delete },
    { "AAudioStream_getSampleRate",        (void *)AAudioStream_getSampleRate },
    { "AAudioStream_getChannelCount",      (void *)AAudioStream_getChannelCount },
    { "AAudioStream_getFormat",            (void *)AAudioStream_getFormat },
    { "AAudioStream_getFramesPerBurst",    (void *)AAudioStream_getFramesPerBurst },
    { "AAudioStream_getFramesPerDataCallback",
      (void *)AAudioStream_getFramesPerDataCallback },
    { "AAudioStream_getBufferCapacityInFrames",
      (void *)AAudioStream_getBufferCapacityInFrames },
    { "AAudioStream_getBufferSizeInFrames",
      (void *)AAudioStream_getBufferSizeInFrames },
    { "AAudioStream_setBufferSizeInFrames",
      (void *)AAudioStream_setBufferSizeInFrames },
    { "AAudioStream_getPerformanceMode",   (void *)AAudioStream_getPerformanceMode },
    { "AAudioStream_getSharingMode",       (void *)AAudioStream_getSharingMode },
    { "AAudioStream_getUsage",             (void *)AAudioStream_getUsage },
    { "AAudioStream_getContentType",       (void *)AAudioStream_getContentType },
    { "AAudioStream_getInputPreset",       (void *)AAudioStream_getInputPreset },
    { "AAudioStream_getDirection",         (void *)AAudioStream_getDirection },
    { "AAudioStream_getDeviceId",          (void *)AAudioStream_getDeviceId },
    { "AAudioStream_getXRunCount",         (void *)AAudioStream_getXRunCount },
    { "AAudioStream_getAudioApiVersion",   (void *)AAudioStream_getAudioApiVersion },
    { "AAudioStream_getState",             (void *)AAudioStream_getState },
    { "AAudioStream_getSessionId",         (void *)AAudioStream_getSessionId },
    { "AAudioStream_getFramesWritten",     (void *)AAudioStream_getFramesWritten },
    { "AAudioStream_getFramesRead",        (void *)AAudioStream_getFramesRead },
    { "AAudioStream_getTimestamp",         (void *)AAudioStream_getTimestamp },
    { "AAudioStream_requestStart",         (void *)AAudioStream_requestStart },
    { "AAudioStream_requestPause",         (void *)AAudioStream_requestPause },
    { "AAudioStream_requestStop",          (void *)AAudioStream_requestStop },
    { "AAudioStream_requestFlush",         (void *)AAudioStream_requestFlush },
    { "AAudioStream_waitForStateChange",   (void *)AAudioStream_waitForStateChange },
    { "AAudioStream_write",                (void *)AAudioStream_write },
    { "AAudioStream_read",                 (void *)AAudioStream_read },
    { "AAudioStream_isMMapUsed",           (void *)AAudioStream_isMMapUsed },
    { "AAudioStream_close",                (void *)AAudioStream_close },
    { "AAudio_getLibraryVersion",          (void *)AAudio_getLibraryVersion },
    { "AAudio_convertResultToText",        (void *)AAudio_convertResultToText },
};

const nx_import *sf_audio_table(size_t *n)
{
    *n = sizeof aa_tab / sizeof *aa_tab;
    return aa_tab;
}

/* ------------------------------------------------- FMODUnity's own setOutput
 *
 * Unity's engine mixer opens the AAudio bridge above and plays fine.  The game's
 * own C# layer (FMODUnity) then creates a *second* system and pins its output by
 * name from RuntimeManager.Initialize, which fails here with
 *   SystemNotInitializedException: [FMOD] Initialization failed :
 *   FMOD.System.setOutput : ERR_PLUGIN_MISSING
 * out of RG.Parsecs.Menu.PauseMenu.Awake / OptionsManager.Awake -- so the mixer
 * works, but no menu gets built and the scene stays black.
 *
 * IL2CPP reaches that entry point through dlsym, which is the one place this
 * port owns on the way in.  The wrapper reports what was asked for and, when
 * this FMOD build does not carry it, asks for each number in turn to learn the
 * set it does have, then picks the best real playback path from it.  Pinning a
 * number by hand (SF_FMOD_OUTPUT) skips that probe entirely, which is what a
 * comparison run needs.
 */

typedef int (*fmod_set_output_fn)(void *system, int output_type);

/* Numbering is FMOD Studio 2.x's own FMOD_OUTPUTTYPE (fmod.h / fmod.cs), not the
 * 1.x table: measured on the box this libfmod.so answers 0 (AUTODETECT) with
 * ERR_PLUGIN_MISSING=54 and accepts 2 3 4 5 15 16, exactly NOSOUND, WAVWRITER,
 * NOSOUND_NRT, WAVWRITER_NRT plus two platform outputs -- on an Android build
 * 16 is AAUDIO.  Which of 15/16 really is AAudio is settled by watching whether
 * the bridge above gets a createStreamBuilder once we ask for it. */
#define FMOD_OUTPUT_AUTODETECT  0
#define FMOD_OUTPUT_NOSOUND     2
#define FMOD_OUTPUT_AUDIOTRACK  9
#define FMOD_OUTPUT_OPENSL     10
#define FMOD_OUTPUT_AAUDIO     16
#define FMOD_OUTPUT_LAST_PROBED 24

/* setOutput on a system that has not been initialized only validates the type
 * against the loaded plugin list and stores it, so asking for every number in
 * turn is a safe way to learn what this particular libfmod.so really carries.
 * Worth learning rather than guessing: the Android output types are compile-time
 * options of the FMOD build, and this build was chosen by the game's vendor. */
static int aa_fmod_set_output(fmod_set_output_fn real, const char *tag,
                              void *system, int output_type)
{
    if (!real)
        return -1;

    /* A pinned output is used as asked, with no probe: the probe below leaves
     * its own last choice installed, so comparing two outputs across runs is
     * only meaningful when the pin is in charge. */
    const char *pin = getenv("SF_FMOD_OUTPUT");
    if (pin && *pin) {
        int want = atoi(pin);
        int r = real(system, want);
        fprintf(stderr, "[audio] %s(output=%d pinned by SF_FMOD_OUTPUT) -> %d\n",
                tag, want, r);
        return r;
    }

    int r = real(system, output_type);
    fprintf(stderr, "[audio] %s(output=%d) -> %d\n", tag, output_type, r);
    if (r == 0)
        return 0;

    int ok[FMOD_OUTPUT_LAST_PROBED + 1];
    int n_ok = 0;
    for (int t = 0; t <= FMOD_OUTPUT_LAST_PROBED; t++) {
        if (t == output_type)
            continue;
        if (real(system, t) == 0)
            ok[n_ok++] = t;
    }
    fprintf(stderr, "[audio] %s: outputs aceitos por este libfmod:", tag);
    if (n_ok == 0)
        fprintf(stderr, " (nenhum)");
    for (int i = 0; i < n_ok; i++)
        fprintf(stderr, " %d", ok[i]);
    fprintf(stderr, "\n");

    /* Prefer a real playback path, then silence: a studio system that inits
     * silently still builds the menu, which is what PauseMenu.Awake aborts
     * today.  AAudio first because that is the bridge this port already proves
     * it can feed. */
    static const int preference[] = { FMOD_OUTPUT_AAUDIO, FMOD_OUTPUT_AUDIOTRACK,
                                      FMOD_OUTPUT_OPENSL, FMOD_OUTPUT_NOSOUND };
    for (size_t p = 0; p < sizeof preference / sizeof *preference; p++) {
        for (int i = 0; i < n_ok; i++) {
            if (ok[i] != preference[p])
                continue;
            int r2 = real(system, ok[i]);
            fprintf(stderr, "[audio] %s: escolhido output %d -> %d\n", tag,
                    ok[i], r2);
            return r2;
        }
    }
    if (n_ok > 0) {
        int r2 = real(system, ok[0]);
        fprintf(stderr, "[audio] %s: escolhido output %d -> %d\n", tag, ok[0],
                r2);
        return r2;
    }
    return r;
}

static fmod_set_output_fn real_setoutput_v5;
static fmod_set_output_fn real_setoutput_v1;

int sf_fmod5_System_SetOutput(void *system, int output_type)
{
    return aa_fmod_set_output(real_setoutput_v5, "FMOD5_System_SetOutput",
                              system, output_type);
}

int sf_fmod_System_SetOutput(void *system, int output_type)
{
    return aa_fmod_set_output(real_setoutput_v1, "FMOD_System_SetOutput",
                              system, output_type);
}

/* ------------------------------------------------- the studio init that fails
 *
 * With setOutput satisfied the C# layer moves on and reports
 *   SystemNotInitializedException: [FMOD] Initialization failed :
 *   Studio::System::initialize : ERR_INTERNAL
 * three times (PauseMenu.Awake, OptionsManager.Awake, ...) while Unity itself
 * prints "defaulting to no-sound mode".  Nothing in that message says which of
 * its causes it is -- the core system not being initialized yet, an output that
 * this studio build rejects, or the banks being unreachable -- so each entry
 * point on that path is wrapped to echo its arguments and result.
 */

typedef int (*fmod_studio_create_fn)(void **system, unsigned int coreversion);
typedef int (*fmod_studio_init_fn)(void *system, int maxchannels,
                                   int studio_flags, int load_bank_flags,
                                   void *advancedsettings);
typedef int (*fmod_system_init_fn)(void *system, int maxchannels, int flags,
                                   void *extra);
typedef int (*fmod_studio_get_ll_fn)(void *studio_system, void **core_system);
typedef int (*fmod_studio_isvalid_fn)(void *studio_system);

static fmod_studio_create_fn real_studio_create;
static fmod_studio_init_fn real_studio_init;
static fmod_system_init_fn real_system_init;
static fmod_studio_get_ll_fn real_studio_get_ll;
static fmod_studio_isvalid_fn real_studio_isvalid;

typedef int (*fmod_studio_loadbank_fn)(void *system, const char *filename,
                                       unsigned int flags, void **bank);
static void sf_fmod_preload_banks(void *system);

static int sf_Studio_System_Create(void **system, unsigned int coreversion)
{
    int r = real_studio_create ? real_studio_create(system, coreversion) : -1;
    fprintf(stderr, "[audio] FMOD_Studio_System_Create(coreversion=%u.%u.%u) -> %d, system=%p\n",
            coreversion >> 16, (coreversion >> 8) & 0xff, coreversion & 0xff, r,
            (void *)(r == 0 && system ? *system : NULL));
    return r;
}

/* FMOD Studio hands the core system back through the studio object, so this is
 * the cheapest test of whether the handle create returned is one FMOD itself
 * accepts: a core pointer that comes back plausible means the studio object is
 * fine and the ERR_INTERNAL lives inside initialize, while a failure here means
 * the object never really existed. */
static int sf_Studio_System_GetLowLevelSystem(void *studio_system,
                                              void **core_system)
{
    int r = real_studio_get_ll ? real_studio_get_ll(studio_system, core_system) : -1;
    fprintf(stderr, "[audio] FMOD_Studio_System_GetLowLevelSystem(system=%p) -> %d, core=%p\n",
            studio_system, r, (void *)(r == 0 && core_system ? *core_system : NULL));
    return r;
}

static int sf_Studio_System_IsValid(void *studio_system)
{
    int r = real_studio_isvalid ? real_studio_isvalid(studio_system) : -1;
    fprintf(stderr, "[audio] FMOD_Studio_System_IsValid(system=%p) -> %d\n",
            studio_system, r);
    return r;
}

static int sf_Studio_System_Initialize(void *system, int maxchannels,
                                      int studio_flags, int load_bank_flags,
                                      void *advancedsettings)
{
    const char *pin = getenv("SF_FMOD_STUDIO_FLAGS");
    if (pin && *pin) {
        int want = (int)strtol(pin, NULL, 0);
        fprintf(stderr, "[audio] FMOD_Studio_System_Initialize: flags 0x%x trocados "
                        "por 0x%x (SF_FMOD_STUDIO_FLAGS)\n", studio_flags, want);
        studio_flags = want;
    }
    int r = real_studio_init ? real_studio_init(system, maxchannels, studio_flags,
                                                load_bank_flags, advancedsettings)
                             : -1;
    fprintf(stderr, "[audio] FMOD_Studio_System_Initialize(system=%p, maxchannels=%d, "
                    "studio_flags=0x%x, load_bank_flags=0x%x, advanced=%p) -> %d\n",
            system, maxchannels, studio_flags, load_bank_flags,
            advancedsettings, r);
    if (r == 0)
        sf_fmod_preload_banks(system);
    return r;
}

/* ------------------------------------------------------------------ banks
 *
 * 60 Parsecs! carrega os bancos FMOD atraves de Application.streamingAssetsPath,
 * que no Android vira "jar:file://!/assets/<bank>" -- mas o sourceDir chega
 * vazio no nosso loader, a URL 404 e os bancos nunca carregam.  Sem o Master
 * Bank, o bus "bus:/Master" nao existe e OptionsManager.InitializeFmodBus()
 * lanca BusNotFoundException DENTRO da corrotina
 * StartupConsoleManager.CompleteInitialization, que aborta -> o menu nunca
 * aparece -> tela preta apos o splash.  Carregamos os bancos aqui, no nivel
 * nativo, logo apos Studio::System::initialize, para que
 * RuntimeManager.GetBus("bus:/Master") encontre o bus e a corrotina prossiga.
 */

static void sf_fmod_preload_banks(void *system)
{
    static int done;
    if (done)
        return;
    done = 1;

    nx_mod *fm = nx_find_mod("libfmodstudio.so");
    if (!fm) {
        fprintf(stderr, "[audio] preload-banks: libfmodstudio.so nao residente\n");
        return;
    }
    fmod_studio_loadbank_fn loadbank =
        (fmod_studio_loadbank_fn)nx_lookup_in(fm,
                                             "FMOD_Studio_System_LoadBankFile");
    if (!loadbank) {
        fprintf(stderr, "[audio] preload-banks: "
                        "FMOD_Studio_System_LoadBankFile ausente\n");
        return;
    }

    DIR *d = opendir(sf_apk);
    if (!d) {
        fprintf(stderr, "[audio] preload-banks: nao abriu %s: %s\n",
                sf_apk, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t L = strlen(nm);
        if (L < 5 || strcmp(nm + L - 5, ".bank") != 0)
            continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", sf_apk, nm);
        void *bank = NULL;
        /* FMOD_STUDIO_LOAD_BANK_NORMAL = 0: carga sincrona, bus disponivel ja. */
        int br = loadbank(system, path, 0, &bank);
        fprintf(stderr, "[audio] preload-banks: loadBankFile(%s) -> %d bank=%p\n",
                path, br, bank);
    }
    closedir(d);
}

static int sf_System_Init(void *system, int maxchannels, int flags, void *extra)
{
    int r = real_system_init ? real_system_init(system, maxchannels, flags, extra) : -1;
    fprintf(stderr, "[audio] FMOD5_System_Init(system=%p, maxchannels=%d, flags=0x%x, "
                    "extra=%p) -> %d\n", system, maxchannels, flags, extra, r);
    return r;
}

/* Called by my_dlsym with the address it just resolved; returns the address the
 * caller should get.  Each name keeps its own real pointer because FMOD_ and
 * FMOD5_ are different entry points in libfmod.so. */
void *sf_audio_fmod_interpose(const char *sym, void *real)
{
    if (!sym)
        return real;
    if (strcmp(sym, "FMOD5_System_SetOutput") == 0) {
        real_setoutput_v5 = (fmod_set_output_fn)real;
        return (void *)sf_fmod5_System_SetOutput;
    }
    if (strcmp(sym, "FMOD_System_SetOutput") == 0) {
        real_setoutput_v1 = (fmod_set_output_fn)real;
        return (void *)sf_fmod_System_SetOutput;
    }
    if (strcmp(sym, "FMOD_Studio_System_Create") == 0) {
        real_studio_create = (fmod_studio_create_fn)real;
        return (void *)sf_Studio_System_Create;
    }
    if (strcmp(sym, "FMOD_Studio_System_Initialize") == 0) {
        real_studio_init = (fmod_studio_init_fn)real;
        return (void *)sf_Studio_System_Initialize;
    }
    if (strcmp(sym, "FMOD_Studio_System_GetLowLevelSystem") == 0) {
        real_studio_get_ll = (fmod_studio_get_ll_fn)real;
        return (void *)sf_Studio_System_GetLowLevelSystem;
    }
    if (strcmp(sym, "FMOD_Studio_System_IsValid") == 0) {
        real_studio_isvalid = (fmod_studio_isvalid_fn)real;
        return (void *)sf_Studio_System_IsValid;
    }
    if (strcmp(sym, "FMOD5_System_Init") == 0) {
        real_system_init = (fmod_system_init_fn)real;
        return (void *)sf_System_Init;
    }
    return real;
}
