#include <limits.h>
#include <errno.h>

#include "assert.h"
#include "thread.h"
#include "thread_flags.h"

#include "net/ble.h"
#include "net/bluetil/addr.h"
#include "net/gnrc/netif.h"
#include "net/gnrc/netif/hdr.h"

#include "net/gnrc/netreg.h"
#include "net/gnrc/pktbuf.h"
#include "net/gnrc/nettype.h"

#include "jelling_netif.h"
#include "jelling.h"

#include "nimble_riot.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "mem/mem.h"

#define ENABLE_DEBUG            0
#include "debug.h"

#ifdef MODULE_GNRC_SIXLOWPAN
#define NETTYPE                 GNRC_NETTYPE_SIXLOWPAN
#elif defined(MODULE_GNRC_IPV6)
#define NETTYPE                 GNRC_NETTYPE_IPV6
#else
#define NETTYPE                 GNRC_NETTYPE_UNDEF
#endif

/* allocate a stack for the netif device */
static char _stack[THREAD_STACKSIZE_DEFAULT];
static thread_t *_netif_thread;

/* keep the actual device state */
static gnrc_netif_t _netif;
static gnrc_nettype_t _nettype = NETTYPE;


static void _netif_init(gnrc_netif_t *netif)
{
    (void)netif;
    DEBUG("_netif_init: called\n");

    gnrc_netif_default_init(netif);
    _netif_thread = thread_get_active( );

#if IS_USED(MODULE_GNRC_NETIF_6LO)
    /* we enable fragmentation for this device, as the 6LoWPAN takes care of
     * of this */
    _netif.sixlo.max_frag_size = JELLING_MTU;
#endif
}

int nimble_netif_close(int handle)
{
    return 0;
}

static inline int _netdev_init(netdev_t *dev)
{
    (void)dev;

    DEBUG("_netdev_init: called\n");

    /* get our own address from the controller */
    uint8_t tmp[6];
    int res = ble_hs_id_copy_addr(nimble_riot_own_addr_type, tmp, NULL);

    assert(res == 0);
    (void)res;

    bluetil_addr_swapped_cp(tmp, _netif.l2addr);
    return 0;
}

static int _netif_send(gnrc_netif_t *netif, gnrc_pktsnip_t *pkt)
{
    assert(pkt->type == GNRC_NETTYPE_NETIF);
    (void)netif;

    jelling_send(pkt);
    gnrc_pktbuf_release(pkt);

    return 0;
}

/* not used, we pass incoming data to GNRC directly from the NimBLE thread */
static gnrc_pktsnip_t *_netif_recv(gnrc_netif_t *netif)
{
    (void)netif;
    return NULL;
}


static inline int _netdev_get(netdev_t *dev, netopt_t opt,
                              void *value, size_t max_len)
{
    (void)dev;
    int res = -ENOTSUP;

    switch (opt) {
        case NETOPT_ADDRESS:
            assert(max_len >= BLE_ADDR_LEN);
            memcpy(value, _netif.l2addr, BLE_ADDR_LEN);
            res = BLE_ADDR_LEN;
            break;
        case NETOPT_ADDR_LEN:
        case NETOPT_SRC_LEN:
            assert(max_len == sizeof(uint16_t));
            *((uint16_t *)value) = BLE_ADDR_LEN;
            res = sizeof(uint16_t);
            break;
        case NETOPT_MAX_PDU_SIZE:
            assert(max_len >= sizeof(uint16_t));
            *((uint16_t *)value) = JELLING_MTU;
            res = sizeof(uint16_t);
            break;
        case NETOPT_PROTO:
            assert(max_len == sizeof(gnrc_nettype_t));
            *((gnrc_nettype_t *)value) = _nettype;
            res = sizeof(gnrc_nettype_t);
            break;
        case NETOPT_DEVICE_TYPE:
            assert(max_len == sizeof(uint16_t));
            *((uint16_t *)value) = NETDEV_TYPE_BLE;
            res = sizeof(uint16_t);
            break;
        default:
            break;
    }

    return res;
}

static inline int _netdev_set(netdev_t *dev, netopt_t opt,
                              const void *value, size_t val_len)
{
    (void)dev;
    int res = -ENOTSUP;

    switch (opt) {
        case NETOPT_PROTO:
            assert(val_len == sizeof(_nettype));
            memcpy(&_nettype, value, sizeof(_nettype));
            res = sizeof(_nettype);
            break;
        default:
            break;
    }

    return res;
}


static const gnrc_netif_ops_t _nimble_netif_ops = {
    .init = _netif_init,
    .send = _netif_send,
    .recv = _netif_recv,
    .get = gnrc_netif_get_from_netdev,
    .set = gnrc_netif_set_from_netdev,
    .msg_handler = NULL,
};


static const netdev_driver_t _nimble_netdev_driver = {
    .send = NULL,
    .recv = NULL,
    .init = _netdev_init,
    .isr  =  NULL,
    .get  = _netdev_get,
    .set  = _netdev_set,
};

static netdev_t _nimble_netdev_dummy = {
    .driver = &_nimble_netdev_driver,
};


void jelling_netif_init(void)
{
    int res;
    DEBUG("jelling_netif_init: called\n");

    res = jelling_init(&_netif,_nettype);
    assert (res == 0);
    gnrc_netif_create(&_netif, _stack, sizeof(_stack), GNRC_NETIF_PRIO,
                    "jelling_netif", &_nimble_netdev_dummy, &_nimble_netif_ops);

}

gnrc_nettype_t nimble_jelling_get_nettype(void) {
    return _nettype;
}
