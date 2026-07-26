#include "bluetooth.h"

#include <string.h>

static void bluetooth_record_relay(bluetooth_runtime_t *runtime, uint8_t source_peer,
                                   uint8_t target_peer, const uint8_t *payload,
                                   size_t payload_len) {
    if (runtime == NULL) {
        return;
    }

    runtime->relay_invocations++;
    runtime->last_relay_source_peer = source_peer;
    runtime->last_relay_target = target_peer;

    if (runtime->relay_target_count < INTERCOM_MAX_PEERS) {
        runtime->relay_targets[runtime->relay_target_count++] = target_peer;
    }

    runtime->last_relay_payload_len = payload_len;
    if (runtime->last_relay_payload_len > BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN) {
        runtime->last_relay_payload_len = BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN;
    }

    if (runtime->last_relay_payload_len > 0U && payload != NULL) {
        memcpy(runtime->last_relay_payload, payload, runtime->last_relay_payload_len);
    } else {
        memset(runtime->last_relay_payload, 0, sizeof(runtime->last_relay_payload));
    }
}

static size_t bluetooth_find_peer_index(const bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (runtime == NULL) {
        return 0U;
    }

    for (size_t index = 0; index < runtime->connected_peer_count; ++index) {
        if (runtime->connected_peers[index] == peer_id) {
            return index;
        }
    }

    return runtime->connected_peer_count;
}

static bool bluetooth_has_peer(const bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_find_peer_index(runtime, peer_id) < (runtime != NULL ? runtime->connected_peer_count : 0U);
}

static void bluetooth_relay(void *context, uint8_t source_peer, uint8_t target_peer,
                            const uint8_t *payload, size_t payload_len) {
    bluetooth_record_relay((bluetooth_runtime_t *)context, source_peer, target_peer, payload,
                           payload_len);
}

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom) {
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->intercom = intercom;
    runtime->initialized = true;
}

bool bluetooth_connect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (runtime == NULL || !runtime->initialized) {
        return false;
    }

    if (bluetooth_has_peer(runtime, peer_id)) {
        return true;
    }

    if (runtime->connected_peer_count >= INTERCOM_MAX_PEERS) {
        return false;
    }

    if (runtime->intercom != NULL && !intercom_add_peer(runtime->intercom, peer_id)) {
        return false;
    }

    runtime->connected_peers[runtime->connected_peer_count++] = peer_id;
    return true;
}

bool bluetooth_disconnect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (runtime == NULL || !runtime->initialized) {
        return false;
    }

    size_t peer_index = bluetooth_find_peer_index(runtime, peer_id);
    bool removed = peer_index < runtime->connected_peer_count;
    if (removed) {
        for (size_t shift = peer_index + 1; shift < runtime->connected_peer_count; ++shift) {
            runtime->connected_peers[shift - 1] = runtime->connected_peers[shift];
        }
        runtime->connected_peer_count--;
    }

    if (runtime->intercom != NULL) {
        intercom_remove_peer(runtime->intercom, peer_id);
    }

    return removed;
}

bool bluetooth_is_peer_connected(const bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_has_peer(runtime, peer_id);
}

void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len) {
    if (runtime == NULL || !runtime->initialized || runtime->intercom == NULL) {
        return;
    }

    /* Fresh audio can still be relayed even if the runtime cannot register the source
     * peer because the peer limit has already been reached; the relay loop still uses the
     * existing intercom peers. */
    (void)bluetooth_connect_peer(runtime, source_peer);

    runtime->packets_received++;
    runtime->last_source_peer = source_peer;
    runtime->last_payload_len = payload_len;
    runtime->relay_target_count = 0U;
    runtime->last_relay_source_peer = source_peer;
    runtime->last_relay_target = 0U;
    runtime->last_relay_payload_len = 0U;
    memset(runtime->last_relay_payload, 0, sizeof(runtime->last_relay_payload));

    runtime->last_relay_count = intercom_rebroadcast(
        runtime->intercom, source_peer, payload, payload_len, bluetooth_relay, runtime);
}
