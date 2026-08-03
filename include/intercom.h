#ifndef INTERCOM_H
#define INTERCOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INTERCOM_MAX_PEERS 8U

typedef struct {
    uint8_t peers[INTERCOM_MAX_PEERS];
    size_t peer_count;
    bool ptt_pressed;
    bool enabled;
} intercom_state_t;

typedef void (*intercom_relay_fn)(void *context, uint8_t source_peer,
                                  uint8_t target_peer, const uint8_t *payload,
                                  size_t payload_len);

void intercom_init(intercom_state_t *state);
bool intercom_enable(intercom_state_t *state, bool enabled);
bool intercom_add_peer(intercom_state_t *state, uint8_t peer_id);
bool intercom_remove_peer(intercom_state_t *state, uint8_t peer_id);
void intercom_set_ptt(intercom_state_t *state, bool pressed);
bool intercom_toggle_ptt(intercom_state_t *state);
size_t intercom_rebroadcast(intercom_state_t *state, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len,
                           intercom_relay_fn relay, void *context);

#ifdef __cplusplus
}
#endif

#endif
