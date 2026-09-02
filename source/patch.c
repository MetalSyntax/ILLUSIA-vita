/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/utils.h"

extern so_module so_mod;

// SO_CONTINUE (so_util.h) assigns its call's result to a local `type r`,
// which doesn't compile for `void` -- same restore/call/repatch sequence,
// usable for any hook of a void-returning original function.
#define SO_CONTINUE_VOID(h, ...) do { \
    kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).orig_instr, sizeof((h).orig_instr)); \
    kuKernelFlushCaches((void *)(h).addr, sizeof((h).orig_instr)); \
    if ((h).thumb_addr) { ((void(*)())(h).thumb_addr)(__VA_ARGS__); } \
    else { ((void(*)())(h).addr)(__VA_ARGS__); } \
    kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).patch_instr, sizeof((h).patch_instr)); \
    kuKernelFlushCaches((void *)(h).addr, sizeof((h).patch_instr)); \
} while (0)

static void *hooked_Gcx_MM_Alloc(uint32_t size) {
    return calloc(1, size);
}

static void *hooked_Gcx_MM_Calloc(uint32_t size) {
    return calloc(1, size);
}

static void hooked_Gcx_MM_Free(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

static void *hooked_Gcx_MM_Realloc(void *ptr, uint32_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

static void *hooked_operator_new(uint32_t size) {
    return calloc(1, size);
}

// Bug #8/#9 (port_progress.md): CMvLayerData::PreLoad receives a "this" that
// custom_memcpy's guard shows is a small/invalid pointer (constant per map,
// e.g. 0x350, 0x42b) even though CMvMap::PreLoad's disassembly computes it as
// array_base + layer_index*0x18, where array_base comes straight from this
// operator new[] call ((layer_count*3+1)*8 bytes, e.g. 80 bytes for 3
// layers) -- which should be a normal calloc() heap pointer, not a tiny one.
// Logging every SMALL allocation here (skip textures/large buffers to avoid
// flooding the log) lets the next real run show directly whether the
// allocation itself is already bad, or whether the corruption happens later
// (e.g. when CMvMap+0xe4 is read back before each CMvLayerData::PreLoad call).
static void *hooked_operator_new_array(uint32_t size) {
    void *ptr = calloc(1, size);
    if (size <= 512) {
        l_info("operator_new[] size=%u -> %p", (unsigned int) size, ptr);
    }
    return ptr;
}

static void hooked_operator_delete(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

static void hooked_operator_delete_array(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

static void hooked_GxCreateGlobalHeap(void) {
    l_info("GxCreateGlobalHeap bypassed (using native heap).");
}

static void hooked_Gcx_MM_Init(void *buffer, uint32_t size) {
    // No-op
}

static int hooked_CreateMemory(void *buffer, int size) {
    return 1;
}

static void hooked_DestroyMemory(void) {
    // No-op
}

static uint32_t hooked_MC_knlGetTotalMemory(void) {
    return 256 * 1024 * 1024;
}

static uint32_t hooked_MC_knlGetFreeMemory(void) {
    return 200 * 1024 * 1024;
}

static void hooked_Gcx_MM_States(uint32_t *pTotal, uint32_t *pFree, uint32_t *pUsed) {
    if (pTotal) *pTotal = 256 * 1024 * 1024;
    if (pFree) *pFree = 200 * 1024 * 1024;
    if (pUsed) *pUsed = 56 * 1024 * 1024;
}

// Bug #8/#9/#10 (port_progress.md): CMvLayerData::PreLoad's "this" ends up as
// a tiny/invalid pointer (e.g. 0x34c) constant across all layers of a given
// map, even though CMvMap::PreLoad's real disassembly computes it as
// array_base + layer_index*0x18 from an operator new[] allocation we already
// confirmed (via hooked_operator_new_array's logging) returns a normal heap
// pointer (e.g. 0x82bfd380). Static disassembly of both CMvMap::PreLoad (the
// only call site in the whole .so) and CMvLayerData::PreLoad's own prologue
// checked out clean and self-consistent on every register (r8/r9/sl/fp
// save-restore, CMvLayer::CMvLayer not touching r8 at all) -- yet the runtime
// value contradicts it. This wraps the real entry point (not just the
// memcpy call site) with SO_CONTINUE so the exact "this" the .so itself
// passes in gets logged with zero behavior change, to settle definitively
// whether the corruption happens before this function is even called (bad
// argument at the call site) or something else.
static so_hook hook_CMvLayerData_PreLoad;

static int hooked_CMvLayerData_PreLoad(void *this_ptr, int layer, int param2,
                                        long param3, unsigned int param4) {
    l_info("CMvLayerData::PreLoad this=%p layer=%d param2=%d param3=0x%lx param4=0x%x",
           this_ptr, layer, param2, param3, (unsigned int) param4);
    return SO_CONTINUE(int, hook_CMvLayerData_PreLoad, this_ptr, layer, param2, param3, param4);
}

// Bug #10 (port_progress.md): CMvLayer::Draw crashes dereferencing a wild
// tileset pointer obtained by indexing the tileset table with a tile ID read
// from this layer's tile buffer (this_layer+0xc). custom_memcpy's guard
// already proves that SOME of CMvLayerData::PreLoad's writes into a layer
// object are lost (root cause still unconfirmed, see the hook above) --
// which, per CMvLayerData::PreLoad's own logic, means this+0xc can be left
// NULL (never written, only calloc-zeroed at construction) instead of
// pointing at the real tile data. Reading tile IDs from address 0 produces
// garbage that indexes out of range into the tileset table. Skip drawing a
// layer entirely if its tile buffer was never populated, instead of
// crashing the whole game -- this doesn't fix the underlying data-loss bug,
// but converts a fatal crash into "that layer doesn't render".
static so_hook hook_CMvLayer_Draw;

static int hooked_CMvLayer_Draw(void *layer, int x, int y, void *tile_points, int flag4, int flag5) {
    void *tile_buffer = *(void **) ((uint8_t *) layer + 0xc);
    if (!tile_buffer) {
        return 0;
    }
    return SO_CONTINUE(int, hook_CMvLayer_Draw, layer, x, y, tile_points, flag4, flag5);
}

// Bug #11 (port_progress.md): DrawMissionMode() crashes dereferencing this+0x84
// (a CGsKeymap* -- confirmed against out_ghidra.c:83854, in_r0[0x21] = piVar2,
// index 0x21*4 = 0x84) because CMvNetworkMenu::Initialize() used to be fully
// no-op'd for "offline mode". The real Initialize() (out_ghidra.c:83856-83959)
// never touches the network at all -- it only builds ~20 local CGsKeymap menu
// keymaps and a CGsKeymapScroll for local UI navigation, then calls
// ChangeState(). The only reason it was unsafe to run for real was Bug #3
// (Gcx_MM_Alloc's fixed-block allocator ran out of slots on the 5th CGsKeymap
// and returned NULL) -- now fixed by redirecting operator new/delete to the
// native heap, so there's no more reason to skip this function's real logic.
static so_hook hook_CMvNetworkMenu_Initialize;

static void hooked_CMvNetworkMenu_Initialize(void *self) {
    l_info("CMvNetworkMenu::Initialize running for real (local UI setup, no network I/O).");
    SO_CONTINUE_VOID(hook_CMvNetworkMenu_Initialize, self);
}

static void hooked_CMvNet_SendMain(void *self) {
    // No-op (offline)
}

static void hooked_CMvNet_Initialize(void *self) {
    l_info("CMvNet::Initialize stubbed (offline mode).");
}

static void hooked_CMvNetworkMenu_ResetKeyMap(void *self) {
    // No-op (offline)
}

// Bug #13 investigation (port_progress.md): user reports every character
// save load fails with "the save file has been corrupted, you must create a
// new character". CGsEncryptFile::LoadBegin (out_ghidra.c:7205) can fail
// that way for two completely different reasons -- a phone-number mismatch
// (GsGetPhoneNum() called at load time doesn't strcmp-match what was stored
// in the file at save time) or a checksum mismatch (CulCheckSum() over the
// decrypted payload doesn't match the stored value) -- and both collapse
// into the exact same generic failure with no distinguishing log output.
// These are diagnostic-only hooks (zero behavior change, same SO_CONTINUE/
// SO_CONTINUE_VOID passthrough pattern already used for CMvLayerData::PreLoad
// and CMvNetworkMenu::Initialize) that log the phone-number bytes and
// checksum value on every call, so the NEXT real save+load cycle's log
// tells us definitively which check is failing instead of guessing at a fix.
static so_hook hook_GsGetPhoneNum;

static void hooked_GsGetPhoneNum(char *out) {
    SO_CONTINUE_VOID(hook_GsGetPhoneNum, out);
    l_info("GsGetPhoneNum -> \"%.16s\"", out);
}

static so_hook hook_CulCheckSum;

static void hooked_CulCheckSum(void *this_ptr, void *data, uint32_t len) {
    SO_CONTINUE_VOID(hook_CulCheckSum, this_ptr, data, len);
    uint32_t checksum = *(uint32_t *)((uint8_t *)this_ptr + 0x8c);
    l_info("CGsEncryptFile::CulCheckSum this=%p len=%u -> checksum=0x%08x", this_ptr, (unsigned int) len, checksum);
}

static so_hook hook_CGsEncryptFile_LoadBegin;

static int hooked_CGsEncryptFile_LoadBegin(void *this_ptr, const char *path, int with_phone_check) {
    int ret = SO_CONTINUE(int, hook_CGsEncryptFile_LoadBegin, this_ptr, path, with_phone_check);
    l_info("CGsEncryptFile::LoadBegin(\"%s\", phone_check=%d) -> %d", path, with_phone_check, ret);
    return ret;
}

// Bug #13 root cause (confirmed via game_017.log): GsFSFileSize(out_ghidra.c:7391)
// gets the file size by calling MC_fsFileAttribute(out_ghidra.c:116808), which
// stat()s the file and copies one 32-bit word out of the result at a fixed byte
// offset (0x30, confirmed via objdump against the real .so) into its output. That
// offset falls inside `st_size` in our bionic-compatible `stat64_bionic` layout
// (source/reimpl/io.h) -- specifically its upper 32 bits, which are always 0 for
// any real save file. So GsFSFileSize always returns 0, and CGsEncryptFile::LoadBegin
// (out_ghidra.c:7205) bails out on its very first check (`if (uVar1 != 0)`) before
// ever calling CGsFile::Load, GsGetPhoneNum, or CulCheckSum -- matching game_017.log
// exactly: `stat(...Save0.dat): 0` (file found) immediately followed by
// `CGsEncryptFile::LoadBegin("Save0.dat", phone_check=1) -> 0` with neither of the
// other two diagnostic hooks above ever firing. This is a full replacement (not a
// passthrough): it bypasses the broken stat-offset read entirely and computes the
// real file size directly, using the exact same DATA_PATH"saves/"<name> real path
// that get_real_path() (out_ghidra.c:116681) and our own m_isFileExist/
// m_getAbsolueFilePath (source/java.c) already agree on.
static uint32_t hooked_GsFSFileSize(const char *name, int mode) {
    char path[512];
    snprintf(path, sizeof(path), "%ssaves/%s", DATA_PATH, name);
    uint32_t size = file_exists(path) ? (uint32_t) file_size(path) : 0;
    l_info("GsFSFileSize(\"%s\") -> %u", name, (unsigned int) size);
    return size;
}

// Investigation (reported by the user, 2026-09-02): FPS drops to ~3 for a
// moment when walking between stages/areas, and "the map" briefly shows up,
// which looked wrong to them. Two diagnostic-only hooks (same SO_CONTINUE/
// SO_CONTINUE_VOID passthrough pattern as everywhere else in this file, zero
// behavior change) to get real numbers instead of guessing:
//   1. CMvMap::Load(int,bool) -- the actual per-transition asset load (called
//      from CMvMap::DoMapChange, out_ghidra.c:65562, right after Release()).
//      Times how long it really takes; a real log will show whether the
//      stall is genuinely spent here (most likely, given every asset is read
//      through synchronous fopen/fread with no threading -- confirmed
//      pthread-free .so, port_progress.md Fase 3) or somewhere else.
//   2. CMvMap::OpenWorldMap() -- out_ghidra.c:65562's own DoMapChange() calls
//      this directly, unconditionally, right after loading certain field
//      maps (when CMvGameUI+0x140 == 0 and the new map's XLS-table-6 type
//      isn't 6) -- i.e. auto-opening the World Map screen after some map
//      transitions IS real, original engine behavior, not a bug we
//      introduced. Logging when it fires (and its return value: 0 means it
//      didn't actually open, e.g. GetMapIDForWorldMap() found nothing) tells
//      us whether "shows the map" is this feature legitimately running, just
//      made jarring by how slow the load before it is.
static so_hook hook_CMvMap_Load;

static int hooked_CMvMap_Load(void *this_ptr, int map_id, int unk) {
    uint64_t start = current_timestamp_ms();
    int ret = SO_CONTINUE(int, hook_CMvMap_Load, this_ptr, map_id, unk);
    l_info("CMvMap::Load(map_id=%d) -> %d took %llu ms", map_id, ret,
           (unsigned long long) (current_timestamp_ms() - start));
    return ret;
}

static so_hook hook_CMvMap_OpenWorldMap;

static int hooked_CMvMap_OpenWorldMap(void *this_ptr) {
    int ret = SO_CONTINUE(int, hook_CMvMap_OpenWorldMap, this_ptr);
    l_info("CMvMap::OpenWorldMap() -> %d", ret);
    return ret;
}

static void patch_symbol(const char *symbol_name, uintptr_t target_func) {
    uintptr_t addr = so_symbol(&so_mod, symbol_name);
    if (addr) {
        hook_addr(addr, target_func);
        l_info("Hooked %s -> 0x%08x", symbol_name, (unsigned int)addr);
    } else {
        l_warn("Symbol %s not found for hooking", symbol_name);
    }
}

void so_patch(void) {
    // Redirect internal Gcx_MM memory manager and operators new/delete to native malloc/free
    patch_symbol("_Z12Gcx_MM_Allocj", (uintptr_t)&hooked_Gcx_MM_Alloc);
    patch_symbol("_Z13Gcx_MM_Callocj", (uintptr_t)&hooked_Gcx_MM_Calloc);
    patch_symbol("_Z11Gcx_MM_FreePv", (uintptr_t)&hooked_Gcx_MM_Free);
    patch_symbol("_Z14Gcx_MM_ReallocPvj", (uintptr_t)&hooked_Gcx_MM_Realloc);
    patch_symbol("_Znwj", (uintptr_t)&hooked_operator_new);
    patch_symbol("_Znaj", (uintptr_t)&hooked_operator_new_array);
    patch_symbol("_ZdlPv", (uintptr_t)&hooked_operator_delete);
    patch_symbol("_ZdaPv", (uintptr_t)&hooked_operator_delete_array);
    patch_symbol("_Z18GxCreateGlobalHeapv", (uintptr_t)&hooked_GxCreateGlobalHeap);
    patch_symbol("_Z11Gcx_MM_InitPvj", (uintptr_t)&hooked_Gcx_MM_Init);
    patch_symbol("_Z12CreateMemoryPvi", (uintptr_t)&hooked_CreateMemory);
    patch_symbol("_Z13DestroyMemoryv", (uintptr_t)&hooked_DestroyMemory);
    patch_symbol("MC_knlGetTotalMemory", (uintptr_t)&hooked_MC_knlGetTotalMemory);
    patch_symbol("MC_knlGetFreeMemory", (uintptr_t)&hooked_MC_knlGetFreeMemory);
    patch_symbol("_Z13Gcx_MM_StatesPjS_S_", (uintptr_t)&hooked_Gcx_MM_States);

    // Bug #10: skip drawing a layer whose tile buffer never got populated
    // instead of crashing on the resulting garbage tileset lookup.
    uintptr_t cmv_layer_draw_addr = so_symbol(&so_mod, "_ZN8CMvLayer4DrawEiiP12tagTILEPOINTbb");
    if (cmv_layer_draw_addr) {
        hook_CMvLayer_Draw = hook_addr(cmv_layer_draw_addr, (uintptr_t)&hooked_CMvLayer_Draw);
        l_info("Hooked CMvLayer::Draw -> 0x%08x", (unsigned int) cmv_layer_draw_addr);
    } else {
        l_warn("Symbol CMvLayer::Draw not found for hooking");
    }

    // Diagnostic-only hook (see comment above hooked_CMvLayerData_PreLoad):
    // logs the real "this" the .so passes in, then calls through unchanged.
    uintptr_t cmv_layer_data_preload_addr = so_symbol(&so_mod, "_ZN12CMvLayerData7PreLoadE15EnumObjectLayerilj");
    if (cmv_layer_data_preload_addr) {
        hook_CMvLayerData_PreLoad = hook_addr(cmv_layer_data_preload_addr, (uintptr_t)&hooked_CMvLayerData_PreLoad);
        l_info("Hooked CMvLayerData::PreLoad -> 0x%08x", (unsigned int) cmv_layer_data_preload_addr);
    } else {
        l_warn("Symbol CMvLayerData::PreLoad not found for hooking");
    }

    // Bug #11: let CMvNetworkMenu::Initialize() run for real (local UI setup only,
    // see comment above hooked_CMvNetworkMenu_Initialize) instead of no-op'ing it.
    uintptr_t cmv_network_menu_init_addr = so_symbol(&so_mod, "_ZN14CMvNetworkMenu10InitializeEv");
    if (cmv_network_menu_init_addr) {
        hook_CMvNetworkMenu_Initialize = hook_addr(cmv_network_menu_init_addr, (uintptr_t)&hooked_CMvNetworkMenu_Initialize);
        l_info("Hooked CMvNetworkMenu::Initialize -> 0x%08x", (unsigned int) cmv_network_menu_init_addr);
    } else {
        l_warn("Symbol CMvNetworkMenu::Initialize not found for hooking");
    }

    // Bug #13 investigation: diagnostic-only hooks (see comment above
    // hooked_GsGetPhoneNum) to pin down why every character save load
    // reports "corrupted, create a new character".
    uintptr_t gs_get_phone_num_addr = so_symbol(&so_mod, "_Z13GsGetPhoneNumPc");
    if (gs_get_phone_num_addr) {
        hook_GsGetPhoneNum = hook_addr(gs_get_phone_num_addr, (uintptr_t)&hooked_GsGetPhoneNum);
        l_info("Hooked GsGetPhoneNum -> 0x%08x", (unsigned int) gs_get_phone_num_addr);
    } else {
        l_warn("Symbol GsGetPhoneNum not found for hooking");
    }

    uintptr_t cul_checksum_addr = so_symbol(&so_mod, "_ZN14CGsEncryptFile11CulCheckSumEPvj");
    if (cul_checksum_addr) {
        hook_CulCheckSum = hook_addr(cul_checksum_addr, (uintptr_t)&hooked_CulCheckSum);
        l_info("Hooked CGsEncryptFile::CulCheckSum -> 0x%08x", (unsigned int) cul_checksum_addr);
    } else {
        l_warn("Symbol CGsEncryptFile::CulCheckSum not found for hooking");
    }

    uintptr_t encrypt_file_load_begin_addr = so_symbol(&so_mod, "_ZN14CGsEncryptFile9LoadBeginEPKcb");
    if (encrypt_file_load_begin_addr) {
        hook_CGsEncryptFile_LoadBegin = hook_addr(encrypt_file_load_begin_addr, (uintptr_t)&hooked_CGsEncryptFile_LoadBegin);
        l_info("Hooked CGsEncryptFile::LoadBegin -> 0x%08x", (unsigned int) encrypt_file_load_begin_addr);
    } else {
        l_warn("Symbol CGsEncryptFile::LoadBegin not found for hooking");
    }

    // Bug #13 fix (see comment above hooked_GsFSFileSize): full replacement,
    // not a passthrough -- the original always returns 0.
    patch_symbol("_Z12GsFSFileSizePKci", (uintptr_t)&hooked_GsFSFileSize);

    // Stage-transition FPS/"map flash" investigation (see comment above
    // hooked_CMvMap_Load): diagnostic-only, times the real per-transition
    // load and logs every CMvMap::OpenWorldMap() call.
    uintptr_t cmv_map_load_addr = so_symbol(&so_mod, "_ZN6CMvMap4LoadEib");
    if (cmv_map_load_addr) {
        hook_CMvMap_Load = hook_addr(cmv_map_load_addr, (uintptr_t)&hooked_CMvMap_Load);
        l_info("Hooked CMvMap::Load -> 0x%08x", (unsigned int) cmv_map_load_addr);
    } else {
        l_warn("Symbol CMvMap::Load not found for hooking");
    }

    uintptr_t cmv_map_open_world_map_addr = so_symbol(&so_mod, "_ZN6CMvMap12OpenWorldMapEv");
    if (cmv_map_open_world_map_addr) {
        hook_CMvMap_OpenWorldMap = hook_addr(cmv_map_open_world_map_addr, (uintptr_t)&hooked_CMvMap_OpenWorldMap);
        l_info("Hooked CMvMap::OpenWorldMap -> 0x%08x", (unsigned int) cmv_map_open_world_map_addr);
    } else {
        l_warn("Symbol CMvMap::OpenWorldMap not found for hooking");
    }

    // Bypass dead online multiplayer/auction network subsystems
    patch_symbol("_ZN14CMvNetworkMenu11ResetKeyMapEv", (uintptr_t)&hooked_CMvNetworkMenu_ResetKeyMap);
    patch_symbol("_ZN6CMvNet8SendMainEv", (uintptr_t)&hooked_CMvNet_SendMain);
    patch_symbol("_ZN6CMvNet10InitializeEv", (uintptr_t)&hooked_CMvNet_Initialize);
    
    // Fix Bug #5: CMvLayerData::PreLoad skips execution because it checks `if (0 < (long)__src)`
    // Pointers on PS Vita are > 0x81000000, which are negative as signed longs.
    // 0x8ae62: ble -> beq (0xdd 0x1d -> 0xd0 0x1d)
    uint16_t beq_instr = 0xd01d;
    kuKernelCpuUnrestrictedMemcpy((void *)(so_mod.text_base + 0x8ae62), &beq_instr, sizeof(beq_instr));
}

