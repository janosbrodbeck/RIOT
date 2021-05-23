#include "mutex.h"
#include "jelling_duplicate_detection.h"

#define ENABLE_DEBUG            0
#include "debug.h"

static mutex_t _lock;
static duplicate_detection_entry_t _entries[JELLING_DUPLICATE_DETECTION_ENTRY_COUNT];
static uint8_t _pos;

void jelling_dd_init(void) {
    mutex_init(&_lock);
    memset(_entries, 0, sizeof(_entries));
    _pos = 0;
}

void jelling_dd_add(uint8_t *addr, uint8_t pkt_num) {
    /* insert into ringbuffer/list */
    mutex_lock(&_lock);
    memcpy(_entries[_pos].addr, addr, BLE_ADDR_LEN);
    _entries[_pos].pkt_num = pkt_num;
    mutex_unlock(&_lock);

    _pos++;
    if (_pos == JELLING_DUPLICATE_DETECTION_ENTRY_COUNT) {
        _pos = 0;
    }
}

bool jelling_dd_check_for_entry(uint8_t *addr, uint8_t pkt_num) {
    for (int i = 0; i < JELLING_DUPLICATE_DETECTION_ENTRY_COUNT; i++) {
        if (memcmp(_entries[i].addr, addr, BLE_ADDR_LEN) == 0 &&
                _entries[i].pkt_num == pkt_num) {
            DEBUG("DD: found duplicate\n");
            return true;
        }
    }
    return false;
}
