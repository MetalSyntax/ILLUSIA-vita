/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// UDP forwarding to psvita-toolkit's "Live Log Server" (debugnet_server.py),
// so `logs/live_session_*.log` actually gets lines instead of staying at
// 0 bytes -- that file only fills up if something on-device sends it UDP
// datagrams, and until now nothing here did (sceClibPrintf alone only goes
// to the kernel debug channel, not this listener). See port_progress.md,
// "arreglar el logging del toolkit" for the crash-triage motivation.
//
// DEBUGNET_HOST_IP is the DEV MACHINE's IP (not the Vita's) -- same subnet
// as `vita_ip` in .psvita-toolkit.json, but this repo can't know which of
// the host's own interfaces the Vita can actually reach. Verify/adjust this
// against `ifconfig`/`ipconfig` on the dev machine before relying on it.
#define DEBUGNET_HOST_IP "192.168.3.35"
#define DEBUGNET_PORT    9999

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

static int _debugnet_sock = -1;
static struct sockaddr_in _debugnet_dest;

static void _debugnet_send(const char* line) {
    if (_debugnet_sock < 0) {
        _debugnet_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (_debugnet_sock >= 0) {
            memset(&_debugnet_dest, 0, sizeof(_debugnet_dest));
            _debugnet_dest.sin_family = AF_INET;
            _debugnet_dest.sin_port = htons(DEBUGNET_PORT);
            inet_pton(AF_INET, DEBUGNET_HOST_IP, &_debugnet_dest.sin_addr);
        }
    }
    if (_debugnet_sock >= 0) {
        sendto(_debugnet_sock, line, strlen(line), 0,
               (struct sockaddr*) &_debugnet_dest, sizeof(_debugnet_dest));
    }
}

static char log_filename[128] = "";

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        for (int i = 1; i <= 999; i++) {
            sceClibSnprintf(log_filename, sizeof(log_filename), "ux0:data/illusia/logs/game_%03d.log", i);
            SceUID fd = sceIoOpen(log_filename, SCE_O_RDONLY, 0);
            if (fd < 0) break;
            sceIoClose(fd);
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf(buffer_b);
    _debugnet_send(buffer_b);

    if (log_filename[0] != '\0') {
        SceUID fd = sceIoOpen(log_filename, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, buffer_b, strlen(buffer_b));
            sceIoClose(fd);
        }
    }

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
