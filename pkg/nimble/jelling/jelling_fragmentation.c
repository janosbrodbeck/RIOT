#include "jelling_fragmentation.h"

#define ENABLE_DEBUG                0
#include "debug.h"

#if ENABLE_DEBUG
static void _print_data(uint8_t *data, uint8_t len) {
    printf("DEBUG: fragmentation: ");
    for (int i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    puts("");
}
#endif

size_t _write_hdr(uint8_t *data, uint8_t *next_hop, uint8_t pkt_num, bool first);

int jelling_fragment_into_mbuf(gnrc_pktsnip_t *pkt, struct os_mbuf *mbuf,
                                uint8_t *next_hop, uint8_t pkt_num) {
    uint8_t data[JELLING_FRAGMENTATION_FRAGMENT_SIZE];
    uint8_t len = 0;
    uint8_t max_len = JELLING_FRAGMENTATION_FIRST_FRAGMENT_SIZE;
    int res;

    DEBUG("DEBUG: about to send %d bytes\n", gnrc_pkt_len(pkt));
    len = _write_hdr(data, next_hop, pkt_num, true);

    while (pkt) {
        uint8_t pkt_size = pkt->size;
        uint8_t pkt_written = 0;
        while (pkt_written != pkt_size) {
            /* do we fit into the current fragment? */
            if(pkt_size - pkt_written <= max_len-len) {
                memcpy(data+len, pkt->data+pkt_written, pkt_size-pkt_written);
                len += pkt_size-pkt_written;
                pkt_written += pkt_size-pkt_written;
            } else { /* we don't fit */
                memcpy(data+len, pkt->data+pkt_written, max_len-len);
                len += max_len-len;
                pkt_written += max_len-len;
            }

            /* fragment is full */
            if (len == max_len) {
                res = os_mbuf_append(mbuf, &len, 1);
                if (res != 0 ) { return res; }
                res = os_mbuf_append(mbuf, data, len);
                if (res != 0 ) { return res; }
#if ENABLE_DEBUG
                _print_data(data, len);
#endif

                /* reset */
                len = 0;
                /* there is more data */
                if (pkt_written != pkt_size || pkt->next != NULL) {
                    len = _write_hdr(data, next_hop, pkt_num, false);
                }
            }
        }
        pkt = pkt->next;
    }

    /* started fragment left */
    if (len != 0) {
        res = os_mbuf_append(mbuf, &len, 1);
        if (res != 0 ) { return res; }
        res = os_mbuf_append(mbuf, data, len);
#if ENABLE_DEBUG
        _print_data(data, len);
        DEBUG("DEBUG: wrote: %d bytes\n", len)
#endif
    }
    return res;
}

size_t _write_hdr(uint8_t *data, uint8_t *next_hop, uint8_t pkt_num, bool first) {
    uint8_t len = 0;
    memset(data+len, BLE_GAP_AD_VENDOR, 1);
    len++;
    memset(data+len, VENDOR_ID_1, 1);
    len++;
    memset(data+len, VENDOR_ID_2, 1);
    len++;
    if (first) {
        memcpy(data+len, next_hop, BLE_ADDR_LEN);
        len += BLE_ADDR_LEN;
    }
    memset(data+len, pkt_num, 1);
    len++;
    return len;
}


