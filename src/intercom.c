#include "intercom.h"

#include <string.h>

void intercom_init(intercom_state_t *state) {
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->enabled = true;
}

bool intercom_enable(intercom_state_t *state, bool enabled) {
    if (state == NULL) {
        return false;
    }

    state->enabled = enabled;
    return true;
}

bool intercom_add_peer(intercom_state_t *state, uint8_t peer_id) {
    if (state == NULL || state->peer_count >= INTERCOM_MAX_PEERS) {
        return false;
    }

    for (size_t index = 0; index < state->peer_count; ++index) {
        if (state->peers[index] == peer_id) {
            return true;
        }
    }

    state->peers[state->peer_count++] = peer_id;
    return true;
}

bool intercom_remove_peer(intercom_state_t *state, uint8_t peer_id) {
    if (state == NULL) {
        return false;
    }

    for (size_t index = 0; index < state->peer_count; ++index) {
        if (state->peers[index] == peer_id) {
            for (size_t shift = index + 1; shift < state->peer_count; ++shift) {
                state->peers[shift - 1] = state->peers[shift];
            }
            state->peer_count--;
            return true;
        }
    }

    return false;
}

void intercom_set_ptt(intercom_state_t *state, bool pressed) {
    if (state != NULL) {
        state->ptt_pressed = pressed;
    }
}

size_t intercom_rebroadcast(intercom_state_t *state, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len,
                           intercom_relay_fn relay, void *context) {
    if (state == NULL || !state->enabled || !state->ptt_pressed || payload == NULL ||
        payload_len == 0U || relay == NULL) {
        return 0U;
    }

    size_t relayed = 0U;
    for (size_t index = 0; index < state->peer_count; ++index) {
        uint8_t target_peer = state->peers[index];
        if (target_peer == source_peer) {
            continue;
        }

        relay(context, source_peer, target_peer, payload, payload_len);
        relayed++;
    }

    return relayed;
}
