#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "logger.h"

#define MAX_SOUNDS 130
#define GRAIN 256
// Bug #12 (port_progress.md): a single shared SFX slot meant a new sound
// effect (e.g. casting a skill) silently overwrote/cut off whatever SFX was
// still playing (e.g. a hit sound) instead of playing alongside it -- very
// noticeable in combat, where hits/skills/UI sounds routinely overlap. This
// pool lets up to MAX_SFX_CHANNELS play at once, all mixed together below.
#define MAX_SFX_CHANNELS 6

typedef struct {
    int16_t *data;
    uint32_t length_samples;
} SoundFile;

static SoundFile sounds[MAX_SOUNDS];
static int port = -1;

static int bgm_playing = -1;
static int bgm_pos = 0;
static int bgm_loop = 0;
static int bgm_vol = 10;

typedef struct {
    int playing; // sound id, -1 = channel free
    uint32_t pos;
    int vol;
} SfxChannel;

static SfxChannel sfx_channels[MAX_SFX_CHANNELS];
static int sfx_next_evict = 0;

static SceUID audio_thd = -1;
static int audio_running = 0;

static void load_wav(int id) {
    if (id < 0 || id >= MAX_SOUNDS) return;
    if (sounds[id].data != NULL) return;

    char path[256];
    snprintf(path, sizeof(path), DATA_PATH"assets/sound/%03d.wav", id);

    FILE *f = fopen(path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 44, SEEK_SET); // Skip WAV header

    uint32_t data_size = size - 44;
    sounds[id].length_samples = data_size / 2;
    sounds[id].data = (int16_t*)malloc(data_size);
    if (sounds[id].data) {
        fread(sounds[id].data, 1, data_size, f);
    }
    fclose(f);
}

static int audio_thread(SceSize args, void *argp) {
    int16_t buf[GRAIN];

    while (audio_running) {
        memset(buf, 0, sizeof(buf));

        // Mix BGM
        if (bgm_playing >= 0 && sounds[bgm_playing].data != NULL) {
            for (int i = 0; i < GRAIN; i++) {
                if (bgm_pos >= sounds[bgm_playing].length_samples) {
                    if (bgm_loop) bgm_pos = 0;
                    else { bgm_playing = -1; break; }
                }
                int32_t s = sounds[bgm_playing].data[bgm_pos++] * bgm_vol / 10;
                buf[i] = s;
            }
        }

        // Mix SFX -- every active channel gets summed (with clamping) into the
        // same buffer, instead of a single shared slot overwriting the last one.
        for (int c = 0; c < MAX_SFX_CHANNELS; c++) {
            int id = sfx_channels[c].playing;
            if (id < 0 || sounds[id].data == NULL) continue;

            for (int i = 0; i < GRAIN; i++) {
                if (sfx_channels[c].pos >= sounds[id].length_samples) {
                    sfx_channels[c].playing = -1;
                    break;
                }
                int32_t s = buf[i] + (sounds[id].data[sfx_channels[c].pos++] * sfx_channels[c].vol / 10);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                buf[i] = (int16_t)s;
            }
        }

        if (port >= 0) {
            sceAudioOutOutput(port, buf);
        } else {
            sceKernelDelayThread(1000 * 1000 * GRAIN / 8000); // Wait manually if port failed
        }
    }
    return 0;
}

void audio_init() {
    memset(sounds, 0, sizeof(sounds));
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) sfx_channels[i].playing = -1;

    // Preload BGMs (100 to 111) to avoid lag, others lazy load
    for(int i = 100; i <= 111; i++) load_wav(i);

    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, 8000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO);
    if (port < 0) {
        l_error("Failed to open audio port");
    }

    audio_running = 1;
    audio_thd = sceKernelCreateThread("audio_thread", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(audio_thd, 0, NULL);
}

void audio_play(int id, int vol, int loop) {
    if (id < 0 || id >= MAX_SOUNDS) return;
    load_wav(id);
    
    if (id >= 100 && id <= 111) { // Assuming >= 100 is BGM
        bgm_playing = id;
        bgm_pos = 0;
        bgm_loop = loop;
        bgm_vol = vol;
        return;
    }

    // Find a free SFX channel; if all are busy, evict the oldest one
    // (round-robin) instead of dropping the new sound on the floor.
    int slot = -1;
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) {
        if (sfx_channels[i].playing < 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        slot = sfx_next_evict;
        sfx_next_evict = (sfx_next_evict + 1) % MAX_SFX_CHANNELS;
    }
    sfx_channels[slot].playing = id;
    sfx_channels[slot].pos = 0;
    sfx_channels[slot].vol = vol;
}

void audio_stop_bgm() {
    bgm_playing = -1;
}

void audio_stop_all() {
    bgm_playing = -1;
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) sfx_channels[i].playing = -1;
}
