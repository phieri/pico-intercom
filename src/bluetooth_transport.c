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

void bluetooth_transport_init(bluetooth_transport_t *transport) {
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
    transport->initialized = true;
    transport->enabled = true;
    transport->error = false;
    transport->last_error_code = 0U;
}

bool bluetooth_transport_set_enabled(bluetooth_transport_t *transport, bool enabled) {
    if (transport == NULL || !transport->initialized) {
        return false;
    }

    transport->enabled = enabled;
    transport->error = false;
    return true;
}

bool bluetooth_transport_connect(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || !transport->initialized || !transport->enabled) {
        return false;
    }

    if (bluetooth_transport_has_peer(transport, peer_id)) {
        return true;
    }

    if (transport->connected_peer_count >= INTERCOM_MAX_PEERS) {
        transport->error = true;
        transport->last_error_code = 2U;
        return false;
    }

    transport->connected_peers[transport->connected_peer_count] = peer_id;
    transport->peer_states[transport->connected_peer_count] =
        BLUETOOTH_TRANSPORT_STATE_CONNECTED;
    transport->connected_peer_count++;
    transport->last_error_code = 0U;
    return true;
}

bool bluetooth_transport_disconnect(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || !transport->initialized) {
        return false;
    }

    const size_t peer_index = bluetooth_transport_find_peer_index(transport, peer_id);
    if (peer_index >= transport->connected_peer_count) {
        transport->error = true;
        transport->last_error_code = 3U;
        return false;
    }

    for (size_t shift = peer_index + 1U; shift < transport->connected_peer_count; ++shift) {
        transport->connected_peers[shift - 1U] = transport->connected_peers[shift];
        transport->peer_states[shift - 1U] = transport->peer_states[shift];
    }
    transport->connected_peer_count--;
    if (transport->connected_peer_count < INTERCOM_MAX_PEERS) {
        transport->peer_states[transport->connected_peer_count] =
            BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
    }

    transport->last_error_code = 0U;
    return true;
}

bool bluetooth_transport_is_connected(const bluetooth_transport_t *transport, uint8_t peer_id) {
    return bluetooth_transport_has_peer(transport, peer_id);
}

bool bluetooth_transport_queue_packet(bluetooth_transport_t *transport, uint8_t source_peer,
                                      uint8_t target_peer, const uint8_t *payload,
                                      size_t payload_len) {
    if (transport == NULL || !transport->initialized || !transport->enabled ||
        !bluetooth_transport_is_connected(transport, target_peer)) {
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
    packet->source_peer = source_peer;
    packet->target_peer = target_peer;
    packet->payload_len = payload_len;
    if (payload_len > 0U && payload != NULL) {
        memcpy(packet->payload, payload, payload_len);
    } else {
        memset(packet->payload, 0, sizeof(packet->payload));
    }

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
