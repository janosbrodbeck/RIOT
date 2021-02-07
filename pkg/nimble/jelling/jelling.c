#include <limits.h>
#include <errno.h>

#include "assert.h"
#include "thread.h"
#include "thread_flags.h"
#include "mutex.h"
#include "xtimer.h"

#include "net/ble.h"
#include "net/bluetil/addr.h"
#include "net/gnrc/netif.h"
#include "net/gnrc/netif/hdr.h"

#include "net/gnrc/netreg.h"
#include "net/gnrc/pktbuf.h"
#include "net/gnrc/nettype.h"

#include "jelling_netif.h"
#include "jelling.h"
#include "jelling_fragmentation.h"
#include "jelling_duplicate_detection.h"

#include "nimble_riot.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

#define ADV_INSTANCES           (MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES+1)

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

typedef struct {
    bool ongoing;
    uint8_t pkt_num;
    uint8_t next_hop_match;
    uint8_t data[JELLING_HDR_RESERVED];
    size_t len;
} _chained_packet;

static int _send_pkt(uint8_t instance, struct os_mbuf *mbuf);
static int _start_scanner(void);
static int _gap_event(struct ble_gap_event *event, void *arg);
static void _on_data(struct ble_gap_event *event, void *arg);
static int _configure_adv_instance(uint8_t instance);
static bool _filter_manufacturer_id(uint8_t *data, uint8_t len);
static uint8_t _filter_next_hop_addr(uint8_t *data, uint8_t len);
static size_t _prepare_ipv6_packet(uint8_t *data, size_t len);

static mutex_t _instance_status_lock;
static _adv_instance_status_t _instance_status[ADV_INSTANCES];
static jelling_status_t _jelling_status;
static jelling_config_t _config;

static gnrc_netif_t *_netif;
static gnrc_nettype_t _nettype;

static _chained_packet _chain;
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
                if (_config.advertiser_verbose) {
                    printf("Skipped ICMP packet\n");
                }
                return 0;
            }
            tmp = tmp->next;
        }
    }

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
        printf("Info: could not find idle advertising instance\n");
        return -EBUSY;
    }

    /* allocate mbuf */
    struct os_mbuf *buf = os_msys_get_pkthdr(JELLING_HDR_RESERVED, 0);
    if (buf == NULL) {
        return -ENOBUFS;
    }

    /* get netif_hdr */
    gnrc_netif_hdr_t *hdr = (gnrc_netif_hdr_t *)pkt->data;
    int res;
    if (hdr->flags & GNRC_NETIF_HDR_FLAGS_MULTICAST) {
        if (IS_ACTIVE(JELLING_SKIP_MULTICAST_PACKETS)) {
            goto exit;
        }
        res = jelling_fragment_into_mbuf(pkt->next, buf, _ble_mc_addr, _pkt_next_num);
    } else { /* unicast */
        uint8_t *dst_addr = gnrc_netif_hdr_get_dst_addr(hdr);
        res = jelling_fragment_into_mbuf(pkt->next, buf, dst_addr, _pkt_next_num);
    }
    _pkt_next_num++;

    if (res < 0) {
        printf("jelling_send: fragmentation failed. Return code: %02X\n", res);
        goto exit;
    }

    if(IS_ACTIVE(JELLING_DEBUG_IPV6_PACKET_SIZES)) {
        printf("Sending IPv6 packet of %d bytes\n", gnrc_pkt_len(pkt)-pkt->size);
    }

    /* note: ble_gap_ext_adv_set_data already calls os_mbuf_free_chain! */
    res = _send_pkt(instance, buf);
    return res;

exit:
    os_mbuf_free_chain(buf);
    return res;
}

static int _send_pkt(uint8_t instance, struct os_mbuf *mbuf)
{
    int res = ble_gap_ext_adv_set_data(instance, mbuf);
    if (res) {
        return res;
    }

    res = ble_gap_ext_adv_start(instance, _config.advertiser_duration,
                            _config.advertiser_max_events);
    if (res) {
        printf("Couldn't start advertising. Return code: 0x%02X\n", res);
        return -1;
    }

    if(IS_ACTIVE(JELLING_DEBUG_ADVERTISING_PROCESS)) {
        printf("DEBUG: Advertising for packet started successfully\n");
    }
    return res;
}

static int _start_scanner(void) {
    struct ble_gap_ext_disc_params uncoded = {0};
    uint8_t limited = _config.scanner_limited;
    uint8_t filter_duplicates = _config.scanner_filter_duplicates;
    uint16_t duration = _config.scanner_duration;
    uint16_t period = _config.scanner_period;

    /* Set uncoded parameters */
    uncoded.passive = 1;
    uncoded.itvl = _config.scanner_itvl;
    uncoded.window = _config.scanner_window;

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
            return 0;

    }
    return 0;
}

/* magic - avoid touching */
static size_t _prepare_ipv6_packet(uint8_t *data, size_t len)
{
    size_t len_data_type;
    uint16_t pos_ipv6 = 0;
    uint16_t pos = 0;

    bool first = true;
    while (pos < len) {
        len_data_type = data[pos];
        /* sanity check */
        if (len_data_type+pos > len || len_data_type+pos_ipv6 > len) {
            goto error;
        }
        if (data[pos+1] != 0xFF) {
            goto error;
        }

        if (first) {
            /* skip jelling header */
            memcpy(data+pos_ipv6, data+pos+PACKET_DATA_OFFSET, len_data_type-PACKET_DATA_OFFSET+1);
            pos_ipv6 += len_data_type-PACKET_DATA_OFFSET+1;
            first = false;
        } else {
            memcpy(data+pos_ipv6, data+pos+PACKET_NEXT_HOP_OFFSET+1, len_data_type-PACKET_NEXT_HOP_OFFSET);
            pos_ipv6 += len_data_type-PACKET_NEXT_HOP_OFFSET;
        }
        pos += 1 + len_data_type; /* len field is not included in len_data_type */
    }

    /* make pos to len */
    pos_ipv6++;

    return pos_ipv6;

error:
    if (IS_ACTIVE(JELLING_DEBUG_BROKEN_BLE_DATA)) {
        printf("Broken DATA: \n");
        for (int i=0; i < len; i++) {
            printf("%02X ", data[i]);
            if (i % 30 == 0 && i != 0 && i != 1) {
                puts("");
            }
        }
        printf("\n========================\n");
    }
    return -1;
}

static void _on_data(struct ble_gap_event *event, void *arg)
{
    /* TRUNCATED status -> recv of chained packet failed */
    if (event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_TRUNCATED) {
        _chain.ongoing = false;
        return;
    }

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

    if (!_chain.ongoing) {
        bool jelling_packet = _filter_manufacturer_id((uint8_t *)event->ext_disc.data,
                            event->ext_disc.length_data);

        /* do not process any further if unknown packet */
        if (!jelling_packet) {
            return;
        }
    }

    /* print info */
    if (_config.scanner_verbose) {
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
    }

    /* No chained packets ongoing -> needs to be the first packet containing
       the jelling header */
    if (!_chain.ongoing) {
       _chain.next_hop_match = _filter_next_hop_addr((uint8_t *)event->ext_disc.data+PACKET_NEXT_HOP_OFFSET,
            event->ext_disc.length_data-PACKET_NEXT_HOP_OFFSET);
        if(!_chain.next_hop_match) {
            return;
        }

        /* duplicate detection */
        _chain.pkt_num = event->ext_disc.data[PACKET_PKG_NUM_OFFSET];
        if (IS_ACTIVE(JELLING_DUPLICATE_DETECTION_FEATURE_ENABLE)) {
            if (_config.duplicate_detection_enable) {
                if (jelling_dd_check_for_entry(event->ext_disc.addr.val, _chain.pkt_num)) {
                    return;
                }
            }
        }

        /* incomplete event, prepare for more data  */
        if (event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE) {
            _chain.ongoing = true;
        }
        /* copy data into buffer */
        memcpy(_chain.data, event->ext_disc.data, event->ext_disc.length_data);
        _chain.len = event->ext_disc.length_data;

    } else { /* subsequent packet without jelling header */
        /* sanity check */
        if (_chain.len+event->ext_disc.length_data > sizeof(_chain.data)) {
            printf("Broken packets from nimBLE\n");
            _chain.ongoing = false;
            return;
        }
        /* copy data into buffer */
        memcpy(_chain.data+_chain.len, event->ext_disc.data,
               event->ext_disc.length_data);
        _chain.len += event->ext_disc.length_data;

        if (event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) {
            _chain.ongoing = false;
        }
    }

    if (_chain.ongoing) {
        return;
    }

    /* Process BLE data */
    size_t ipv6_packet_size = _prepare_ipv6_packet(_chain.data, _chain.len);
    if (ipv6_packet_size == -1) {
        if (IS_ACTIVE(JELLING_DEBUG_BROKEN_BLE_DATA_MSG)) {
            printf("Broken BLE data\n");
        }
        return;
    }

    /* add to duplicate detection */
    if (IS_ACTIVE(JELLING_DUPLICATE_DETECTION_FEATURE_ENABLE)) {
        if (_config.duplicate_detection_enable) {
            jelling_dd_add(event->ext_disc.addr.val, _chain.pkt_num);
        }
    }

    if(IS_ACTIVE(JELLING_DEBUG_IPV6_PACKET_SIZES)) {
        printf("Received IPv6 packet of %d bytes\n", ipv6_packet_size);
    }

    /* prepare gnrc pkt */
    gnrc_pktsnip_t *if_snip;
    /* destination can be multicast or unicast addr */
    if (_chain.next_hop_match == ADDR_UNICAST) {
        if_snip = gnrc_netif_hdr_build(event->ext_disc.addr.val, BLE_ADDR_LEN,
                        _ble_addr, BLE_ADDR_LEN);
    } else {
        if_snip = gnrc_netif_hdr_build(event->ext_disc.addr.val, BLE_ADDR_LEN,
                        _ble_mc_addr, BLE_ADDR_LEN);
    }

    /* we need to add the device PID to the netif header */
    gnrc_netif_hdr_set_netif(if_snip->data, _netif);

    /* allocate space in the pktbuf to store the packet */
    gnrc_pktsnip_t *payload = gnrc_pktbuf_add(if_snip, NULL, ipv6_packet_size,
                                _nettype);
    if (payload == NULL) {
        gnrc_pktbuf_release(if_snip);
        printf("Payload allocation failed\n");
        return;
    }

    /* copy payload from event into pktbuffer */
    memcpy(payload->data, _chain.data, ipv6_packet_size);

    /* finally dispatch the receive packet to gnrc */
    if (!gnrc_netapi_dispatch_receive(payload->type, GNRC_NETREG_DEMUX_CTX_ALL,
                                    payload)) {
        printf("Could not dispatch\n");
        gnrc_pktbuf_release(payload);
    }
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
    params.itvl_min = _config.advertiser_itvl_min;
    params.itvl_max = _config.advertiser_itvl_max;
    params.tx_power = 127;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    //params.sid = 0;

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

    if(_config.scanner_enable && ble_gap_disc_active() == 0) {
        _start_scanner();
    }
    _jelling_status = JELLING_RUNNING;
}

void jelling_stop(void)
{
    int rc;
    mutex_lock(&_instance_status_lock);
    bool wait = false;
    for (int i=0; i < ADV_INSTANCES; i++) {
        _instance_status[i] = STOPPED;
        rc = ble_gap_ext_adv_stop(i);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            wait = true;
        }
    }
    if (wait) {
        printf("At least one advertising instace is busy. Sleeping three seconds...\n");
        xtimer_sleep(3);
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
    if (IS_ACTIVE(JELLING_DUPLICATE_DETECTION_FEATURE_ENABLE)) {
        printf("Init\n");
        jelling_dd_init();
    }

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
    if (IS_ACTIVE(MODULE_GNRC_IPV6)) {
        printf(" -> ");
        bluetil_addr_ipv6_l2ll_print(_ble_addr);
    }
    puts("");

    printf("Own Address (live): ");
    bluetil_addr_print(own_addr);
    puts("");

    printf("Advertising instances: %d\n", ADV_INSTANCES);
    printf("Non-standard 6LoWPAN MTU allowed: %d\n", CONFIG_GNRC_NETIF_NONSTANDARD_6LO_MTU);
    printf("IPv6 MTU: %d bytes\n", JELLING_IPV6_MTU);
    printf("BLE maximum PDU: %d bytes\n", MYNEWT_VAL_BLE_EXT_ADV_MAX_SIZE);
    printf("MSYS_BLOCK_SIZE: %d\n", MYNEWT_VAL(MSYS_1_BLOCK_SIZE));
    printf("MSYS_BLOCK_COUNT: %d\n", MYNEWT_VAL(MSYS_1_BLOCK_COUNT));
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
    _config.advertiser_enable = JELLING_ADVERTISING_ENABLE_DFLT;
    _config.advertiser_verbose = JELLING_ADVERTISING_VERBOSE_DFLT;
    _config.advertiser_block_icmp = JELLING_ADVERTISING_BLOCK_ICMP_DFLT;
    _config.advertiser_duration = JELLING_ADVERTISING_DURATION_DFLT;
    _config.advertiser_max_events = JELLING_ADVERTISING_MAX_EVENTS_DFLT;
    _config.advertiser_itvl_min = JELLING_ADVERTISING_ITVL_MIN_DFLT;
    _config.advertiser_itvl_max = JELLING_ADVERTISING_ITVL_MAX_DFLT;

    _config.scanner_enable = JELLING_SCANNER_ENABLE_DFLT;
    _config.scanner_verbose = JELLING_SCANNER_VERBOSE_DFLT;
    _config.scanner_itvl = JELLING_SCANNER_ITVL_DFLT;
    _config.scanner_window = JELLING_SCANNER_WINDOW_DFLT;
    _config.scanner_period = JELLING_SCANNER_PERIOD_DFLT;
    _config.scanner_duration = JELLING_SCANNER_DURATION_DFLT;
    _config.scanner_filter_duplicates = JELLING_SCANNER_FILTER_DUPLICATES_DFLT;

    memcpy(_config.scanner_filter, 0, sizeof(_config.scanner_filter));
    for (int i=0; i < JELLING_SCANNER_FILTER_SIZE; i++) {
        _config.scanner_filter[i].empty = true;
    }
    _config.scanner_filter_empty = true;
    if (IS_ACTIVE(JELLING_DUPLICATE_DETECTION_FEATURE_ENABLE)) {
        _config.duplicate_detection_enable = JELLING_DUPLICATE_DETECTION_ACTIVATION_DFTL;
    }
}

jelling_config_t *jelling_get_config(void) {
    return &_config;
}

void jelling_restart_advertiser(void) {
    jelling_stop();
    /* reconfigure instances */
    int res;
    for (int i = 0; i < ADV_INSTANCES; i++) {
        res = _configure_adv_instance(i);
        if (res < 0) {
            _jelling_status = JELLING_INIT_ERROR;
            break;
        }
        _instance_status[i] = STOPPED;
    }
    jelling_start();
}

void jelling_restart_scanner(void) {
    ble_gap_disc_cancel();
    if (_config.scanner_enable) {
        _start_scanner();
    }
}

void jelling_print_config(void) {
    printf("--- Jelling configuration  ---\n");

    printf("Advertiser:\n");
    if (_config.advertiser_enable) {
        printf("    Enabled: true\n");
    } else { printf("    Enabled: false\n"); }
    if (_config.advertiser_verbose) {
        printf("    Verbose: true\n");
    } else { printf("    Verbose: false\n"); }
    if (_config.advertiser_block_icmp) {
        printf("    ICMP packets blocked: true\n");
    } else { printf("    ICMP packets blocked: false\n"); }
    printf("    Max events: %d \n", _config.advertiser_max_events);
    printf("    Duration: %d (Unit: 10ms)\n", _config.advertiser_duration);
    printf("    Itvl min: %ld (Unit: 0.625ms)\n", _config.advertiser_itvl_min);
    printf("    Itvl max: %ld (Unit: 0.625ms)\n", _config.advertiser_itvl_max);

    printf("Scanner:\n");
    if (_config.scanner_enable) {
        printf("    Enabled: true\n");
    } else { printf("    Enabled: false\n"); }
    if (IS_ACTIVE(JELLING_DUPLICATE_DETECTION_FEATURE_ENABLE)) {
        if (_config.duplicate_detection_enable) {
            printf("    Duplicate detection: enabled\n");
        } else { printf("   Duplicate etection: disabled\n"); }
    }
    if (_config.scanner_verbose) {
        printf("    Verbose: true\n");
    } else { printf("    Verbose: false\n"); }
    printf("    Duration: %d (Unit 10ms)\n", _config.scanner_duration);
    printf("    Period: %d (Unit 1.28s)\n", _config.scanner_period);
    printf("    Itvl: %ld (Unit 0.625ms)\n", _config.scanner_itvl);
    printf("    Window: %ld (Unit 0.625ms)\n", _config.scanner_window);

    bool empty = true;
    printf("    Filter: ");
    for (int i=0; i < JELLING_SCANNER_FILTER_SIZE; i++) {
        if (!_config.scanner_filter[i].empty) {
            printf("\n");
            printf("        Node: ");
            bluetil_addr_print(_config.scanner_filter[i].addr);
            empty = false;
        }
    }
    if (empty) {
        printf("no address in filter\n");
    } else { puts(""); }
}
