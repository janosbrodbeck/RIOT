/**
 * @defgroup    net_dsm DTLS Session Management (DSM)
 * @ingroup     net net_dtls
 * @brief       This module provides functionality to store and retrieve session
 *              information of DTLS connections.
 *
 * @{\
 *
 * @file
 * @brief       DTLS session management module definition
 *
 * @author      János Brodbeck <janos.brodbeck@ml-pa.com>
 */

#ifndef NET_DSM_H
#define NET_DSM_H

#include <stdint.h>
#include "net/sock/dtls.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Maximum number of maintained DTLS sessions (tinyDTLS)
 */
#ifndef DTLS_PEER_MAX
#define DTLS_PEER_MAX   (1)
#endif

/**
 * @brief Session management states
 */
typedef enum {
    NO_SPACE = -1,
    NO_SESSION = 0,
    HANDSHAKE_ONGOING,
    SESSION_ESTABLISHED
} dsm_state_t;

/**
 * @brief   Initialize the DTLS session management
 *
 * Must call once before first use.
 */
void dsm_init(void);

/**
 * @brief   Stores a session
 *
 * Stores a given session in the internal storage of the session management.
 * If the session is already stored only the state will be updated when the session
 * gets established.
 *
 * @param[in]   sock        @ref sock_dtls_t, which the session is created on
 * @param[in]   session     Session to store
 * @param[in]   new_state   New state of the session
 * @param[in]   restore     Indicates, whether the session object should be restored
 *                          when an already established session is found
 *
 * @return Previous state of the session. If no session existed before it returns
 *         NO_SESSION. If no space is available it returns NO_SPACE.
 */
dsm_state_t dsm_store(sock_dtls_t *sock, sock_dtls_session_t *session,
                      dsm_state_t new_state, bool restore);

/**
 * @brief   Removes a session
 *
 * Removes a given session in the internal storage of the session management.
 *
 * @param[in]  sock         @ref sock_dtls_t, which the session is created on
 * @param[in]  session      Session to store
 */
void dsm_remove(sock_dtls_t *sock, sock_dtls_session_t *session);

/**
 * @brief   Returns the maximum number of available sessions slots
 *
 * @return  Number of maximum available sessio slots.
 */
uint8_t dsm_get_num_maximum_slots(void);

/**
 * @brief   Returns the number of currently available session slots
 *
 * @return  Number of available session slots in the session management.
 */
uint8_t dsm_get_num_available_slots(void);

/**
 * @brief   Returns the oldest used session
 *
 * @param[in]   sock        @ref sock_dtls_t, which the session is created on
 * @param[in]   session     Empty session
 *
 * @return   1, on success
 * @return   -1, when no session is stored
 */
ssize_t dsm_get_oldest_used_session(sock_dtls_t *sock, sock_dtls_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* NET_DSM_H */
/** @} */
