#include "net/gnrc/pktbuf.h"

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
