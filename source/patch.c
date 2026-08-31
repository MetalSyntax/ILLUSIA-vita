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
#include <stdlib.h>
#include <string.h>
#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

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
    // SO_CONTINUE assigns its call's result to a local `type r`, which doesn't
    // compile for `void` -- same restore/call/repatch sequence, inlined here.
    kuKernelCpuUnrestrictedMemcpy((void *)hook_CMvNetworkMenu_Initialize.addr,
                                  hook_CMvNetworkMenu_Initialize.orig_instr,
                                  sizeof(hook_CMvNetworkMenu_Initialize.orig_instr));
    kuKernelFlushCaches((void *)hook_CMvNetworkMenu_Initialize.addr,
                         sizeof(hook_CMvNetworkMenu_Initialize.orig_instr));
    if (hook_CMvNetworkMenu_Initialize.thumb_addr) {
        ((void(*)(void*))hook_CMvNetworkMenu_Initialize.thumb_addr)(self);
    } else {
        ((void(*)(void*))hook_CMvNetworkMenu_Initialize.addr)(self);
    }
    kuKernelCpuUnrestrictedMemcpy((void *)hook_CMvNetworkMenu_Initialize.addr,
                                  hook_CMvNetworkMenu_Initialize.patch_instr,
                                  sizeof(hook_CMvNetworkMenu_Initialize.patch_instr));
    kuKernelFlushCaches((void *)hook_CMvNetworkMenu_Initialize.addr,
                         sizeof(hook_CMvNetworkMenu_Initialize.patch_instr));
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

