#include "bluetooth_transport.h"

#include <stdio.h>
#include <string.h>

enum {
    BLUETOOTH_TRANSPORT_ERROR_NONE = 0U,
    BLUETOOTH_TRANSPORT_ERROR_PEER_LIMIT = 2U,
    BLUETOOTH_TRANSPORT_ERROR_NOT_CONNECTED = 3U,
    BLUETOOTH_TRANSPORT_ERROR_QUEUE_FULL = 4U,
    BLUETOOTH_TRANSPORT_ERROR_PEER_UNDISCOVERED = 5U,
    BLUETOOTH_TRANSPORT_ERROR_AUDIO_UNAVAILABLE = 6U,
};

static size_t bluetooth_transport_find_connected_index(const bluetooth_transport_t *transport,
                                                       uint8_t peer_id) {
    if (transport == NULL) {
        return 0U;
    }

    for (size_t index = 0; index < transport->connected_peer_count; ++index) {
        if (transport->connected_peers[index] == peer_id) {
            return index;
        }
    }

    return transport->connected_peer_count;
}

static bool bluetooth_transport_has_peer(const bluetooth_transport_t *transport, uint8_t peer_id) {
    return bluetooth_transport_find_connected_index(transport, peer_id) <
           (transport != NULL ? transport->connected_peer_count : 0U);
}

static size_t bluetooth_transport_find_discovered_index(const bluetooth_transport_t *transport,
                                                        uint8_t peer_id) {
    if (transport == NULL) {
        return INTERCOM_MAX_PEERS;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        if (transport->discovered_peers[index].valid &&
            transport->discovered_peers[index].peer_id == peer_id) {
            return index;
        }
    }

    return INTERCOM_MAX_PEERS;
}

static bluetooth_transport_peer_info_t *
bluetooth_transport_get_discovered_peer(bluetooth_transport_t *transport, uint8_t peer_id,
                                        bool create) {
    if (transport == NULL || peer_id == 0U || peer_id == transport->local_peer_id) {
        return NULL;
    }

    const size_t found = bluetooth_transport_find_discovered_index(transport, peer_id);
    if (found < INTERCOM_MAX_PEERS) {
        return &transport->discovered_peers[found];
    }

    if (!create) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &transport->discovered_peers[index];
        if (peer->valid) {
            continue;
        }

        memset(peer, 0, sizeof(*peer));
        peer->valid = true;
        peer->peer_id = peer_id;
        if (transport->discovered_peer_count < INTERCOM_MAX_PEERS) {
            transport->discovered_peer_count++;
        }
        return peer;
    }

    return NULL;
}

static bool bluetooth_transport_remember_peer(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || peer_id == 0U) {
        return false;
    }

    for (size_t index = 0; index < transport->remembered_peer_count; ++index) {
        if (transport->remembered_peers[index] == peer_id) {
            return true;
        }
    }

    if (transport->remembered_peer_count >= INTERCOM_MAX_PEERS) {
        transport->error = true;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_PEER_LIMIT;
        return false;
    }

    transport->remembered_peers[transport->remembered_peer_count++] = peer_id;
    return true;
}

static bool bluetooth_transport_peer_is_remembered(const bluetooth_transport_t *transport,
                                                   uint8_t peer_id) {
    if (transport == NULL || peer_id == 0U) {
        return false;
    }

    for (size_t index = 0; index < transport->remembered_peer_count; ++index) {
        if (transport->remembered_peers[index] == peer_id) {
            return true;
        }
    }

    return false;
}

static void bluetooth_transport_make_peer_name(char *buffer, size_t buffer_len, uint8_t peer_id) {
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }

    (void)snprintf(buffer, buffer_len, "headset-%u", (unsigned)peer_id);
}

static bool bluetooth_transport_peer_ready_for_audio(
    const bluetooth_transport_peer_info_t *peer) {
    return peer != NULL && peer->valid && peer->audio_ready;
}

static void bluetooth_transport_mark_connected(bluetooth_transport_t *transport,
                                               bluetooth_transport_peer_info_t *peer) {
    if (transport == NULL || peer == NULL) {
        return;
    }

    peer->paired = true;
    peer->pairing_pending = false;
    peer->last_connected_ms = transport->last_poll_ms;

    const size_t peer_index =
        bluetooth_transport_find_connected_index(transport, peer->peer_id);
    if (peer_index < transport->connected_peer_count) {
        transport->peer_states[peer_index] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
        transport->pending_pair_peer_id = 0U;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NONE;
        return;
    }

    if (transport->connected_peer_count >= INTERCOM_MAX_PEERS) {
        transport->error = true;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_PEER_LIMIT;
        return;
    }

    const size_t slot = transport->connected_peer_count++;
    transport->connected_peers[slot] = peer->peer_id;
    transport->peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
    transport->pending_pair_peer_id = 0U;
    transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NONE;
}

static void bluetooth_transport_purge_stale_peer(bluetooth_transport_t *transport, size_t index) {
    if (transport == NULL || index >= INTERCOM_MAX_PEERS) {
        return;
    }

    bluetooth_transport_peer_info_t *peer = &transport->discovered_peers[index];
    if (!peer->valid || bluetooth_transport_has_peer(transport, peer->peer_id) ||
        bluetooth_transport_peer_is_remembered(transport, peer->peer_id) ||
        peer->pairing_pending) {
        return;
    }

    memset(peer, 0, sizeof(*peer));
    if (transport->discovered_peer_count > 0U) {
        transport->discovered_peer_count--;
    }
}

static size_t bluetooth_transport_drop_peer_packets(bluetooth_transport_t *transport,
                                                    uint8_t peer_id) {
    if (transport == NULL || peer_id == 0U) {
        return 0U;
    }

    size_t removed = 0U;
    size_t index = 0U;
    while (index < transport->queued_packet_count) {
        const bluetooth_transport_packet_t *packet = &transport->queue[index];
        if (packet->source_peer != peer_id && packet->target_peer != peer_id) {
            index++;
            continue;
        }

        for (size_t shift = index + 1U; shift < transport->queued_packet_count; ++shift) {
            transport->queue[shift - 1U] = transport->queue[shift];
        }
        transport->queued_packet_count--;
        removed++;
    }

    transport->packets_dropped += removed;
    return removed;
}

void bluetooth_transport_init(bluetooth_transport_t *transport) {
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
    transport->initialized = true;
    transport->enabled = true;
    transport->medium = BLUETOOTH_TRANSPORT_MEDIUM_CLASSIC_AUDIO;
    transport->local_peer_id = 1U;
    transport->backend_ready = true;
    transport->network_connected = true;
}

bool bluetooth_transport_set_enabled(bluetooth_transport_t *transport, bool enabled) {
    if (transport == NULL || !transport->initialized) {
        return false;
    }

    transport->enabled = enabled;
    transport->error = false;
    transport->backend_ready = enabled;
    transport->network_connected = enabled;

    if (!enabled) {
        transport->pending_pair_peer_id = 0U;
    }

    return true;
}

void bluetooth_transport_set_local_peer_id(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL) {
        return;
    }

    transport->local_peer_id = peer_id != 0U ? peer_id : 1U;
}

uint8_t bluetooth_transport_local_peer_id(const bluetooth_transport_t *transport) {
    return transport != NULL && transport->local_peer_id != 0U ? transport->local_peer_id : 1U;
}

bool bluetooth_transport_connect(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || !transport->initialized || !transport->enabled || peer_id == 0U) {
        return false;
    }

    if (!bluetooth_transport_remember_peer(transport, peer_id)) {
        return false;
    }

    if (bluetooth_transport_has_peer(transport, peer_id)) {
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NONE;
        return true;
    }

    bluetooth_transport_peer_info_t *peer =
        bluetooth_transport_get_discovered_peer(transport, peer_id, true);
    if (peer == NULL) {
        transport->error = true;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_PEER_UNDISCOVERED;
        return false;
    }

    if (peer->name[0] == '\0') {
        bluetooth_transport_make_peer_name(peer->name, sizeof(peer->name), peer_id);
    }

    if (!peer->audio_ready && peer->last_seen_ms == 0U && !peer->pairing_pending) {
        peer->audio_ready = true;
    }

    if (!bluetooth_transport_peer_ready_for_audio(peer)) {
        peer->pairing_pending = true;
        transport->error = true;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_AUDIO_UNAVAILABLE;
        return false;
    }

    peer->pairing_pending = true;
    peer->disconnect_requested = false;
    peer->reconnect_blocked = false;
    transport->pending_pair_peer_id = peer_id;
    bluetooth_transport_mark_connected(transport, peer);
    return bluetooth_transport_has_peer(transport, peer_id);
}

bool bluetooth_transport_disconnect(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || !transport->initialized) {
        return false;
    }

    const size_t peer_index = bluetooth_transport_find_connected_index(transport, peer_id);
    if (peer_index >= transport->connected_peer_count) {
        transport->error = true;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NOT_CONNECTED;
        return false;
    }

    for (size_t shift = peer_index + 1U; shift < transport->connected_peer_count; ++shift) {
        transport->connected_peers[shift - 1U] = transport->connected_peers[shift];
        transport->peer_states[shift - 1U] = transport->peer_states[shift];
    }
    if (transport->connected_peer_count > 0U) {
        transport->connected_peer_count--;
    }
    transport->connected_peers[transport->connected_peer_count] = 0U;
    transport->peer_states[transport->connected_peer_count] =
        BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;

    bluetooth_transport_peer_info_t *peer =
        bluetooth_transport_get_discovered_peer(transport, peer_id, false);
    if (peer != NULL) {
        peer->paired = bluetooth_transport_peer_is_remembered(transport, peer_id);
        peer->pairing_pending = false;
        peer->disconnect_requested = true;
        peer->reconnect_blocked = true;
        peer->last_disconnected_ms = transport->last_poll_ms;
        peer->reconnect_attempts++;
    }

    (void)bluetooth_transport_drop_peer_packets(transport, peer_id);
    transport->pending_pair_peer_id = 0U;
    transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NONE;
    return true;
}

bool bluetooth_transport_restore_pairing(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || !transport->initialized || peer_id == 0U) {
        return false;
    }

    if (!bluetooth_transport_remember_peer(transport, peer_id)) {
        return false;
    }

    bluetooth_transport_peer_info_t *peer =
        bluetooth_transport_get_discovered_peer(transport, peer_id, true);
    if (peer != NULL && peer->name[0] == '\0') {
        bluetooth_transport_make_peer_name(peer->name, sizeof(peer->name), peer_id);
    }
    if (peer != NULL) {
        peer->paired = true;
        peer->disconnect_requested = false;
        peer->reconnect_blocked = false;
    }

    transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NONE;
    return true;
}

bool bluetooth_transport_report_peer(bluetooth_transport_t *transport, uint8_t peer_id,
                                     const char *name, bool audio_ready) {
    if (transport == NULL || !transport->initialized || peer_id == 0U ||
        peer_id == transport->local_peer_id) {
        return false;
    }

    bluetooth_transport_peer_info_t *peer =
        bluetooth_transport_get_discovered_peer(transport, peer_id, true);
    if (peer == NULL) {
        transport->error = true;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_PEER_LIMIT;
        return false;
    }

    peer->audio_ready = audio_ready;
    peer->last_seen_ms = transport->last_poll_ms;
    if (name != NULL && name[0] != '\0') {
        int written = snprintf(peer->name, sizeof(peer->name), "%s", name);
        if (written < 0) {
            return false;
        }
    } else if (peer->name[0] == '\0') {
        bluetooth_transport_make_peer_name(peer->name, sizeof(peer->name), peer_id);
    }

    if (bluetooth_transport_peer_is_remembered(transport, peer_id)) {
        peer->paired = true;
    }

    transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NONE;
    return true;
}

bool bluetooth_transport_poll(bluetooth_transport_t *transport) {
    if (transport == NULL || !transport->initialized || !transport->enabled) {
        return false;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &transport->discovered_peers[index];
        if (!peer->valid) {
            continue;
        }

        if (transport->last_poll_ms >= peer->last_seen_ms &&
            (transport->last_poll_ms - peer->last_seen_ms) >
            BLUETOOTH_TRANSPORT_DISCOVERY_TIMEOUT_MS) {
            bluetooth_transport_purge_stale_peer(transport, index);
            continue;
        }

        if (!bluetooth_transport_peer_is_remembered(transport, peer->peer_id) ||
            bluetooth_transport_has_peer(transport, peer->peer_id) ||
            !bluetooth_transport_peer_ready_for_audio(peer) || peer->reconnect_blocked ||
            peer->disconnect_requested) {
            continue;
        }

        (void)bluetooth_transport_connect(transport, peer->peer_id);
    }

    return transport->backend_ready;
}

bool bluetooth_transport_select_pairing_candidate(const bluetooth_transport_t *transport,
                                                  uint8_t *peer_id) {
    if (transport == NULL || peer_id == NULL) {
        return false;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        const bluetooth_transport_peer_info_t *peer = &transport->discovered_peers[index];
        if (!peer->valid || peer->peer_id == 0U || peer->pairing_pending ||
            !peer->audio_ready || bluetooth_transport_has_peer(transport, peer->peer_id)) {
            continue;
        }

        *peer_id = peer->peer_id;
        return true;
    }

    return false;
}

bool bluetooth_transport_is_connected(const bluetooth_transport_t *transport, uint8_t peer_id) {
    return bluetooth_transport_has_peer(transport, peer_id);
}

bool bluetooth_transport_queue_packet(bluetooth_transport_t *transport, uint8_t source_peer,
                                      uint8_t target_peer, const uint8_t *payload,
                                      size_t payload_len) {
    if (transport == NULL || !transport->initialized || !transport->enabled ||
        payload == NULL || payload_len == 0U) {
        return false;
    }

    if (!bluetooth_transport_is_connected(transport, source_peer) &&
        !bluetooth_transport_is_connected(transport, target_peer)) {
        transport->error = true;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NOT_CONNECTED;
        return false;
    }

    if (transport->queued_packet_count >= BLUETOOTH_TRANSPORT_QUEUE_DEPTH) {
        transport->packets_dropped++;
        transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_QUEUE_FULL;
        return false;
    }

    if (payload_len > BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN) {
        payload_len = BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN;
    }

    bluetooth_transport_packet_t *packet = &transport->queue[transport->queued_packet_count];
    packet->source_peer = source_peer;
    packet->target_peer = target_peer;
    packet->payload_len = payload_len;
    memcpy(packet->payload, payload, payload_len);

    transport->queued_packet_count++;
    transport->packets_queued++;
    transport->last_source_peer = source_peer;
    transport->last_target_peer = target_peer;
    transport->last_error_code = BLUETOOTH_TRANSPORT_ERROR_NONE;
    return true;
}

bool bluetooth_transport_dequeue_packet(bluetooth_transport_t *transport,
                                        bluetooth_transport_packet_t *packet) {
    if (transport == NULL || packet == NULL || transport->queued_packet_count == 0U) {
        return false;
    }

    *packet = transport->queue[0U];
    for (size_t index = 1U; index < transport->queued_packet_count; ++index) {
        transport->queue[index - 1U] = transport->queue[index];
    }
    transport->queued_packet_count--;
    transport->packets_delivered++;
    return true;
}

size_t bluetooth_transport_pending_count(const bluetooth_transport_t *transport) {
    return transport != NULL ? transport->queued_packet_count : 0U;
}

const char *bluetooth_transport_state_name(bluetooth_transport_state_t state) {
    switch (state) {
    case BLUETOOTH_TRANSPORT_STATE_CONNECTING:
        return "connecting";
    case BLUETOOTH_TRANSPORT_STATE_CONNECTED:
        return "connected";
    case BLUETOOTH_TRANSPORT_STATE_DISCONNECTING:
        return "disconnecting";
    case BLUETOOTH_TRANSPORT_STATE_DISCONNECTED:
    default:
        return "disconnected";
    }
}
