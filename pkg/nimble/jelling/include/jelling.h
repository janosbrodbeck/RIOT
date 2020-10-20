#include "net/gnrc/pktbuf.h"

#ifndef JELLING_ADVERTISER_ENABLE
#define JELLING_ADVERTISER_ENABLE           (1)
#endif

#ifndef JELLING_ADVERTISER_VERBOSE
#define JELLING_ADVERTISER_VERBOSE          (0)
#endif

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

#ifndef JELLING_SCANNER_ENABLE
#define JELLING_SCANNER_ENABLE              (1)
#endif

#ifndef JELLING_SCANNER_VERBOSE
#define JELLING_SCANNER_VERBOSE             (0)
#endif

#ifndef JELLING_SCANNER_ITVL
#define JELLING_SCANNER_ITVL                (BLE_GAP_SCAN_FAST_INTERVAL_MIN)
#endif

#ifndef JELLING_SCANNER_WINDOW
#define JELLING_SCANNER_WINDOW              (BLE_GAP_SCAN_FAST_WINDOW)
#endif

#ifndef JELLING_SCANNER_DURATION
#define JELLING_SCANNER_DURATION            (0)
#endif

#ifndef JELLING_SCANNER_PERIOD
#define JELLING_SCANNER_PERIOD              (0)
#endif

#ifndef JELLING_SCANNER_WAIT_TILL_NEXT
#define JELLING_SCANNER_WAIT_TILL_NEXT      (150)
#endif

/**
 * @brief   Status types of the NimBLE jelling module
 */
typedef enum {
    JELLING_INIT_ERROR = -2,
    JELLING_RUNTIME_ERROR = -1,
    JELLING_STOPPED = 0,         /**< network connector is stopped */
    JELLING_RUNNING = 1
} jelling_status_t;

int jelling_init(void);
void jelling_start(void);
void jelling_stop(void);
int jelling_send(gnrc_pktsnip_t *pkt);
void jelling_print_info(void);
