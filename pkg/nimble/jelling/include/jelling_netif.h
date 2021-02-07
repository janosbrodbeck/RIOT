/**
 * @defgroup    pkg_nimble_jelling IPv6-over-BLE-Advertising netif Implementation
 * @ingroup     pkg_nimble
 * @brief       GNRC netif implementation for NimBLE, enabling IPv6-over-BLE-Advertising
 *              approach.
 *
 * @{
 *
 * @file
 * @brief       IPv6-over-BLE-Advertising netif implementation for GNRC/NimBLE
 *
 * @author      János Brodbeck <janos.brodbeck@fu-berlin.de>
 */

#ifndef JELLING_NETIF_H
#define JELLING_NETIF_H

#include <stdint.h>

#include "net/ble.h"

#include "host/ble_hs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Default MTU size supported by the NimBLE jelling wrapper
 */
/* NOTE: We do not use the @ref IPV6_MIN_MTU define here, as the iov6.h header
         pulls in some other RIOT headers that clash with NimBLE header (e.g.
 *       byteorder.h vs. endian.h) */
#ifndef JELLING_IPV6_MTU
#define JELLING_IPV6_MTU             1280
#endif
/**
 * @brief   Reserved bytes for flags and headers
 */
#ifndef JELLING_HDR_RESERVED
#define JELLING_HDR_RESERVED    MYNEWT_VAL_BLE_EXT_ADV_MAX_SIZE
#endif

/**
 * @brief   Initialize the jelling netif implementation, spawns the netif thread
 *
 * This function is meant to be called once during system initialization, i.e.
 * auto-init.
 */
void nimble_jelling_init(void);
gnrc_nettype_t nimble_jelling_get_nettype(void);

#ifdef __cplusplus
}
#endif

#endif /* JELLING_NETIF_H */
/** @} */
