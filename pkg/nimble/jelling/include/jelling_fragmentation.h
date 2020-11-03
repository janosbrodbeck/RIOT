#ifndef JELLING_FRAGMENTATION_H
#define JELLING_FRAGMENTATION_H

#include "jelling.h"
#include "net/gnrc/pkt.h"
#include "nimble_riot.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

#ifndef JELLING_FRAGMENTATION_FRAGMENT_BUF_SIZE
#define JELLING_FRAGMENTATION_FRAGMENT_BUF_SIZE         255
#endif

#ifndef JELLING_FIRST_FRAGMENT_SIZE
#define JELLING_FIRST_FRAGMENT_SIZE                     228
#endif

#ifndef JELLING_SUBSEQUENT_FRAGMENT_SIZE
#define JELLING_SUBSEQUENT_FRAGMENT_SIZE                228
#endif

int jelling_fragment_into_mbuf(gnrc_pktsnip_t *pkt, struct os_mbuf *mbuf,
                                uint8_t *next_hop, uint8_t pkt_num);

#endif /* JELLING_FRAGMENTATION_H */