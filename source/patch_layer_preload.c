#include "patch.h"
#include <stdio.h>
#include <stdint.h>

extern void sceClibPrintf(const char *fmt, ...);

// Prototype of the original function
typedef uint32_t (*CMvLayerData_PreLoad_t)(void *this, uint32_t layer, uint32_t size, void *src, uint32_t offset);
CMvLayerData_PreLoad_t orig_CMvLayerData_PreLoad = (CMvLayerData_PreLoad_t)(0x9808ae50 | 1); // We'll patch this dynamically

uint32_t hooked_CMvLayerData_PreLoad(void *this, uint32_t layer, uint32_t size, void *src, uint32_t offset) {
    sceClibPrintf("[HOOK] CMvLayerData::PreLoad(this=0x%08X, layer=%u, size=%u, src=0x%08X, offset=0x%08X)\n", 
                  (uint32_t)this, layer, size, (uint32_t)src, offset);
    return orig_CMvLayerData_PreLoad(this, layer, size, src, offset);
}
