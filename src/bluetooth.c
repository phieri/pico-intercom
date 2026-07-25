#include "bluetooth.h"

#include <string.h>

static bool bluetooth_has_peer(const bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (runtime == NULL) {
        return false;
    }

    for (size_t index = 0; index < runtime->connected_peer_count; ++index) {
        if (runtime->connected_peers[index] == peer_id) {
            return true;
        }
    }

    return false;
}

static void bluetooth_relay(void *context, uint8_t source_peer, uint8_t target_peer,
                            const uint8_t *payload, size_t payload_len) {
    bluetooth_runtime_t *runtime = (bluetooth_runtime_t *)context;
    (void)source_peer;
    (void)target_peer;
    (void)payload;
    (void)payload_len;

    if (runtime != NULL) {
        runtime->relay_invocations++;
    }
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

    bool removed = false;
    for (size_t index = 0; index < runtime->connected_peer_count; ++index) {
        if (runtime->connected_peers[index] != peer_id) {
            continue;
        }

        for (size_t shift = index + 1; shift < runtime->connected_peer_count; ++shift) {
            runtime->connected_peers[shift - 1] = runtime->connected_peers[shift];
        }
        runtime->connected_peer_count--;
        removed = true;
        break;
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

    bluetooth_connect_peer(runtime, source_peer);

    runtime->packets_received++;
    runtime->last_source_peer = source_peer;
    runtime->last_payload_len = payload_len;

    runtime->last_relay_count = intercom_rebroadcast(
        runtime->intercom, source_peer, payload, payload_len, bluetooth_relay, runtime);
}
