// Run the actual device-side validation helpers on a host C compiler.
#include "../civ_format.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint8_t header[16] = {'C', 'I', 'V', '7', 'F', 'Z', '0', '1', 1, 0, 0, 0, 1, 0, 0, 0};
    assert(civ_header_valid(header, 28));
    assert(!civ_header_valid(header, 27));
    assert(!civ_header_valid(header, 29));
    header[7] = '2';
    assert(!civ_header_valid(header, 28));
    header[7] = '1';
    header[8] = 0;
    assert(!civ_header_valid(header, 16));
    memset(header + 8, 0xff, 4);
    assert(!civ_header_valid(header, 16 + (uint64_t)UINT32_MAX * 12));
    header[8] = 1;
    memset(header + 9, 0, 3);
    header[12] = 0;
    assert(!civ_header_valid(header, 28));

    uint8_t record[12] = {32};
    assert(civ_record_valid(record));
    record[2] = 2; // Shift + a single key.
    record[4] = 4;
    assert(civ_record_valid(record));
    record[0] = 0;
    assert(!civ_record_valid(record));
    record[0] = 32;
    record[2] = 1; // Ctrl is not part of this macro format.
    assert(!civ_record_valid(record));
    record[2] = 0;
    record[3] = 1;
    assert(!civ_record_valid(record));
    record[3] = 0;
    record[4] = 0x66;
    assert(!civ_record_valid(record));
    record[4] = 4;
    record[5] = 5; // Reject unordered multi-key reports.
    assert(!civ_record_valid(record));
    record[5] = 0;
    record[10] = 4;
    assert(civ_record_valid(record));
    record[10] = 8;
    assert(!civ_record_valid(record));
    puts("Device format validation passed");
    return 0;
}
