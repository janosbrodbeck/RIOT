#include "jelling_fragmentation.h"

#define ENABLE_DEBUG                0
#include "debug.h"

#if ENABLE_DEBUG
static void _print_data(uint8_t *data, uint8_t len) {
    /*printf("DEBUG: Fragment: ");
    for (int i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    } */
    printf("DEBUG: Fragment size: %d (+1 byte length field)\n", len);
}
#endif

size_t _write_hdr(uint8_t *data, uint8_t *next_hop, uint8_t pkt_num, bool write_next_hop);

int jelling_fragment_into_mbuf(gnrc_pktsnip_t *pkt, struct os_mbuf *mbuf,
                                uint8_t *next_hop, uint8_t pkt_num) {
    uint8_t data[JELLING_FRAGMENTATION_FRAGMENT_BUF_SIZE];
    size_t len = 0;
    uint8_t max_len = JELLING_FIRST_FRAGMENT_SIZE;
    int res;

    DEBUG("DEBUG: About to fragment %d bytes\n", gnrc_pkt_len(pkt));
    len = _write_hdr(data, next_hop, pkt_num, true);

    while (pkt) {
        size_t pkt_size = pkt->size;
        size_t pkt_written = 0;
        while (pkt_written != pkt_size) {
            /* do we fit into the current fragment? */
            if(pkt_size - pkt_written <= max_len-len) {
                memcpy(data+len, pkt->data+pkt_written, pkt_size-pkt_written);
                len += pkt_size-pkt_written;
                pkt_written += pkt_size-pkt_written;
            } else { /* we don't fit */
                memcpy(data+len, pkt->data+pkt_written, max_len-len);
                pkt_written += max_len-len;
                len = max_len;
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
                if (pkt_written != pkt_size ||
                        (pkt->next != NULL && pkt->next->size != 0)) {
                    pkt_num++;
                    len = _write_hdr(data, next_hop, pkt_num, true);
                    max_len = JELLING_SUBSEQUENT_FRAGMENT_SIZE;
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
#endif
    }
#if ENABLE_DEBUG
    printf("DEBUG: Fragmentation completed\n");
#endif
    return res < 0 ? -1 : res;
}

size_t _write_hdr(uint8_t *data, uint8_t *next_hop, uint8_t pkt_num, bool write_next_hop) {
    uint8_t len = 0;
    memset(data+len, BLE_GAP_AD_VENDOR, 1);
    len++;
    memset(data+len, VENDOR_ID_1, 1);
    len++;
    memset(data+len, VENDOR_ID_2, 1);
    len++;
    if (write_next_hop) {
        memcpy(data+len, next_hop, BLE_ADDR_LEN);
        len += BLE_ADDR_LEN;
    }
    memset(data+len, pkt_num, 1);
    len++;
    return len;
}


