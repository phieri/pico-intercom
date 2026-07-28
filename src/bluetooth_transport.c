#include "bluetooth_transport.h"

#include <string.h>

static size_t bluetooth_transport_find_peer_index(const bluetooth_transport_t *transport,
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
    return bluetooth_transport_find_peer_index(transport, peer_id) <
           (transport != NULL ? transport->connected_peer_count : 0U);
}

static bool bluetooth_transport_remember_peer(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || peer_id == 0U) {
        return false;
    }

    for (size_t index = 0; index < transport->remembered_peer_count; ++index) {
        if (transport->remembered_peers[index] == peer_id) {
            return true;
        }

        static bool bluetooth_transport_peer_is_discovered(const bluetooth_transport_t *transport,
                                                           uint8_t peer_id) {
            if (transport == NULL) {
                return false;
            }

            for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
                if (transport->discovered_peers[index].valid &&
                    transport->discovered_peers[index].peer_id == peer_id) {
                    return true;
                }
            }

            return false;
        }
    }

    if (transport->remembered_peer_count >= INTERCOM_MAX_PEERS) {
        transport->error = true;
        transport->last_error_code = 2U;
        return false;
    }

    transport->remembered_peers[transport->remembered_peer_count++] = peer_id;
    return true;
}

void bluetooth_transport_init(bluetooth_transport_t *transport) {
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
    transport->initialized = true;
    transport->enabled = true;
    transport->local_peer_id = 1U;
#if !defined(PICO_INTERCOM_TARGET)
    transport->backend_ready = true;
    transport->network_connected = true;
#endif
}

bool bluetooth_transport_set_enabled(bluetooth_transport_t *transport, bool enabled) {
    if (transport == NULL || !transport->initialized) {
        return false;
    }

    transport->enabled = enabled;
    transport->error = false;

#if !defined(PICO_INTERCOM_TARGET)
    transport->backend_ready = enabled;
    transport->network_connected = enabled;
#endif

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

#if defined(PICO_INTERCOM_TARGET)
    if (!bluetooth_transport_peer_is_discovered(transport, peer_id)) {
        transport->error = true;
        transport->last_error_code = 13U;
        return false;
    }

    transport->pending_pair_peer_id = peer_id;
    transport->last_error_code = 0U;
    return transport->backend_ready;
#else
    if (bluetooth_transport_has_peer(transport, peer_id)) {
        return true;
    }

    if (transport->connected_peer_count >= INTERCOM_MAX_PEERS) {
        transport->error = true;
        transport->last_error_code = 2U;
        return false;
    }

    const size_t peer_index = transport->connected_peer_count++;
    transport->connected_peers[peer_index] = peer_id;
    transport->peer_states[peer_index] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
    transport->last_error_code = 0U;
    return true;
#endif
}

bool bluetooth_transport_disconnect(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || !transport->initialized) {
        return false;
    }

    const size_t peer_index = bluetooth_transport_find_peer_index(transport, peer_id);
#if defined(PICO_INTERCOM_TARGET)
    if (peer_index >= transport->connected_peer_count) {
        transport->last_error_code = 0U;
        transport->pending_pair_peer_id = 0U;
        return true;
    }
#else
    if (peer_index >= transport->connected_peer_count) {
        transport->error = true;
        transport->last_error_code = 3U;
        return false;
    }
#endif

    for (size_t shift = peer_index + 1U; shift < transport->connected_peer_count; ++shift) {
        transport->connected_peers[shift - 1U] = transport->connected_peers[shift];
        transport->peer_states[shift - 1U] = transport->peer_states[shift];
    }
    if (transport->connected_peer_count > 0U) {
        transport->connected_peer_count--;
    }
    if (transport->connected_peer_count < INTERCOM_MAX_PEERS) {
        transport->peer_states[transport->connected_peer_count] =
            BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
    }

    transport->pending_pair_peer_id = 0U;
    transport->last_error_code = 0U;
    return true;
}

bool bluetooth_transport_restore_pairing(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || !transport->initialized || peer_id == 0U) {
        return false;
    }

    if (!bluetooth_transport_remember_peer(transport, peer_id)) {
        return false;
    }

    transport->last_error_code = 0U;
    return true;
}

bool bluetooth_transport_poll(bluetooth_transport_t *transport) {
    if (transport == NULL || !transport->initialized || !transport->enabled) {
        return false;
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
            bluetooth_transport_has_peer(transport, peer->peer_id)) {
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

    if (transport->queued_packet_count >= BLUETOOTH_TRANSPORT_QUEUE_DEPTH) {
        transport->packets_dropped++;
        transport->last_error_code = 4U;
        return false;
    }

    if (payload_len > BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN) {
        payload_len = BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN;
    }

    bluetooth_transport_packet_t *packet = &transport->queue[transport->queued_packet_count];
    memset(packet, 0, sizeof(*packet));
    packet->source_peer = source_peer;
    packet->target_peer = target_peer;
    packet->payload_len = payload_len;
    memcpy(packet->payload, payload, payload_len);

    transport->queued_packet_count++;
    transport->packets_queued++;
    transport->last_source_peer = source_peer;
    transport->last_target_peer = target_peer;
    transport->last_error_code = 0U;
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
