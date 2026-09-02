# Title

ILLUSIA - PS Vita Port v1.0.1 🎮✨

# Description

**ILLUSIA** for the PS Vita is out of Beta! This release builds directly on
the v0.9.0-beta foundation -- same unmodified original ARMv6 Android
executable (`libgameDSO.so`, Gamevil's **Nexus2** engine), running natively
through a custom SoLoader/FalsoJNI bridge with OpenGL ES 1.1 fixed-function
rendering on **vitaGL** -- with a full rewrite of the audio engine and a
round of real, root-caused bug fixes found and confirmed since the Beta,
each one cross-checked against the decompiled `.so` and real console logs,
not guessed. Full history in [`port_progress.md`](port_progress.md).

## 🔊 Audio Engine Rewrite (the big one this release)

The Beta's custom WAV mixer got replaced end to end after finding it was
silently playing the wrong content in several situations:

- **Real music, not sound effects, in-game and in cutscenes.** The `.so`
  never sends a literal sound-file number across the JNI boundary -- it
  resolves it to its position in an internal 70-entry registered-id list and
  sends *that index* instead (confirmed byte-for-byte by reading the real
  table straight out of `libgameDSO.so`'s `.data` section). Our old code
  treated that index as a literal filename, so it played unrelated
  low-numbered files instead of the actual BGM tracks the game intended --
  including the title screen's own theme. Fixed by translating the index
  back to the real id before every playback.
- **Higher-fidelity audio.** Switched from the bundled 8kHz mono `.wav`
  assets to the bundled 22050Hz stereo `.caf` (IMA4 ADPCM) assets -- the same
  recordings, noticeably crisper -- with a from-scratch IMA4/CAF decoder.
- **Correct volume, no more clipping/distortion.** A real gameplay log
  showed the engine always sends volume on a 0-100 scale, not the 0-10 the
  mixer assumed -- every sound was being amplified 10x and hard-clipping.
  Fixed to match the real scale.
- **No more overlapping ambient loops going silent after one playthrough**
  (fixed routing to match the real engine's own loop-vs-one-shot logic
  instead of a guessed id range).
- **The Gamevil boot jingle no longer plays ahead of its own logo** --
  audio output is now gated to start exactly in sync with the first real
  frame shown on screen.
- **Rebuilt on SDL2** (`SDL_OpenAudioDevice`) instead of a hand-rolled
  `sceAudioOut` thread, for a more standard, maintainable audio backend.

## ⚡ Performance

- Reduced file I/O overhead during stage/area transitions: removed a
  redundant existence-check syscall before every asset open, and gave the
  game's own file reads a larger buffer to cut down on small, repeated I/O
  calls. Confirmed smoother on real hardware, though very large or
  asset-heavy areas may still show a brief pause while loading -- still
  being profiled, see Known Issues.

## 🐛 Also Fixed Since the Beta

- **Save/load hardened further:** the `struct stat` layout fix from the
  Beta got a stricter, compile-time-verified version (`_Static_assert` on
  the real field offset), closing the door on the same class of corruption
  resurfacing on a toolchain update.
- **`CMvNetworkMenu::DrawMissionMode()` crash** during normal gameplay,
  caused by an over-broad "offline mode" stub that zeroed out UI state the
  renderer still expected to read every frame.
- A couple of build-only fixes (missing `sceIoMkdir` declaration, an SDL2
  vitasdk dependency needed for the audio rewrite above).

## ✨ Carried Over From the Beta

- **Full physical + touch controls**, including a SELECT-to-skip-cutscene
  mapping added specifically for this port (no original input the game
  exposes could otherwise advance a scripted cutscene).
- **Persistent save games** (`ux0:data/illusia/saves/`).
- **Clean boot** with vitaGL's own splashscreen disabled -- straight to the
  title screen.
- **30 FPS cap** matching the original engine's intended pacing.

## ⚠️ Known Issues

- One reported cutscene sound that may still be playing an unexpected id --
  under investigation with diagnostic logging in place, not yet reproduced
  with a confirmed root cause.
- Stage transitions are noticeably better after this release's I/O fix, but
  not fully profiled -- a brief pause is still possible on heavier areas.

Neither blocks normal play. If you hit either (or anything else), a console
log goes a long way -- see [`port_progress.md`](port_progress.md) for how
every other bug in this list was actually diagnosed.

## 🛠️ Installation Instructions

1. Install `kubridge.skprx` if you don't already have it (checked explicitly
   at boot, see `source/utils/init.c`).
2. Install `libshacccg.suprx` if you don't already have it (standard
   requirement for any homebrew using vitaGL/SceShaccCg to compile shaders).
3. Install the provided `illusia.vpk`.
4. Extract the `assets/` folder from your own legally obtained copy of the
   original ILLUSIA Android APK and copy it to `ux0:data/illusia/assets/`.
   (Note: game data files are **not** provided with this release -- this repo
   only distributes the port's code, never the original game's assets.)
5. Launch the game and enjoy! The first run automatically creates
   `ux0:data/illusia/saves/` for save data.

A huge thanks to the PS Vita homebrew community and the vitaGL contributors
for making this possible! Let us know your feedback and bug reports.
