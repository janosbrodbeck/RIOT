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
#if JELLING_DUPLICATE_DETECTION_ENABLE
    #include "jelling_duplicate_detection.h"
#endif

#include "nimble_riot.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

#define ENABLE_DEBUG            (0)
#include "debug.h"
#define ADV_INSTANCES           (MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES+1)
#define VENDOR_ID_1             (0xFE)
#define VENDOR_ID_2             (0xED)

/* offset between data type (manufacturer specific data) and
 * start of next hop address */
#define PACKET_NEXT_HOP_OFFSET  (4)
#define PACKET_PKG_NUM_OFFSET   (10)
#define PACKET_DATA_OFFSET      (11)

typedef enum {
    ADDR_MULTICAST = 1,
    ADDR_UNICAST
} _address_type_t;

typedef enum {
    STOPPED = 0,
    IDLE = 1,
    ADVERTISING = 2
} _adv_instance_status_t;

static int _send_pkt(struct os_mbuf *mbuf);
static int _start_scanner(void);
static int _gap_event(struct ble_gap_event *event, void *arg);
static void _scan_complete(void);
static void _on_data(struct ble_gap_event *event, void *arg);
static int _prepare_mbuf(gnrc_pktsnip_t *pkt, gnrc_netif_hdr_t *hdr, struct os_mbuf *mbuf);
static size_t _prepare_hdr_in_buf(uint8_t *buf, gnrc_netif_hdr_t *hdr, bool first);
static int _configure_adv_instance(uint8_t instance);
static bool _filter_manufacturer_id(uint8_t *data, uint8_t len);
static uint8_t _filter_next_hop_addr(uint8_t *data, uint8_t len);

static mutex_t _instance_status_lock;
static _adv_instance_status_t _instance_status[ADV_INSTANCES];
static jelling_status_t _jelling_status;
static jelling_config_t _config;

static gnrc_netif_t *_netif;
static gnrc_nettype_t _nettype;

static uint8_t _pkt_next_num;
static uint8_t _ble_addr[BLE_ADDR_LEN];
static uint8_t _ble_mc_addr[BLE_ADDR_LEN];

int jelling_send(gnrc_pktsnip_t* pkt) {
    /* jelling stopped or error appeared */
    if (_jelling_status <= 0) {
        return -1;
    }
    if (!_config.advertiser_enable) {
        return 0;
    }

    /* if configured: don't send ICMP messages */
    if (_config.advertiser_block_icmp) {
        gnrc_pktsnip_t *tmp = pkt;
        while (tmp) {
            if (tmp-> type == GNRC_NETTYPE_ICMPV6) {
                printf("Skipped ICMP\n");
                return 0;
            }
            tmp = tmp->next;
        }
    }

    /* allocate mbuf */
    struct os_mbuf *buf = os_msys_get_pkthdr(JELLING_MTU+JELLING_HDR_RESERVED, 0);
    if (buf == NULL) {
        return -ENOBUFS;
    }

    /* get netif_hdr */
    gnrc_netif_hdr_t *hdr = (gnrc_netif_hdr_t *)pkt->data;

    /* fragment data for NimBLE/BLE and write it directly into the mbuf */
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
            memcpy(buf+len, _ble_mc_addr, BLE_ADDR_LEN);
        }
        else { /* unicast */
            /* insert destination l2 address */
            uint8_t *dst_addr = gnrc_netif_hdr_get_dst_addr(hdr);
            memcpy(buf+len, dst_addr, BLE_ADDR_LEN);
        }
        len += BLE_ADDR_LEN;
    }
    memset(buf+len, _pkt_next_num, 1);
    len += 1;
    _pkt_next_num++;
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

static int _start_scanner(void) {
    struct ble_gap_ext_disc_params uncoded = {0};
    uint8_t limited = 0;
    uint8_t filter_duplicates = 1;
    uint16_t duration = JELLING_SCANNER_DURATION;
    uint16_t period = JELLING_SCANNER_PERIOD;

    /* Set uncoded parameters */
    uncoded.passive = 1;
    uncoded.itvl = JELLING_SCANNER_ITVL;
    uncoded.window = JELLING_SCANNER_WINDOW;

    /* start scan */
    int rc = ble_gap_ext_disc(nimble_riot_own_addr_type, duration, period, filter_duplicates,
                            BLE_HCI_SCAN_FILT_NO_WL, limited, &uncoded, NULL, _gap_event, NULL);
    if(rc != 0) {
        printf("_start_scanner failed. Return code %02X\n", rc);
    }
    return rc;
}

static int _gap_event(struct ble_gap_event *event, void *arg)
{
    (void) arg;

    switch(event->type) {
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if(_config.advertiser_verbose) {
                printf("advertise complete; reason=%d, instance=%u, handle=%d\n",
                        event->adv_complete.reason, event->adv_complete.instance,
                        event->adv_complete.conn_handle);
            }
            mutex_lock(&_instance_status_lock);
            _instance_status[event->adv_complete.instance] = IDLE;
            mutex_unlock(&_instance_status_lock);
            return 0;
        case BLE_GAP_EVENT_EXT_DISC:
            _on_data(event, arg);
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            printf("BLE_GAP_EVENT_DISC_COMPLETE\n");
            _scan_complete();
            return 0;

    }
    return 0;
}

static void _on_data(struct ble_gap_event *event, void *arg)
{
    /* if we have sth in our filter list */
    if (!_config.scanner_filter_empty) {
        bool match = false;
        for (int i = 0; i < JELLING_SCANNER_FILTER_SIZE; i++) {
            if (!_config.scanner_filter[i].empty) {
                if (memcmp(_config.scanner_filter[i].addr,
                        event->ext_disc.addr.val, 6) == 0) {
                    match = true;
                    break;
                }
            }
        }
        if (!match) {
            return;
        }
    }
    bool jelling_packet = _filter_manufacturer_id((uint8_t *)event->ext_disc.data,
                        event->ext_disc.length_data);

    /* print info */
    if(_config.scanner_verbose) {
        printf("Address: ");
        bluetil_addr_print(event->ext_disc.addr.val);
        printf("    Data Length: %d bytes    ", event->ext_disc.length_data);
        switch(event->ext_disc.data_status) {
            case BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE:
                printf("COMPLETE    ");
                break;
            case BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE:
                printf("INCOMPLETE    ");
                break;
            case BLE_GAP_EXT_ADV_DATA_STATUS_TRUNCATED:
                printf("TRUNCATED    ");
                break;
        }
        if (jelling_packet) {
            printf("Jelling packet\n");
        } else { printf("Unknown packet\n"); }
    }

    /* do not process any further if unknown packet */
    if (!jelling_packet) {
        return;
    }

    uint8_t next_hop_match = _filter_next_hop_addr((uint8_t *)event->ext_disc.data+PACKET_NEXT_HOP_OFFSET,
        event->ext_disc.length_data-PACKET_NEXT_HOP_OFFSET);
    if(!next_hop_match) {
        return;
    }

#if JELLING_DUPLICATE_DETECTION_ENABLE
    if (_config.duplicate_detection_enable) {
        uint8_t num;
        memcpy(&num, event->ext_disc.data+PACKET_PKG_NUM_OFFSET, 1);
        if (jelling_dd_check_for_entry(event->ext_disc.addr.val, num)) {
            return;
        }
        jelling_dd_add(event->ext_disc.addr.val, num);
    }
#endif

    /* prepare gnrc pkt */
    gnrc_pktsnip_t *if_snip;
    /* destination can be multicast or unicast addr */
    if (next_hop_match == ADDR_UNICAST) {
        if_snip = gnrc_netif_hdr_build(event->ext_disc.addr.val, BLE_ADDR_LEN,
                        _ble_addr, BLE_ADDR_LEN);
    } else {
        if_snip = gnrc_netif_hdr_build(event->ext_disc.addr.val, BLE_ADDR_LEN,
                        _ble_mc_addr, BLE_ADDR_LEN);
    }

    /* we need to add the device PID to the netif header */
    gnrc_netif_hdr_set_netif(if_snip->data, _netif);

    /* allocate space in the pktbuf to store the packet */
    size_t data_size = event->ext_disc.length_data-PACKET_DATA_OFFSET;
    gnrc_pktsnip_t *payload = gnrc_pktbuf_add(if_snip, NULL, data_size,
                                _nettype);
    if (payload == NULL) {
        gnrc_pktbuf_release(if_snip);
        printf("Payload allocation failed\n");
        return;
    }

    /* copy payload from event into pktbuffer */
    memcpy(payload->data, event->ext_disc.data+PACKET_DATA_OFFSET, data_size);

    /* finally dispatch the receive packet to gnrc */
    if (!gnrc_netapi_dispatch_receive(payload->type, GNRC_NETREG_DEMUX_CTX_ALL,
                                    payload)) {
        printf("Could not dispatch\n");
        gnrc_pktbuf_release(payload);
    }
}

static void _scan_complete(void) {

}

static uint8_t _filter_next_hop_addr(uint8_t *data, uint8_t len) {
    /* cmp with own addr and multicast addr */
    if (memcmp(data, _ble_mc_addr, BLE_ADDR_LEN) == 0) {
        return ADDR_MULTICAST;
    }
    if (memcmp(data, _ble_addr, BLE_ADDR_LEN) == 0) {
        return ADDR_UNICAST;
    }
    return 0;
}

static bool _filter_manufacturer_id(uint8_t *data, uint8_t len) {
    if (len > 4) {
        if (data[2] == VENDOR_ID_1 && data[3] == VENDOR_ID_2) {
            return true;
        }
    }
    return false;
}

static int _configure_adv_instance(uint8_t instance) {
    struct ble_gap_ext_adv_params params = { 0 };
    int8_t selected_tx_power;

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

    if(_config.scanner_enable) {
        _start_scanner();
    }
    _jelling_status = JELLING_RUNNING;
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

int jelling_init(gnrc_netif_t *netif, gnrc_nettype_t nettype)
{
    _netif = netif;
    _nettype = nettype;

    _jelling_status = JELLING_STOPPED;
    jelling_load_default_config();
    _pkt_next_num = 0;
#if JELLING_DUPLICATE_DETECTION_ENABLE
    jelling_dd_init();
#endif

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

    /* workaround: save L2 ble random addr */
    uint8_t tmp_addr[BLE_ADDR_LEN];
    ble_hs_id_copy_addr(nimble_riot_own_addr_type, tmp_addr, NULL);
    bluetil_addr_swapped_cp(tmp_addr, _ble_addr);

    /* prepare multicast addr for comparison */
    memset(_ble_mc_addr, 0xFF, 1);
    memset(_ble_mc_addr+1, 0, 5);
    return 0;
}

void jelling_print_info(void)
{
    uint8_t own_addr[BLE_ADDR_LEN];
    uint8_t tmp_addr[BLE_ADDR_LEN];
    ble_hs_id_copy_addr(nimble_riot_own_addr_type, tmp_addr, NULL);
    bluetil_addr_swapped_cp(tmp_addr, own_addr);
    printf("Own Address: ");
    bluetil_addr_print(_ble_addr);
    #ifdef MODULE_GNRC_IPV6
        printf(" -> ");
        bluetil_addr_ipv6_l2ll_print(_ble_addr);
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
        case JELLING_STOPPED:
            printf("STOPPED\n");
            break;
        case JELLING_RUNNING:
            printf("RUNNING\n");
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

int jelling_filter_add(char *addr)
{
    int pos = -1;
    /* find empty space in filter list */
    for (int i = 0; i < JELLING_SCANNER_FILTER_SIZE; i++) {
        if (_config.scanner_filter[i].empty) {
            pos = i;
            break;
        }
    }
    if (pos == -1) {
        return -1;
    }

    if (bluetil_addr_from_str(_config.scanner_filter[pos].addr, addr) == NULL) {
        return -1;
    }

    /* swap address (ask hauke why our addr is represented swapped) */
    uint8_t tmp[BLE_ADDR_LEN];
    bluetil_addr_swapped_cp(_config.scanner_filter[pos].addr, tmp);
    memcpy(_config.scanner_filter[pos].addr, tmp, BLE_ADDR_LEN);

    _config.scanner_filter_empty = false;
    _config.scanner_filter[pos].empty = false;
    return 0;
}

void jelling_filter_clear(void)
{
    memcpy(_config.scanner_filter, 0, sizeof(_config.scanner_filter));
    for (int i=0; i < JELLING_SCANNER_FILTER_SIZE; i++) {
        _config.scanner_filter[i].empty = true;
    }
    _config.scanner_filter_empty = true;
}

void jelling_load_default_config(void)
{
    _config.advertiser_enable = true;
    _config.advertiser_verbose = false;
    _config.advertiser_block_icmp = false;
    _config.scanner_enable = true;
    _config.scanner_verbose = false;
    memcpy(_config.scanner_filter, 0, sizeof(_config.scanner_filter));
    for (int i=0; i < JELLING_SCANNER_FILTER_SIZE; i++) {
        _config.scanner_filter[i].empty = true;
    }
    _config.scanner_filter_empty = true;
#if JELLING_DUPLICATE_DETECTION_ENABLE
    _config.duplicate_detection_enable = JELLING_DUPLICATE_DETECTION_ACTIVATION_DFTL;
#endif
}

jelling_config_t *jelling_get_config(void) {
    return &_config;
}

void jelling_print_config(void) {
    printf("--- Jelling configuration  ---\n");

    if (_config.advertiser_enable) {
        printf("Advertiser: enabled\n");
    } else { printf("Advertiser: disabled\n"); }
    if (_config.advertiser_verbose) {
        printf("Advertiser: verbose\n");
    } else { printf("Advertiser: not verbose\n"); }
    if (_config.advertiser_block_icmp) {
        printf("Advertiser: ICMP packets blocked\n");
    } else { printf("Advertiser: ICMP packets not blocked\n"); }


    if (_config.scanner_enable) {
        printf("Scanner: enabled\n");
    } else { printf("Scanner: disabled\n"); }
    if (_config.scanner_verbose) {
        printf("Scanner: verbose\n");
    } else { printf("Scanner: not verbose\n"); }
    bool empty = true;
    for (int i=0; i < JELLING_SCANNER_FILTER_SIZE; i++) {
        if (!_config.scanner_filter[i].empty) {
            printf("Filter: ");
            bluetil_addr_print(_config.scanner_filter[i].addr);
            puts("");
            empty = false;
        }
    }
    if (empty) {
        printf("Scanner: no address in filter\n");
    }
#if JELLING_DUPLICATE_DETECTION_ENABLE
    if (_config.duplicate_detection_enable) {
        printf("Duplicate detection: enabled\n");
    } else { printf("Duplicate etection: disabled\n"); }
#endif
}
