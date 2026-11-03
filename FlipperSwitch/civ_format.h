#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define CIV_HEADER_SIZE 16
#define CIV_RECORD_SIZE 12
#define CIV_MAX_RECORDS 100000

static inline uint16_t civ_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t civ_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline bool civ_header_valid(const uint8_t* header, uint64_t size) {
    uint32_t count = civ_u32(header + 8);
    uint32_t lines = civ_u32(header + 12);
    return memcmp(header, "CIV7FZ01", 8) == 0 && count > 0 && count <= CIV_MAX_RECORDS &&
           lines > 0 && lines <= 10000 &&
           size == CIV_HEADER_SIZE + (uint64_t)count * CIV_RECORD_SIZE;
}

static inline bool civ_record_valid(const uint8_t* r) {
    if(!civ_u16(r) || r[2] & ~2 || r[3] || r[4] > 0x65 ||
       (civ_u16(r + 10) != 0 && civ_u16(r + 10) != 4))
        return false;
    // This format deliberately permits one keyboard key per report.
    for(unsigned i = 5; i < 10; ++i) {
        if(r[i]) return false;
    }
    return true;
}
