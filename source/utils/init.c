/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/dirent.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;

// Memory pool for sceNet's own allocator, not related to the game's heap.
static char net_init_memory[16 * 1024];

void soloader_init_all() {
    // Before anything else logs: sceNet* must be up for utils/logger.c's
    // debugnet UDP forwarding (logs/live_session_*.log on the dev machine)
    // to have any chance of sending. Harmless if it fails (e.g. no Wi-Fi) --
    // _log_print()/_debugnet_send() just silently skip the sendto() then,
    // sceClibPrintf() keeps working either way.
    // SCE_SYSMODULE_NET must be loaded before sceNetInit()/socket() will do
    // anything at all -- without this, sceNetInit() (and every socket() the
    // game itself makes through dynlib.c's socket/connect hooks) silently
    // fails, which is exactly why the first attempt at UDP debug logging
    // sent 0 packets despite compiling and linking cleanly.
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam net_init_param = {
        .memory = net_init_memory,
        .size = sizeof(net_init_memory),
        .flags = 0
    };
    int net_ret = sceNetInit(&net_init_param);
    if (net_ret < 0) {
        // Doesn't go over UDP (that's the thing that's broken here) -- still
        // reaches sceClibPrintf/VitaShell's log window, since l_error isn't
        // gated by DEBUG_SOLOADER.
        l_error("sceNetInit failed: 0x%x -- UDP debug logging won't work this run.", net_ret);
    }
    sceNetCtlInit();

	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0) {
        sceIoMkdir(DATA_PATH"saves", 0777);
        l_success("FIOS initialized.");
    }
#endif

    void audio_init();
    audio_init();

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", SO_PATH);
    }
    // Format matches gdb_bridge.py's watch_for_so_base() regex (SO_BASE=0x...)
    // so the toolkit can auto-capture the real runtime base over the same
    // debugnet UDP line -- LOAD_ADDRESS above is only the requested base,
    // sceKernel's actual mapping can shift it slightly (see crash dumps'
    // "Base de memoria AUTO-DETECTADA" never landing exactly on 0x98000000).
    l_info("SO_BASE=0x%08x", (unsigned int) so_mod.text_base);

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");
}
