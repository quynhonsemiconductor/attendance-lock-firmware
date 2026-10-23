// reader: scaffold stub - compiles, no behaviour. Firmware v1 fills these in.
#include "reader/reader.h"

void reader_init(void) {
    // no-op in the scaffold
}

bool reader_poll(char *uid_out, size_t uid_len, reader_lane_t *lane_out) {
    return false; // scaffold stub
}

