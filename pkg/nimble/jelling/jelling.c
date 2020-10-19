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
#define L2_ADDR_LEN             (6)
#define VENDOR_ID_1             (0xFE)
#define VENDOR_ID_2             (0xED)

typedef enum {
    STOPPED = 0,
    IDLE = 1,
    ADVERTISING = 2
} _adv_instance_status_t;

static int _configure_adv_instance(uint8_t instance);
static int _gap_event(struct ble_gap_event *event, void *arg);
static int _prepare_mbuf(gnrc_pktsnip_t *pkt, gnrc_netif_hdr_t *hdr, struct os_mbuf *mbuf);
static int _send_pkt(struct os_mbuf *mbuf);
static size_t _prepare_hdr_in_buf(uint8_t *buf, gnrc_netif_hdr_t *hdr, bool zfirst);

mutex_t _instance_status_lock;
static _adv_instance_status_t _instance_status[ADV_INSTANCES];
static jelling_status_t _jelling_status;


int jelling_send(gnrc_pktsnip_t* pkt) {
    /* jelling stopped or error appeared */
    if (_jelling_status <= 0) {
        return -1;
    }

    /* allocate mbuf */
    struct os_mbuf *buf = os_msys_get_pkthdr(JELLING_MTU+JELLING_HDR_RESERVED, 0);
    if(buf == NULL) {
        return -ENOBUFS;
    }

    /* get netif_hdr */
    gnrc_netif_hdr_t *hdr = (gnrc_netif_hdr_t *)pkt->data;

    int res = _prepare_mbuf(pkt, hdr, buf);
    if (res != 0) {
        printf("jelling_send: mbuf_append failed. Return code: %d\n", res);
        os_mbuf_free_chain(buf);
        return -1;
    }
    _send_pkt(buf);
    os_mbuf_free_chain(buf);
    return 0;
}

static int _prepare_mbuf(gnrc_pktsnip_t *pkt, gnrc_netif_hdr_t *hdr, struct os_mbuf *mbuf)
{
    size_t buf_len = 0;
    /* helper buffer */
    uint8_t buf[JELLING_MTU+JELLING_HDR_RESERVED];
    const uint8_t MAX_SIZE = JELLING_MTU > 255 ? 255 : JELLING_MTU;

    /* written bytes in the curr fragment */
    uint8_t len_written = 0;

    /* pos_buf := position in buffer; pos_len := length field of current fragment */
    uint8_t *pos_buf, *pos_len;
    pos_len = buf;

    /* write first fragment with dst l2 address */
    len_written += _prepare_hdr_in_buf(buf, hdr, true);
    pos_buf = buf + len_written;

    /* drop first snippet. We only used it to determine the dst address */
    pkt = pkt->next;

    while (pkt) {
        uint8_t len_curr_pkt = pkt->size;
        /* offset for position in the current gnrc_pktsnip */
        uint8_t pkt_offset = 0;
        while(len_curr_pkt != 0) {
            if (len_curr_pkt <= MAX_SIZE-len_written) {
                /* we fit in the current fragment */
                memcpy(pos_buf, pkt->data+pkt_offset, len_curr_pkt);
                pos_buf += len_curr_pkt;
                len_written += len_curr_pkt;
                len_curr_pkt = 0;
            } else { /* we don't fit */
                uint8_t to_write = MAX_SIZE-len_written;
                memcpy(pos_buf, pkt->data+pkt_offset, to_write);
                pkt_offset += to_write;
                pos_buf += to_write;
                len_written += to_write;
                len_curr_pkt -= to_write;
            }

            /* fragment is full */
            if (len_written == MAX_SIZE)
            {
                /* write len field, -1 because the length field does not count */
                memset(pos_len, MAX_SIZE-1, 1);
                pos_len = pos_buf;
                pos_buf += 1;
                buf_len += len_written;

                /* there is more data */
                if (len_curr_pkt != 0 || pkt->next != NULL) {
                    /* write new header in next fragment */
                    len_written = _prepare_hdr_in_buf(buf, hdr, false);
                    pos_buf += len_written;
                }
            }
        }
        pkt = pkt->next;
    }
    /* started fragment left */
    if (len_written != 0) {
        /* write len field, -1 because the length field does not count */
        memset(pos_len, len_written-1, 1);
        buf_len += len_written;
    }

    /* copy helper buffer into mbuf */
    int res = os_mbuf_append(mbuf, buf, buf_len);
    return res;
}

static size_t _prepare_hdr_in_buf(uint8_t *buf, gnrc_netif_hdr_t *hdr, bool first) {
    /* one because of the reserved len field */
    size_t len = 1;

     /* data type: manufacturer specific data */
    memset(buf+len, BLE_GAP_AD_VENDOR, 1);
    /* 2 octet company identifier code */
    memset(buf+len+1, VENDOR_ID_1, 1);
    memset(buf+len+2, VENDOR_ID_2, 1);
    len += 3;
    if (first) {
        if (hdr->flags & GNRC_NETIF_HDR_FLAGS_MULTICAST) {
            memset(buf+len, 0xff, 1);
            memset(buf+len+1, 0, L2_ADDR_LEN-1);
        }
        else { /* unicast */
            /* insert destination l2 address */
            uint8_t *dst_addr = gnrc_netif_hdr_get_dst_addr(hdr);
            memcpy(buf+len, dst_addr, L2_ADDR_LEN);
        }
        len += L2_ADDR_LEN;
    }
    return len;
}

static int _send_pkt(struct os_mbuf *mbuf)
{
    int res;
    uint8_t instance = -1;

    /* find free advertising instace */
    mutex_lock(&_instance_status_lock);
    for (int i = 0; i < ADV_INSTANCES; i++) {
        if (_instance_status[i] == IDLE) {
            _instance_status[i] = ADVERTISING;
            instance = i;
            break;
        }
    }
    mutex_unlock(&_instance_status_lock);

    if (instance == -1) {
        return -1;
    }

    res = ble_gap_ext_adv_set_data(instance, mbuf);
    if (res) {
        printf("Could not set advertising data: 0x%02X\n", res);
        return res;
    }

    res = ble_gap_ext_adv_start(instance, JELLING_ADVERTISING_DURATION,
                            JELLING_ADVERTISING_EVENTS);
    if (res) {
        printf("Couldn't start advertising. Return code: 0x%02X\n", res);
        return -1;
    }
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
    _jelling_status = JELLING_IDLE;
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
