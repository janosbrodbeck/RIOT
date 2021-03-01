#ifndef JELLING_DUPLICATE_DETECTION_H
#define JELLING_DUPLICATE_DETECTION_H

#include "string.h"
#include "net/ble.h"
#include "tsrb.h"

/**
 * @brief   The maximum number of stored packets in the duplicate detection system.
 *          Increase in areas of high BLE device density. Decrease in quiet
 *          environments.
 *          (Disable DD if the BLE controller duplicate filter is sufficient)
 */
#ifndef JELLING_DUPLICATE_DETECTION_ENTRY_COUNT
#define JELLING_DUPLICATE_DETECTION_ENTRY_COUNT         10
#endif

typedef struct  {
    uint8_t addr[BLE_ADDR_LEN];
    uint8_t pkt_num;
} duplicate_detection_entry_t;

void jelling_dd_init(void);
void jelling_dd_add(uint8_t *addr, uint8_t pkt_numy);
bool jelling_dd_check_for_entry(uint8_t *addr, uint8_t pkt_num);

#endif /* JELLING_DUPLICATE_DETECTION_H */
