#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "logger.h"

#define MAX_SOUNDS 130
#define GRAIN 256
// Bug #12 (port_progress.md): a single shared SFX slot meant a new sound
// effect (e.g. casting a skill) silently overwrote/cut off whatever SFX was
// still playing (e.g. a hit sound) instead of playing alongside it -- very
// noticeable in combat, where hits/skills/UI sounds routinely overlap. This
// pool lets up to MAX_SFX_CHANNELS play at once, all mixed together below.
#define MAX_SFX_CHANNELS 6

// assets/sound/*.wav (8kHz mono PCM, built 2011-05-24) and assets/sound/*.caf
// (built 2011-09-22, ~4 months later) both ship in the real .apk with the
// exact same id set and, per id, the exact same duration (e.g. id 108: both
// report ~12.857s) -- they're the same recordings, just two encodings. The
// .caf is the higher-fidelity one: confirmed with Apple's own `afinfo` that
// every .caf in this asset set is uniformly 2 ch, 22050 Hz, IMA4 ADPCM
// ("ima4"), 68 bytes/packet (= 34 bytes/channel-block), 64 frames/packet.
// That's stereo at ~2.75x the sample rate of the mono/8kHz .wav we used to
// read directly as raw PCM -- noticeably crisper, which is what should be
// played instead.
#define CAF_SAMPLE_RATE 22050
#define CAF_CHANNELS 2
#define CAF_BYTES_PER_CHANNEL_BLOCK 34 // 2-byte header + 32 bytes (64 nibbles)
#define CAF_FRAMES_PER_PACKET 64

// Bug: every sound played "muy fuerte y distorsiona" (too loud, hard-clipped).
// The volume argument the .so passes to OnSoundPlay (see java.c's diagnostic
// log, port_progress.md Bug #16/#19) was assumed to be on a 0-10 scale, but a
// real gameplay log (logs/game_023.log) showed EVERY single OnSoundPlay call
// -- BGM/loop and SFX alike, 55/55 in that session, no exceptions -- sends
// vol=100. Dividing by 10 amplified every sample 10x; the saturation clamp
// added for Bug #16 stopped it from wrapping into noise, but it still meant
// nearly every sample hard-clipped to +/-32767. 100 is the real full-volume
// unit the engine uses (percent), confirmed empirically, not guessed.
#define VOLUME_SCALE 100

// Bug #20 (port_progress.md) -- THE major one. `CGsSound::PlaySound`
// (out_ghidra.c:19311) never sends the literal sound-file number across the
// JNI OnSoundPlay boundary. It resolves the caller's literal id (e.g. 0x6c=
// 108 for "RES_BGM_TITLE", confirmed via a debug string in the .so) to that
// id's POSITION inside an internal registered-id list via
// GetSoundIDListIdx(), and passes THAT INDEX to MC_androidPlaySound ->
// playAndroidSound -> the JNI OnSoundPlay call our java.c intercepts. This is
// invisible on Android because aapt assigns res/raw/sNNN.ogg resource ids in
// the exact same alphabetically-sorted order as this list, so `R.raw.s000 +
// index` (NexusSound.playSound in the decompiled Java) lands on the right
// file purely because both lists are sorted identically -- but our code was
// treating the incoming index as a literal filename number directly
// (snprintf("%03d.caf", index)), which is only correct for the first 3
// entries (ids 0,1,2 have no gap before them). This table is NOT inferred --
// it's byte-for-byte what CGsSound::OnCreate(this, 0x46, &DAT_00113a70)
// copies from the real .so's .data section (dumped directly from
// illusia_extract/lib/armeabi/libgameDSO.so at the correct file offset,
// vaddr 0x00103a70 = DAT_00113a70 minus this decompilation's consistent
// 0x10000 Ghidra-address offset -- see port_progress.md's vtable-placeholder
// note for the same offset quirk). Confirmed against logs/game_023.log:
// index 65 (loop=1) and index 58 (loop=1) -- which our old code played as
// the unrelated files 065.caf/058.caf -- resolve here to real ids 108 and
// 100, both genuine BGM tracks. 108 is exactly "RES_BGM_TITLE" per the .so's
// own debug print; 100 is the first dedicated in-game music track, matching
// the user's real-world comparison against Illusia 2's Android build (menu
// music vs. a distinct in-game music, not a looping effect).
#define NUM_SOUND_INDICES 70
static const uint8_t sound_index_table[NUM_SOUND_INDICES] = {
      0,  1,  2,  5,  6, 10, 13, 14, 15, 16, 17, 18, 20, 22, 23, 33,
     36, 37, 38, 39, 40, 41, 42, 44, 45, 46, 47, 54, 55, 56, 57, 58,
     59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74,
     75, 76, 77, 78, 79, 80, 81, 82, 83, 84,100,101,102,103,104,105,
    106,108,110,111,120,121
};

// Translates the index the .so hands OnSoundPlay into the real
// assets/sound/%03d.caf file number. Returns -1 for an out-of-range index.
static int sound_index_to_real_id(int index) {
    if (index < 0 || index >= NUM_SOUND_INDICES) {
        l_warn("sound_index_to_real_id: index %d out of range (0-%d)", index, NUM_SOUND_INDICES - 1);
        return -1;
    }
    return sound_index_table[index];
}

typedef struct {
    int16_t *data;          // interleaved L/R, CAF_CHANNELS per frame
    uint32_t length_frames; // frame count (NOT interleaved sample count)
} SoundFile;

static SoundFile sounds[MAX_SOUNDS];

static int bgm_playing = -1;
static int bgm_pos = 0;
static int bgm_loop = 0;
static int bgm_vol = VOLUME_SCALE;

typedef struct {
    int playing; // sound id, -1 = channel free
    uint32_t pos;
    int vol;
} SfxChannel;

static SfxChannel sfx_channels[MAX_SFX_CHANNELS];
static int sfx_next_evict = 0;

static SDL_AudioDeviceID audio_dev = 0;

// Bug: the Gamevil boot jingle was audible before its logo ever reached the
// screen. audio_init() (and the game's own early OnSoundPlay calls) run
// before main.c's render loop has swapped a single real frame -- synchronous
// asset/texture loading in the Native* init calls can take noticeably longer
// than our native mixer needs to start outputting samples, so the sting
// played out (or well into) that gap. The mixer now stays silent AND frozen
// (no position advance) until main.c calls audio_open_gate() right after its
// first real gl_swap(), so whatever was queued starts in sync with the first
// visible frame instead of running ahead of it.
static volatile int audio_gate_open = 0;

void audio_open_gate(void) {
    l_info("audio_open_gate: audio output enabled (first real frame shown)");
    audio_gate_open = 1;
}

// ---- QuickTime/CAF IMA4 ADPCM decode --------------------------------------
// Standard IMA ADPCM step/index tables (same ones used by every IMA4/DVI4
// decoder -- not engine-specific).
static const int16_t ima_step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,
    80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
    494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
    8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767
};
static const int8_t ima_index_table[16] = {
    -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8
};

static int16_t ima4_decode_nibble(int nibble, int *predictor, int *step_index) {
    int step = ima_step_table[*step_index];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    *predictor += (nibble & 8) ? -diff : diff;
    if (*predictor > 32767) *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;
    *step_index += ima_index_table[nibble];
    if (*step_index < 0) *step_index = 0;
    if (*step_index > 88) *step_index = 88;
    return (int16_t)*predictor;
}

// Decodes one 34-byte QuickTime IMA4 channel block (64 samples) into `out`,
// writing every `out_stride`-th slot (so callers can decode straight into an
// interleaved multi-channel buffer).
static void ima4_decode_block(const uint8_t *block, int16_t *out, int out_stride) {
    uint16_t header = ((uint16_t)block[0] << 8) | block[1];
    int predictor = (int16_t)(header & 0xFF80) >> 7; // sign-extend top 9 bits
    int step_index = header & 0x7F;
    if (step_index > 88) step_index = 88;
    for (int i = 0; i < 32; i++) {
        uint8_t byte = block[2 + i];
        out[(2 * i) * out_stride]     = ima4_decode_nibble(byte & 0x0F, &predictor, &step_index);
        out[(2 * i + 1) * out_stride] = ima4_decode_nibble(byte >> 4,   &predictor, &step_index);
    }
}

// Walks a CAF container's chunk list to find the "data" chunk. CAF chunk
// sizes are big-endian signed 64-bit; the data chunk's payload starts with a
// 4-byte mEditCount field before the actual audio bytes (confirmed against
// `afinfo`'s reported "audio data file offset", which is exactly 4 bytes past
// where the raw chunk payload starts).
static int caf_find_data_chunk(FILE *f, long *data_off, long *data_size) {
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8) return 0; // "caff" + version(2) + flags(2)
    if (memcmp(hdr, "caff", 4) != 0) return 0;

    for (;;) {
        uint8_t chdr[12];
        if (fread(chdr, 1, 12, f) != 12) return 0;
        int64_t size = 0;
        for (int i = 0; i < 8; i++) size = (size << 8) | chdr[4 + i];
        long payload_off = ftell(f);
        if (memcmp(chdr, "data", 4) == 0) {
            if (size < 4) return 0;
            *data_off = payload_off + 4;
            *data_size = (long)(size - 4);
            return 1;
        }
        if (size < 0) return 0; // unsupported: chunk with unknown/streamed size
        if (fseek(f, (long)size, SEEK_CUR) != 0) return 0;
    }
}

static void load_caf(int id) {
    if (id < 0 || id >= MAX_SOUNDS) return;
    if (sounds[id].data != NULL) return;

    char path[256];
    snprintf(path, sizeof(path), DATA_PATH"assets/sound/%03d.caf", id);

    FILE *f = fopen(path, "rb");
    if (!f) {
        l_warn("load_caf: %s not found", path);
        return;
    }

    long data_off = 0, data_size = 0;
    if (!caf_find_data_chunk(f, &data_off, &data_size) || data_size <= 0) {
        l_warn("load_caf: %s has no usable \"data\" chunk", path);
        fclose(f);
        return;
    }

    const long bytes_per_packet = CAF_BYTES_PER_CHANNEL_BLOCK * CAF_CHANNELS;
    long num_packets = data_size / bytes_per_packet;
    if (num_packets <= 0) {
        fclose(f);
        return;
    }

    uint8_t *raw = (uint8_t *)malloc((size_t)num_packets * bytes_per_packet);
    if (!raw) {
        fclose(f);
        return;
    }
    fseek(f, data_off, SEEK_SET);
    size_t read = fread(raw, 1, (size_t)num_packets * bytes_per_packet, f);
    fclose(f);
    if (read != (size_t)num_packets * bytes_per_packet) {
        free(raw);
        return;
    }

    uint32_t total_frames = (uint32_t)num_packets * CAF_FRAMES_PER_PACKET;
    int16_t *pcm = (int16_t *)malloc((size_t)total_frames * CAF_CHANNELS * sizeof(int16_t));
    if (!pcm) {
        free(raw);
        return;
    }

    for (long p = 0; p < num_packets; p++) {
        const uint8_t *packet = raw + p * bytes_per_packet;
        int16_t *out_base = pcm + (size_t)p * CAF_FRAMES_PER_PACKET * CAF_CHANNELS;
        for (int ch = 0; ch < CAF_CHANNELS; ch++) {
            ima4_decode_block(packet + ch * CAF_BYTES_PER_CHANNEL_BLOCK, out_base + ch, CAF_CHANNELS);
        }
    }
    free(raw);

    sounds[id].data = pcm;
    sounds[id].length_frames = total_frames;
    l_info("load_caf: %s -> %u frames (%.2fs)", path, total_frames,
           (double) total_frames / CAF_SAMPLE_RATE);
}

// Fills `frames` stereo frames (interleaved L/R) into `buf`. Shared by the
// SDL audio callback below; kept independent of SDL's buffer-size choice so
// it works no matter what `have.samples` SDL actually negotiates.
static void mix_audio(int16_t *buf, int frames) {
    memset(buf, 0, (size_t)frames * CAF_CHANNELS * sizeof(int16_t));
    if (!audio_gate_open) return;

    // Mix BGM/loop channel. `bgm_vol` is scaled by VOLUME_SCALE (see its
    // comment above -- confirmed 100 from a real gameplay log). The clamp
    // below is still kept even though vol=100/VOLUME_SCALE=100 is exactly
    // unity gain: the source audio can still legitimately peak at/near
    // full scale on its own, and this is cheap insurance against any
    // future case where the engine sends a >100 volume.
    if (bgm_playing >= 0 && sounds[bgm_playing].data != NULL) {
        for (int i = 0; i < frames; i++) {
            if ((uint32_t)bgm_pos >= sounds[bgm_playing].length_frames) {
                if (bgm_loop) bgm_pos = 0;
                else { bgm_playing = -1; break; }
            }
            const int16_t *frame = &sounds[bgm_playing].data[bgm_pos * CAF_CHANNELS];
            for (int ch = 0; ch < CAF_CHANNELS; ch++) {
                int32_t s = frame[ch] * bgm_vol / VOLUME_SCALE;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                buf[i * CAF_CHANNELS + ch] = (int16_t)s;
            }
            bgm_pos++;
        }
    }

    // Mix SFX -- every active channel gets summed (with clamping) into the
    // same buffer, instead of a single shared slot overwriting the last one.
    for (int c = 0; c < MAX_SFX_CHANNELS; c++) {
        int id = sfx_channels[c].playing;
        if (id < 0 || sounds[id].data == NULL) continue;

        for (int i = 0; i < frames; i++) {
            if (sfx_channels[c].pos >= sounds[id].length_frames) {
                sfx_channels[c].playing = -1;
                break;
            }
            const int16_t *frame = &sounds[id].data[sfx_channels[c].pos * CAF_CHANNELS];
            for (int ch = 0; ch < CAF_CHANNELS; ch++) {
                int32_t s = buf[i * CAF_CHANNELS + ch] + (frame[ch] * sfx_channels[c].vol / VOLUME_SCALE);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                buf[i * CAF_CHANNELS + ch] = (int16_t)s;
            }
            sfx_channels[c].pos++;
        }
    }
}

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
    (void) userdata;
    int frames = len / (CAF_CHANNELS * (int)sizeof(int16_t));
    mix_audio((int16_t *) stream, frames);
}

void audio_init() {
    memset(sounds, 0, sizeof(sounds));
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) sfx_channels[i].playing = -1;

    // Preload known BGM tracks (100-111) plus the ambient loops (120/121) to
    // avoid a stutter on first play; anything else lazy loads on demand.
    for(int i = 100; i <= 111; i++) load_caf(i);
    load_caf(120);
    load_caf(121);

    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        l_error("SDL_Init(SDL_INIT_AUDIO) failed: %s", SDL_GetError());
        return;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = CAF_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = CAF_CHANNELS;
    want.samples = GRAIN;
    want.callback = sdl_audio_callback;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev == 0) {
        l_error("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }
    l_info("SDL audio device open: freq=%d channels=%d samples=%d format=0x%04x",
           have.freq, have.channels, have.samples, have.format);

    SDL_PauseAudioDevice(audio_dev, 0); // unpause -- start pulling from sdl_audio_callback
}

void audio_play(int index, int vol, int loop) {
    int id = sound_index_to_real_id(index);
    if (id < 0) return;
    l_info("audio_play: index=%d -> real_id=%d vol=%d loop=%d", index, id, vol, loop);

    load_caf(id);

    // Route like the real engine (NexusSound.playSound): the `loop` flag,
    // not the sound id, decides the channel. Any looping request (music OR
    // an ambient loop, e.g. ids 120/121 which sit outside the 100-111 BGM
    // range) takes the single sustained slot, replacing whatever loop was
    // playing before -- same one-loop-at-a-time limitation the original
    // Android engine has with its single mBgmPlayer. Everything else is a
    // one-shot through the polyphonic SFX pool below. The old id-range
    // heuristic routed non-BGM-numbered loops into the SFX pool, which has
    // no concept of looping, so ambient sounds and looping power effects
    // played once and then went silent.
    if (loop) {
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
    l_info("audio_stop_bgm");
    bgm_playing = -1;
}

void audio_stop_all() {
    l_info("audio_stop_all");
    bgm_playing = -1;
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) sfx_channels[i].playing = -1;
}
