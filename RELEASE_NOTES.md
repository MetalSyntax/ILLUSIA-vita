# Title

ILLUSIA Vita v0.9.0-beta - First playable beta release

# Description

Native PS Vita port of ILLUSIA (Android, Gamevil Nexus2 engine), running the
original libgameDSO.so unmodified through a SoLoader + FalsoJNI bridge.

**Highlights:**
- Playable start to finish on real hardware (Vita/PSTV): story, combat, and menus.
- Native sound (custom WAV mixer with a 6-channel polyphonic SFX pool), 30 FPS cap
  matching the original engine's pacing.
- Physical button + touchscreen controls, with SELECT mapped to skip scripted cutscenes.
- Persistent save games (`ux0:data/illusia/saves/`), fixed at the root cause: a
  bionic/newlib `struct stat` layout mismatch was making the engine's own save-size
  check always fail, so loading a save reported "the save file has been corrupted."
- No more rotating vitaGL logo/animation before the title screen -- vitaGL is now
  vendored and built with its splashscreen disabled instead of using the stock
  VITASDK package.
- Full LiveArea assets.
- 13 confirmed and root-caused engine/loader bugs fixed (memory allocator
  exhaustion, signed-pointer checks, memcpy alignment, endianness, offline
  network-menu stubs, single-channel SFX overwriting hits/skills, broken save-size
  detection, and more) -- see `port_progress.md` for the full log.

This is a beta: the game is fully playable end to end, with no known open
issues at the time of this release. See `README.md` and `port_progress.md`
for details.
