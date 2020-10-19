#include "net/gnrc/pktbuf.h"

/**
 * @brief   Number of advertising events that should be sent
 *          before advertising ends and
 *          a BLE_GAP_EVENT_ADV_COMPLETE event is reported.
 *          Specify 0 for no limit.
 */
#ifndef JELLING_ADVERTISING_EVENTS
#define JELLING_ADVERTISING_EVENTS          (0)
#endif

/**
 * @brief   The duration of the advertisement procedure. On expiration, the
 *          procedure ends and a BLE_GAP_EVENT_ADV_COMPLETE event is reported.
 *          Units are 10 milliseconds. Specify 0 for no expiration.
 */
#ifndef JELLING_ADVERTISING_DURATION
#define JELLING_ADVERTISING_DURATION        (500)
#endif

/**
 * @brief   Status types of the NimBLE jelling module
 */
typedef enum {
    JELLING_INIT_ERROR = -2,
    JELLING_RUNTIME_ERROR = -1,
    JELLING_STOPPED = 0,         /**< network connector is stopped */
    JELLING_IDLE,                /**< network connector is idle  */
    JELLING_ADVERTISING,         /**< network connector is scanning */
    JELLING_SCANNING             /**< network connector is advertising */
} jelling_status_t;

int jelling_init(void);
void jelling_start(void);
void jelling_stop(void);
int jelling_send(gnrc_pktsnip_t *pkt);
void jelling_print_info(void);
