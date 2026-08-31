#include <stdint.h>
#include <stddef.h>

void *custom_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    
    if (((uintptr_t)d % 4 == 0) && ((uintptr_t)s % 4 == 0)) {
        uint32_t *d32 = (uint32_t *)d;
        const uint32_t *s32 = (const uint32_t *)s;
        while (n >= 4) {
            *d32++ = *s32++;
            n -= 4;
        }
        d = (uint8_t *)d32;
        s = (const uint8_t *)s32;
    }
    
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}
