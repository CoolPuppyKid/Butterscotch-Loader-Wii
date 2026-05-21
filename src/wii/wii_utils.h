#include "common.h"
#include <stdint.h>

static uint16_t readLE16(const uint8_t* p) {
    return
        ((uint16_t)p[0]) |
        ((uint16_t)p[1] << 8);
}

static uint32_t readLE32(const uint8_t* p) {
    return
        ((uint32_t)p[0]) |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static int16_t readLES16(const uint8_t* p) {
    return (int16_t)readLE16(p);
}