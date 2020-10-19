#include <limits.h>
#include <errno.h>

#include "assert.h"
#include "thread.h"
#include "thread_flags.h"
#include "mutex.h"

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
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

#define ENABLE_DEBUG            (0)
#include "debug.h"
#define ADV_INSTANCES           (MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES+1)

typedef enum {
    STOPPED = 0,
    IDLE = 1,
    ADVERTISING = 2
} _adv_instance_status_t;

static int _configure_adv_instance(uint8_t instance);
static int _gap_event(struct ble_gap_event *event, void *arg);
static int _prepare_mbuf(gnrc_pktsnip_t *pkt, struct os_mbuf *mbuf);
static int _send_pkt(gnrc_pktsnip_t *pkt, uint8_t instance);

mutex_t _instance_status_lock;
static _adv_instance_status_t _instance_status[ADV_INSTANCES];
static jelling_status_t _jelling_status;


int jelling_send(gnrc_pktsnip_t* pkt) {
    /* jelling stopped or error appeared */
    if (_jelling_status <= 0) {
        return -1;
    }

    struct os_mbuf *buf = os_msys_get_pkthdr(JELLING_MTU+JELLING_HDR_RESERVED, 0);
    if(buf == NULL) {
        return -ENOBUFS;
    }

    int res;
    _send_pkt(pkt, 0);
    return 0;
}

static int _prepare_mbuf(gnrc_pktsnip_t *pkt, struct os_mbuf *mbuf)
{
    return 0;
}

static int _send_pkt(gnrc_pktsnip_t *pkt, uint8_t instance)
{
    size_t data_len = gnrc_pkt_len(pkt);
    int res;
    int max_events = 0;

   /*  if(_fragment(pkt, data_mbuf) != 0) {
        printf("could not fragment packets: %d\n", data_mbuf->om_len);
        return -1;
    } */

    /* res = ble_gap_ext_adv_set_data(instance, data_mbuf);
    if (res) {
        printf("Could not set advertising data: 0x%02X\n", res);
        return res;
    }

    res = ble_gap_ext_adv_start(instance, 0, max_events);
    if (res) {
        printf("Couldn't start advertising. Return code: 0x%02X\n", res);
        return -1;
    }
    printf("advertising started!\n");
    _jelling_status = JELLING_ADVERTISING; */
    return res;
}

static int _gap_event(struct ble_gap_event *event, void *arg)
{
    (void) arg;

    switch(event->type) {
        case BLE_GAP_EVENT_ADV_COMPLETE:
            printf("advertise complete; reason=%d, instance=%u, handle=%d\n",
                       event->adv_complete.reason, event->adv_complete.instance,
                       event->adv_complete.conn_handle);
            mutex_lock(&_instance_status_lock);
            _instance_status[event->adv_complete.instance] = IDLE;
            mutex_unlock(&_instance_status_lock);
            return 0;
    }
    printf("No registerd event occured!\n");
    return 0;
}

static int _configure_adv_instance(uint8_t instance) {
    struct ble_gap_ext_adv_params params = { 0 };
    int8_t selected_tx_power;
    int duration = INT16_MAX;

    memset(&params, 0, sizeof(params));

    /* set advertise parameters */
    params.scannable = 0;
    params.directed = 0;
    params.own_addr_type = nimble_riot_own_addr_type;
    params.channel_map = BLE_GAP_ADV_DFLT_CHANNEL_MAP;
    params.filter_policy = 0;
    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = 400;
    params.tx_power = 127;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;

    int rc = ble_gap_ext_adv_configure(instance, &params, &selected_tx_power, _gap_event, NULL);
    if (rc) {
        printf("_configure_adv_instance: failed to configure advertising instance. "
            "Return code: 0x%02X\n", rc);
        return -1;
    }

    ble_addr_t addr;
    rc = ble_hs_id_copy_addr(nimble_riot_own_addr_type, addr.val, NULL);
    addr.type = nimble_riot_own_addr_type;
    if (rc) {
        printf("_configure_adv_instance: failed to retrieve BLE address\n");
        return -1;
    }
    rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc) {
        printf("_configure_adv_instance: could not set address\n");
        return -1;
    }

    DEBUG("Instance %u configured (selected tx power: %d)\n", instance, selected_tx_power);
    return rc;
}

jelling_status_t jelling_status(void)
{
    return _jelling_status;
}

void jelling_start(void)
{
    mutex_lock(&_instance_status_lock);
    for (int i=0; i < ADV_INSTANCES; i++) {
        _instance_status[i] = IDLE;
    }
    mutex_unlock(&_instance_status_lock);
}

void jelling_stop(void)
{
    int rc;
    mutex_lock(&_instance_status_lock);
    for (int i=0; i < ADV_INSTANCES; i++) {
        rc = ble_gap_ext_adv_stop(i);
        if (rc && rc != 0x02)
        {
            printf("failed to stop advertising instance %d. But it will stop "
            "after it completes its events. Return code: 0x%02X\n", i, rc);
        }
        _instance_status[i] = STOPPED;
    }
    mutex_unlock(&_instance_status_lock);
    _jelling_status = JELLING_STOPPED;
}

int jelling_init(void)
{
    _jelling_status = JELLING_STOPPED;
    mutex_init(&_instance_status_lock);

    int res;
    for (int i = 0; i < ADV_INSTANCES; i++) {
        res = _configure_adv_instance(i);
        if (res < 0) {
            _jelling_status = JELLING_INIT_ERROR;
            break;
        }
        _instance_status[i] = STOPPED;
    }
    return 0;
}

void jelling_print_info(void)
{
    uint8_t own_addr[BLE_ADDR_LEN];
    uint8_t tmp_addr[BLE_ADDR_LEN];
    ble_hs_id_copy_addr(nimble_riot_own_addr_type, tmp_addr, NULL);
    bluetil_addr_swapped_cp(tmp_addr, own_addr);
    printf("Own Address: ");
    bluetil_addr_print(own_addr);
    #ifdef MODULE_GNRC_IPV6
        printf(" -> ");
        bluetil_addr_ipv6_l2ll_print(own_addr);
    #endif
    puts("");
    printf("Advertising instances: %d\n", ADV_INSTANCES);
    printf("MTU: %d bytes\n", JELLING_MTU);
    printf("Jelling status: ");
    switch(_jelling_status) {
        case JELLING_INIT_ERROR:
            printf("INIT ERROR\n");
            break;
        case JELLING_RUNTIME_ERROR:
            printf("RUNTIME ERROR\n");
            break;
        case JELLING_IDLE:
            printf("IDLE\n");
            break;
        case JELLING_STOPPED:
            printf("STOPPED\n");
            break;
        case JELLING_ADVERTISING:
            printf("ADVERTISING\n");
            break;
        case JELLING_SCANNING:
            printf("SCANNING\n");
            break;
    }
    for(int i=0; i < ADV_INSTANCES; i++) {
        printf("Instance %d: ", i);
        switch(_instance_status[i]) {
            case STOPPED:
                printf("STOPPED\n");
                break;
            case IDLE:
                printf("IDLE\n");
                break;
            case ADVERTISING:
                printf("ADVERTISING\n");
                break;
        }
    }
}
