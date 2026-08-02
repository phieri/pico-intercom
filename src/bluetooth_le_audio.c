#include "bluetooth_le_audio.h"

#include <stdio.h>
#include <string.h>

#if defined(PICO_INTERCOM_TARGET)
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#endif

#if defined(PICO_INTERCOM_TARGET)
static bool bluetooth_le_audio_target_initialized = false;

static void bluetooth_le_audio_target_initialize(void) {
    if (bluetooth_le_audio_target_initialized) {
        return;
    }

    btstack_init();
    gap_set_local_name("Pico Intercom");

    static const uint8_t advertisement_data[] = {
        0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
        0x0E, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 'P', 'i', 'c', 'o', ' ', 'I', 'n', 't', 'e', 'r', 'c', 'o', 'm',
    };
    gap_advertisements_set_data((uint8_t *)advertisement_data, sizeof(advertisement_data));
    gap_advertisements_set_params(0x00A0U, 0x00A0U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U);
    gap_advertisements_enable(1);

    bluetooth_le_audio_target_initialized = true;
}
#endif

static bool bluetooth_le_audio_stack_is_ready(const bluetooth_le_audio_stack_t *stack) {
    return stack != NULL && stack->initialized;
}

static uint32_t bluetooth_le_audio_now_ms(void) {
#if defined(PICO_INTERCOM_TARGET)
    return to_ms_since_boot(get_absolute_time());
#else
    return 0U;
#endif
}

static void bluetooth_le_audio_set_transport_online(bluetooth_le_audio_stack_t *stack, bool enabled) {
    if (stack == NULL) {
        return;
    }

    stack->transport.backend_ready = enabled;
    stack->transport.network_connected = enabled;
}

static void bluetooth_le_audio_make_peer_name(char *buffer, size_t buffer_len, uint8_t peer_id) {
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }

    (void)snprintf(buffer, buffer_len, "headset-%u", (unsigned)peer_id);
}

static size_t bluetooth_le_audio_find_connected_index(const bluetooth_le_audio_stack_t *stack,
                                                     uint8_t peer_id) {
    if (stack == NULL) {
        return 0U;
    }

    for (size_t index = 0; index < stack->transport.connected_peer_count; ++index) {
        if (stack->transport.connected_peers[index] == peer_id) {
            return index;
        }
    }

    return stack->transport.connected_peer_count;
}

static bool bluetooth_le_audio_has_connected_peer(const bluetooth_le_audio_stack_t *stack,
                                                 uint8_t peer_id) {
    return bluetooth_le_audio_find_connected_index(stack, peer_id) <
           (stack != NULL ? stack->transport.connected_peer_count : 0U);
}

static bluetooth_transport_peer_info_t *bluetooth_le_audio_get_peer(bluetooth_le_audio_stack_t *stack,
                                                                   uint8_t peer_id,
                                                                   bool create);
static size_t bluetooth_le_audio_drop_outbound_for_peer(bluetooth_le_audio_stack_t *stack,
                                                       uint8_t peer_id);

static void bluetooth_le_audio_refresh_connected_flag(bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL) {
        return;
    }

    stack->connected = false;
    for (size_t index = 0; index < stack->transport.connected_peer_count; ++index) {
        if (stack->transport.peer_states[index] == BLUETOOTH_TRANSPORT_STATE_CONNECTED) {
            stack->connected = true;
            return;
        }
    }
}

static size_t bluetooth_le_audio_reserve_connected_slot(bluetooth_le_audio_stack_t *stack,
                                                       uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return INTERCOM_MAX_PEERS;
    }

    const size_t connected_index = bluetooth_le_audio_find_connected_index(stack, peer_id);
    if (connected_index < stack->transport.connected_peer_count) {
        return connected_index;
    }

    if (stack->transport.connected_peer_count >= INTERCOM_MAX_PEERS) {
        stack->transport.error = true;
        return INTERCOM_MAX_PEERS;
    }

    const size_t slot = stack->transport.connected_peer_count++;
    stack->transport.connected_peers[slot] = peer_id;
    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
    return slot;
}

static void bluetooth_le_audio_mark_connecting(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return;
    }

    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, true);
    const size_t slot = bluetooth_le_audio_reserve_connected_slot(stack, peer_id);
    if (peer == NULL || slot >= INTERCOM_MAX_PEERS) {
        return;
    }

    peer->pairing_pending = true;
    peer->disconnect_requested = false;
    peer->reconnect_blocked = false;
    peer->last_state_change_ms = stack->transport.last_poll_ms;
    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_CONNECTING;
    bluetooth_le_audio_refresh_connected_flag(stack);
}

static void bluetooth_le_audio_mark_disconnecting(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return;
    }

    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, false);
    const size_t slot = bluetooth_le_audio_reserve_connected_slot(stack, peer_id);
    if (slot >= INTERCOM_MAX_PEERS) {
        return;
    }

    if (peer != NULL) {
        peer->disconnect_requested = true;
        peer->pairing_pending = false;
        peer->last_state_change_ms = stack->transport.last_poll_ms;
    }
    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_DISCONNECTING;
    bluetooth_le_audio_refresh_connected_flag(stack);
}

static bluetooth_transport_peer_info_t *bluetooth_le_audio_get_peer(bluetooth_le_audio_stack_t *stack,
                                                                   uint8_t peer_id,
                                                                   bool create) {
    if (stack == NULL || peer_id == 0U || peer_id == stack->transport.local_peer_id) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &stack->transport.discovered_peers[index];
        if (!peer->valid || peer->peer_id != peer_id) {
            continue;
        }
        return peer;
    }

    if (!create) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &stack->transport.discovered_peers[index];
        if (peer->valid) {
            continue;
        }

        memset(peer, 0, sizeof(*peer));
        peer->valid = true;
        peer->peer_id = peer_id;
        bluetooth_le_audio_make_peer_name(peer->name, sizeof(peer->name), peer_id);
        if (stack->transport.discovered_peer_count < INTERCOM_MAX_PEERS) {
            stack->transport.discovered_peer_count++;
        }
        return peer;
    }

    return NULL;
}

static bool bluetooth_le_audio_remember_peer(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return false;
    }

    for (size_t index = 0; index < stack->transport.remembered_peer_count; ++index) {
        if (stack->transport.remembered_peers[index] == peer_id) {
            return true;
        }
    }

    if (stack->transport.remembered_peer_count >= INTERCOM_MAX_PEERS) {
        stack->transport.error = true;
        return false;
    }

    stack->transport.remembered_peers[stack->transport.remembered_peer_count++] = peer_id;
    return true;
}

static bool bluetooth_le_audio_peer_is_remembered(const bluetooth_le_audio_stack_t *stack,
                                                 uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return false;
    }

    for (size_t index = 0; index < stack->transport.remembered_peer_count; ++index) {
        if (stack->transport.remembered_peers[index] == peer_id) {
            return true;
        }
    }

    return false;
}

static void bluetooth_le_audio_mark_connected(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return;
    }

    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, true);
    if (peer == NULL) {
        return;
    }

    peer->paired = true;
    peer->pairing_pending = false;
    peer->disconnect_requested = false;
    peer->reconnect_blocked = false;
    peer->audio_ready = true;
    peer->last_connected_ms = stack->transport.last_poll_ms;
    peer->last_state_change_ms = stack->transport.last_poll_ms;
    peer->sdp_query_attempts = 0U;
    peer->connect_attempts = 0U;
    peer->last_sdp_query_ms = 0U;
    peer->last_connect_attempt_ms = 0U;

    const size_t slot = bluetooth_le_audio_reserve_connected_slot(stack, peer_id);
    if (slot >= INTERCOM_MAX_PEERS) {
        return;
    }

    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
    bluetooth_le_audio_refresh_connected_flag(stack);
}

static void bluetooth_le_audio_mark_disconnected(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return;
    }

    const size_t connected_index = bluetooth_le_audio_find_connected_index(stack, peer_id);
    if (connected_index < stack->transport.connected_peer_count) {
        for (size_t shift = connected_index + 1U; shift < stack->transport.connected_peer_count;
             ++shift) {
            stack->transport.connected_peers[shift - 1U] = stack->transport.connected_peers[shift];
            stack->transport.peer_states[shift - 1U] = stack->transport.peer_states[shift];
        }
        stack->transport.connected_peer_count--;
        stack->transport.connected_peers[stack->transport.connected_peer_count] = 0U;
        stack->transport.peer_states[stack->transport.connected_peer_count] =
            BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
    }

    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, false);
    if (peer != NULL) {
        peer->pairing_pending = false;
        peer->disconnect_requested = false;
        peer->paired = bluetooth_le_audio_peer_is_remembered(stack, peer_id);
        peer->last_disconnected_ms = stack->transport.last_poll_ms;
        peer->last_state_change_ms = stack->transport.last_poll_ms;
        peer->reconnect_attempts++;
    }

    (void)bluetooth_le_audio_drop_outbound_for_peer(stack, peer_id);
    if (stack->paired_peer_id == peer_id) {
        stack->paired_peer_id = 0U;
    }
    bluetooth_le_audio_refresh_connected_flag(stack);
}

static bool bluetooth_le_audio_enqueue_outbound(bluetooth_le_audio_stack_t *stack,
                                               uint8_t source_peer, uint8_t target_peer,
                                               const uint8_t *payload, size_t payload_len) {
    if (stack == NULL || payload == NULL || payload_len == 0U) {
        return false;
    }

    if (stack->outbound_packet_count >= BLUETOOTH_TRANSPORT_QUEUE_DEPTH) {
        stack->transport.packets_dropped++;
        return false;
    }

    if (payload_len > BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN) {
        payload_len = BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN;
    }

    bluetooth_le_audio_packet_t *packet = &stack->outbound_queue[stack->outbound_packet_count++];
    packet->source_peer = source_peer;
    packet->target_peer = target_peer;
    packet->payload_len = payload_len;
    memcpy(packet->payload, payload, payload_len);

    stack->transport.packets_queued++;
    stack->transport.last_source_peer = source_peer;
    stack->transport.last_target_peer = target_peer;
    return true;
}

static bool bluetooth_le_audio_remove_outbound_at(bluetooth_le_audio_stack_t *stack, size_t index,
                                                 bluetooth_le_audio_packet_t *packet) {
    if (stack == NULL || index >= stack->outbound_packet_count || packet == NULL) {
        return false;
    }

    *packet = stack->outbound_queue[index];
    for (size_t shift = index + 1U; shift < stack->outbound_packet_count; ++shift) {
        stack->outbound_queue[shift - 1U] = stack->outbound_queue[shift];
    }
    stack->outbound_packet_count--;
    return true;
}

static size_t bluetooth_le_audio_drop_outbound_for_peer(bluetooth_le_audio_stack_t *stack,
                                                       uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return 0U;
    }

    size_t removed = 0U;
    size_t index = 0U;
    while (index < stack->outbound_packet_count) {
        if (stack->outbound_queue[index].target_peer != peer_id &&
            stack->outbound_queue[index].source_peer != peer_id) {
            index++;
            continue;
        }

        bluetooth_le_audio_packet_t dropped_packet = {0};
        (void)bluetooth_le_audio_remove_outbound_at(stack, index, &dropped_packet);
        removed++;
    }

    stack->transport.packets_dropped += removed;
    return removed;
}

#if defined(PICO_INTERCOM_TARGET)
static void bluetooth_le_audio_backend_packet_handler(uint8_t packet_type, uint16_t channel,
                                                     uint8_t *packet, uint16_t size);

enum {
    BLUETOOTH_LE_AUDIO_RFCOMM_SERVER_CHANNEL = 17U,
    BLUETOOTH_LE_AUDIO_SDP_BUFFER_BYTES = 180U,
    BLUETOOTH_LE_AUDIO_INQUIRY_DURATION = 4U,
    BLUETOOTH_LE_AUDIO_DEVICE_CLASS = 0x240404U,
    BLUETOOTH_LE_AUDIO_SDP_MAX_ATTEMPTS = 3U,
};

#define BLUETOOTH_LE_AUDIO_DEFAULT_PIN "0000"

typedef struct {
    bool valid;
    uint8_t peer_id;
    bd_addr_t address;
    uint16_t rfcomm_cid;
    uint16_t rfcomm_mtu;
    uint8_t rfcomm_channel;
    bool sdp_query_needed;
    bool connect_requested;
} bluetooth_le_audio_backend_peer_t;

typedef struct {
    bool initialized;
    bool service_registered;
    bool powered_on;
    bool inquiry_active;
    bool sdp_query_active;
    bool can_send_pending;
    uint8_t sdp_query_peer_id;
    uint8_t connect_peer_id;
    uint16_t pending_send_cid;
    uint8_t spp_service_buffer[BLUETOOTH_LE_AUDIO_SDP_BUFFER_BYTES];
    btstack_packet_callback_registration_t event_registration;
    bluetooth_le_audio_backend_peer_t peers[INTERCOM_MAX_PEERS];
} bluetooth_le_audio_backend_state_t;

static bluetooth_le_audio_backend_state_t bluetooth_le_audio_backend;
static bluetooth_le_audio_stack_t *bluetooth_le_audio_active_stack = NULL;

static void bluetooth_le_audio_backend_set_local_name(void) {
    bd_addr_t local_address;
    char local_name[40];

    gap_local_bd_addr(local_address);
    (void)snprintf(local_name, sizeof(local_name), "Pico Intercom %s",
                   bd_addr_to_str(local_address));
    gap_set_local_name(local_name);
}

static uint8_t bluetooth_le_audio_peer_id_from_address(const bd_addr_t address) {
    uint8_t crc = 0U;
    for (size_t index = 0; index < 6U; ++index) {
        crc ^= address[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x80U) != 0U ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    return (uint8_t)((crc % 250U) + 1U);
}

static bluetooth_le_audio_backend_peer_t *bluetooth_le_audio_backend_peer_by_id(uint8_t peer_id,
                                                                               bool create) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_le_audio_backend_peer_t *peer = &bluetooth_le_audio_backend.peers[index];
        if (peer->valid && peer->peer_id == peer_id) {
            return peer;
        }
    }

    if (!create) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_le_audio_backend_peer_t *peer = &bluetooth_le_audio_backend.peers[index];
        if (peer->valid) {
            continue;
        }
        memset(peer, 0, sizeof(*peer));
        peer->valid = true;
        peer->peer_id = peer_id;
        return peer;
    }

    return NULL;
}

static bluetooth_le_audio_backend_peer_t *bluetooth_le_audio_backend_peer_by_address(
    const bd_addr_t address, bool create) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_le_audio_backend_peer_t *peer = &bluetooth_le_audio_backend.peers[index];
        if (peer->valid && memcmp(peer->address, address, sizeof(peer->address)) == 0) {
            return peer;
        }
    }

    if (!create) {
        return NULL;
    }

    const uint8_t peer_id = bluetooth_le_audio_peer_id_from_address(address);
    bluetooth_le_audio_backend_peer_t *peer = bluetooth_le_audio_backend_peer_by_id(peer_id, true);
    if (peer == NULL) {
        return NULL;
    }
    memcpy(peer->address, address, sizeof(peer->address));
    return peer;
}

static bluetooth_le_audio_backend_peer_t *bluetooth_le_audio_backend_peer_by_cid(uint16_t rfcomm_cid) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_le_audio_backend_peer_t *peer = &bluetooth_le_audio_backend.peers[index];
        if (peer->valid && peer->rfcomm_cid == rfcomm_cid) {
            return peer;
        }
    }

    return NULL;
}

static bluetooth_transport_peer_info_t *bluetooth_le_audio_report_backend_peer(
    bluetooth_le_audio_stack_t *stack, const bd_addr_t address, const char *name, bool audio_ready) {
    if (stack == NULL) {
        return NULL;
    }

    const uint8_t peer_id = bluetooth_le_audio_peer_id_from_address(address);
    bluetooth_le_audio_backend_peer_t *backend_peer =
        bluetooth_le_audio_backend_peer_by_address(address, true);
    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, true);
    if (backend_peer == NULL || peer == NULL) {
        return NULL;
    }

    backend_peer->peer_id = peer_id;
    memcpy(backend_peer->address, address, sizeof(backend_peer->address));

    peer->audio_ready = audio_ready;
    peer->last_seen_ms = stack->transport.last_poll_ms;
    memcpy(peer->address, address, sizeof(peer->address));
    if (name != NULL && name[0] != '\0') {
        (void)snprintf(peer->name, sizeof(peer->name), "%s", name);
    } else if (peer->name[0] == '\0') {
        bluetooth_le_audio_make_peer_name(peer->name, sizeof(peer->name), peer_id);
    }
    if (bluetooth_le_audio_peer_is_remembered(stack, peer_id)) {
        peer->paired = true;
    }
    return peer;
}

static void bluetooth_le_audio_backend_start_inquiry(void) {
    if (bluetooth_le_audio_active_stack == NULL || !bluetooth_le_audio_active_stack->enabled ||
        bluetooth_le_audio_backend.inquiry_active || bluetooth_le_audio_backend.sdp_query_active) {
        return;
    }

    if (gap_inquiry_start(BLUETOOTH_LE_AUDIO_INQUIRY_DURATION) == ERROR_CODE_SUCCESS) {
        bluetooth_le_audio_backend.inquiry_active = true;
    }
}

static void bluetooth_le_audio_backend_note_channel(uint8_t peer_id, uint8_t rfcomm_channel,
                                                   const char *name) {
    bluetooth_le_audio_backend_peer_t *backend_peer =
        bluetooth_le_audio_backend_peer_by_id(peer_id, true);
    if (backend_peer == NULL || bluetooth_le_audio_active_stack == NULL) {
        return;
    }

    backend_peer->rfcomm_channel = rfcomm_channel;
    backend_peer->sdp_query_needed = false;
    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(bluetooth_le_audio_active_stack,
                                                                       peer_id, true);
    if (peer != NULL) {
        peer->audio_ready = rfcomm_channel != 0U;
        peer->last_seen_ms = bluetooth_le_audio_active_stack->transport.last_poll_ms;
        if (name != NULL && name[0] != '\0') {
            (void)snprintf(peer->name, sizeof(peer->name), "%s", name);
        }
    }
}

static void bluetooth_le_audio_backend_request_sdp_query(uint8_t peer_id) {
    bluetooth_le_audio_backend_peer_t *backend_peer =
        bluetooth_le_audio_backend_peer_by_id(peer_id, false);
    if (backend_peer == NULL || !backend_peer->valid) {
        return;
    }
    backend_peer->sdp_query_needed = true;
}

static void bluetooth_le_audio_backend_maybe_start_sdp_query(void) {
    if (bluetooth_le_audio_active_stack == NULL || bluetooth_le_audio_backend.sdp_query_active) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_le_audio_backend_peer_t *backend_peer = &bluetooth_le_audio_backend.peers[index];
        if (!backend_peer->valid || !backend_peer->sdp_query_needed) {
            continue;
        }

        bluetooth_transport_peer_info_t *peer =
            bluetooth_le_audio_get_peer(bluetooth_le_audio_active_stack, backend_peer->peer_id, false);
        if (peer != NULL && peer->last_sdp_query_ms != 0U &&
            (bluetooth_le_audio_active_stack->transport.last_poll_ms - peer->last_sdp_query_ms) <
                BLUETOOTH_TRANSPORT_SDP_RETRY_MS) {
            continue;
        }

        if (memcmp(backend_peer->address, (bd_addr_t){0}, sizeof(backend_peer->address)) == 0) {
            continue;
        }

        if (sdp_client_query_rfcomm_channel_and_name_for_uuid(
                bluetooth_le_audio_backend_packet_handler, backend_peer->address,
                BLUETOOTH_SERVICE_CLASS_SERIAL_PORT) != ERROR_CODE_SUCCESS) {
            continue;
        }

        bluetooth_le_audio_backend.sdp_query_active = true;
        bluetooth_le_audio_backend.sdp_query_peer_id = backend_peer->peer_id;
        backend_peer->sdp_query_needed = false;
        if (peer != NULL) {
            peer->sdp_query_attempts++;
            peer->last_sdp_query_ms = bluetooth_le_audio_active_stack->transport.last_poll_ms;
        }
        return;
    }
}

static void bluetooth_le_audio_backend_maybe_start_connect(void) {
    if (bluetooth_le_audio_active_stack == NULL || bluetooth_le_audio_backend.connect_peer_id != 0U) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_le_audio_backend_peer_t *backend_peer = &bluetooth_le_audio_backend.peers[index];
        if (!backend_peer->valid || !backend_peer->connect_requested || backend_peer->rfcomm_cid != 0U ||
            backend_peer->rfcomm_channel == 0U) {
            continue;
        }

        bluetooth_transport_peer_info_t *peer =
            bluetooth_le_audio_get_peer(bluetooth_le_audio_active_stack, backend_peer->peer_id, false);
        if (peer != NULL && peer->last_connect_attempt_ms != 0U &&
            (bluetooth_le_audio_active_stack->transport.last_poll_ms - peer->last_connect_attempt_ms) <
                BLUETOOTH_TRANSPORT_CONNECT_RETRY_MS) {
            continue;
        }

        if (rfcomm_create_channel(bluetooth_le_audio_backend_packet_handler,
                                  backend_peer->address, backend_peer->rfcomm_channel,
                                  &backend_peer->rfcomm_cid) != ERROR_CODE_SUCCESS) {
            continue;
        }

        bluetooth_le_audio_backend.connect_peer_id = backend_peer->peer_id;
        if (peer != NULL) {
            peer->connect_attempts++;
            peer->last_connect_attempt_ms =
                bluetooth_le_audio_active_stack->transport.last_poll_ms;
        }
        bluetooth_le_audio_mark_connecting(bluetooth_le_audio_active_stack, backend_peer->peer_id);
        return;
    }
}

static void bluetooth_le_audio_backend_maybe_request_send_now(bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL || stack->outbound_packet_count == 0U || bluetooth_le_audio_backend.can_send_pending) {
        return;
    }

    for (size_t index = 0; index < stack->outbound_packet_count; ++index) {
        bluetooth_le_audio_packet_t *packet = &stack->outbound_queue[index];
        bluetooth_le_audio_backend_peer_t *backend_peer =
            bluetooth_le_audio_backend_peer_by_id(packet->target_peer, false);
        if (backend_peer == NULL || backend_peer->rfcomm_cid == 0U) {
            continue;
        }

        bluetooth_le_audio_backend.can_send_pending = true;
        bluetooth_le_audio_backend.pending_send_cid = backend_peer->rfcomm_cid;
        if (rfcomm_request_can_send_now_event(backend_peer->rfcomm_cid) != ERROR_CODE_SUCCESS) {
            bluetooth_le_audio_backend.can_send_pending = false;
            bluetooth_le_audio_backend.pending_send_cid = 0U;
        }
        return;
    }
}

static void bluetooth_le_audio_backend_maybe_autoreconnect(bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &stack->transport.discovered_peers[index];
        if (!peer->valid || !peer->audio_ready || peer->disconnect_requested ||
            peer->reconnect_blocked ||
            bluetooth_le_audio_has_connected_peer(stack, peer->peer_id)) {
            continue;
        }
        if (!peer->pairing_pending && !bluetooth_le_audio_peer_is_remembered(stack, peer->peer_id)) {
            continue;
        }

        bluetooth_le_audio_backend_peer_t *backend_peer =
            bluetooth_le_audio_backend_peer_by_id(peer->peer_id, false);
        if (backend_peer == NULL) {
            continue;
        }

        if (backend_peer->rfcomm_channel == 0U) {
            bluetooth_le_audio_backend_request_sdp_query(peer->peer_id);
            continue;
        }

        backend_peer->connect_requested = true;
        bluetooth_le_audio_mark_connecting(stack, peer->peer_id);
    }
}

static void bluetooth_le_audio_backend_handle_can_send_now(bluetooth_le_audio_stack_t *stack,
                                                          uint16_t rfcomm_cid) {
    if (stack == NULL) {
        return;
    }

    bluetooth_le_audio_backend.can_send_pending = false;
    bluetooth_le_audio_backend.pending_send_cid = 0U;

    for (size_t index = 0; index < stack->outbound_packet_count; ++index) {
        const bluetooth_le_audio_packet_t packet = stack->outbound_queue[index];
        bluetooth_le_audio_backend_peer_t *backend_peer =
            bluetooth_le_audio_backend_peer_by_id(packet.target_peer, false);
        if (backend_peer == NULL) {
            bluetooth_le_audio_packet_t dropped_packet = {0};
            (void)bluetooth_le_audio_remove_outbound_at(stack, index, &dropped_packet);
            stack->transport.packets_dropped++;
            index--;
            continue;
        }
        if (backend_peer->rfcomm_cid != rfcomm_cid) {
            continue;
        }

        if (backend_peer->rfcomm_mtu != 0U && packet.payload_len > backend_peer->rfcomm_mtu) {
            printf("Bluetooth LE Audio dropping oversized packet for peer %u (%u > mtu %u).\n",
                   (unsigned)packet.target_peer, (unsigned)packet.payload_len,
                   (unsigned)backend_peer->rfcomm_mtu);
            bluetooth_le_audio_packet_t dropped_packet = {0};
            (void)bluetooth_le_audio_remove_outbound_at(stack, index, &dropped_packet);
            stack->transport.packets_dropped++;
            if (stack->outbound_packet_count > 0U) {
                bluetooth_le_audio_backend_maybe_request_send_now(stack);
            }
            return;
        }

        if (rfcomm_send(rfcomm_cid, (uint8_t *)packet.payload, (uint16_t)packet.payload_len) !=
            ERROR_CODE_SUCCESS) {
            printf("Bluetooth LE Audio RFCOMM send failed for peer %u; resetting channel.\n",
                   (unsigned)packet.target_peer);
            stack->transport.error = true;
            (void)bluetooth_le_audio_drop_outbound_for_peer(stack, packet.target_peer);
            bluetooth_le_audio_mark_disconnecting(stack, packet.target_peer);
            (void)rfcomm_disconnect(rfcomm_cid);
            return;
        }

        bluetooth_le_audio_packet_t delivered_packet = {0};
        (void)bluetooth_le_audio_remove_outbound_at(stack, index, &delivered_packet);
        stack->transport.packets_delivered++;
        stack->transport.last_source_peer = packet.source_peer;
        stack->transport.last_target_peer = packet.target_peer;
        if (stack->outbound_packet_count > 0U) {
            bluetooth_le_audio_backend_maybe_request_send_now(stack);
        }
        return;
    }
}

static void bluetooth_le_audio_backend_packet_handler(uint8_t packet_type, uint16_t channel,
                                                     uint8_t *packet, uint16_t size) {
    UNUSED(channel);

    bluetooth_le_audio_stack_t *stack = bluetooth_le_audio_active_stack;
    if (stack == NULL) {
        return;
    }

    bd_addr_t event_addr;
    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bluetooth_le_audio_set_transport_online(stack, true);
                bluetooth_le_audio_backend.powered_on = true;
                gap_connectable_control(1);
                gap_discoverable_control(1);
                bluetooth_le_audio_backend_start_inquiry();
            }
            break;
        case GAP_EVENT_INQUIRY_RESULT:
            gap_event_inquiry_result_get_bd_addr(packet, event_addr);
            if (memcmp(event_addr, (bd_addr_t){0}, sizeof(event_addr)) == 0) {
                break;
            }
            if (bluetooth_le_audio_report_backend_peer(stack, event_addr, NULL, false) != NULL) {
                bluetooth_le_audio_backend_request_sdp_query(
                    bluetooth_le_audio_peer_id_from_address(event_addr));
            }
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE:
            bluetooth_le_audio_backend.inquiry_active = false;
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            printf("Bluetooth LE Audio legacy PIN requested for %s; using %s.\n",
                   bd_addr_to_str(event_addr), BLUETOOTH_LE_AUDIO_DEFAULT_PIN);
            gap_pin_code_response(event_addr, BLUETOOTH_LE_AUDIO_DEFAULT_PIN);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            printf("Bluetooth LE Audio SSP confirmation requested for %s; accepting.\n",
                   bd_addr_to_str(event_addr));
            gap_ssp_confirmation_response(event_addr);
            break;
        case RFCOMM_EVENT_INCOMING_CONNECTION: {
            rfcomm_event_incoming_connection_get_bd_addr(packet, event_addr);
            bluetooth_le_audio_backend_peer_t *backend_peer =
                bluetooth_le_audio_backend_peer_by_address(event_addr, true);
            if (backend_peer != NULL) {
                backend_peer->peer_id = bluetooth_le_audio_peer_id_from_address(event_addr);
                backend_peer->rfcomm_channel =
                    rfcomm_event_incoming_connection_get_server_channel(packet);
                backend_peer->rfcomm_cid =
                    rfcomm_event_incoming_connection_get_rfcomm_cid(packet);
                backend_peer->connect_requested = true;
                (void)bluetooth_le_audio_report_backend_peer(stack, event_addr, NULL, true);
                bluetooth_le_audio_mark_connecting(stack, backend_peer->peer_id);
                printf("Bluetooth LE Audio incoming RFCOMM connection from peer %u.\n",
                       (unsigned)backend_peer->peer_id);
                (void)rfcomm_accept_connection(backend_peer->rfcomm_cid);
            } else {
                (void)rfcomm_decline_connection(
                    rfcomm_event_incoming_connection_get_rfcomm_cid(packet));
            }
            break;
        }
        case RFCOMM_EVENT_CHANNEL_OPENED: {
            const uint8_t status = rfcomm_event_channel_opened_get_status(packet);
            rfcomm_event_channel_opened_get_bd_addr(packet, event_addr);
            bluetooth_le_audio_backend.connect_peer_id = 0U;
            bluetooth_le_audio_backend_peer_t *backend_peer =
                bluetooth_le_audio_backend_peer_by_address(event_addr, true);
            const uint8_t peer_id =
                backend_peer != NULL ? bluetooth_le_audio_peer_id_from_address(event_addr) : 0U;
            if (backend_peer == NULL) {
                break;
            }

            backend_peer->peer_id = peer_id;

            if (status != ERROR_CODE_SUCCESS) {
                backend_peer->rfcomm_cid = 0U;
                backend_peer->rfcomm_mtu = 0U;
                backend_peer->connect_requested = true;
                backend_peer->sdp_query_needed = backend_peer->rfcomm_channel == 0U;
                stack->transport.error = true;
                bluetooth_le_audio_mark_disconnected(stack, peer_id);
                printf("Bluetooth LE Audio RFCOMM open failed for peer %u (status 0x%02x).\n",
                       (unsigned)peer_id, (unsigned)status);
                break;
            }

            backend_peer->rfcomm_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            backend_peer->rfcomm_mtu = rfcomm_event_channel_opened_get_max_frame_size(packet);
            backend_peer->connect_requested = false;
            (void)bluetooth_le_audio_report_backend_peer(stack, event_addr, NULL, true);
            bluetooth_le_audio_mark_connected(stack, backend_peer->peer_id);
            stack->paired_peer_id = backend_peer->peer_id;
            printf("Bluetooth LE Audio RFCOMM channel ready for peer %u (mtu=%u).\n",
                   (unsigned)backend_peer->peer_id, (unsigned)backend_peer->rfcomm_mtu);
            break;
        }
        case RFCOMM_EVENT_CAN_SEND_NOW:
            bluetooth_le_audio_backend_handle_can_send_now(
                stack, rfcomm_event_can_send_now_get_rfcomm_cid(packet));
            break;
        case RFCOMM_EVENT_CHANNEL_CLOSED: {
            bluetooth_le_audio_backend.can_send_pending = false;
            bluetooth_le_audio_backend.pending_send_cid = 0U;
            bluetooth_le_audio_backend_peer_t *backend_peer =
                bluetooth_le_audio_backend_peer_by_cid(
                    rfcomm_event_channel_closed_get_rfcomm_cid(packet));
            if (backend_peer == NULL) {
                break;
            }
            bluetooth_transport_peer_info_t *peer =
                bluetooth_le_audio_get_peer(stack, backend_peer->peer_id, false);
            const bool reconnect_blocked =
                peer != NULL ? peer->reconnect_blocked : false;
            const bool reconnect_requested =
                !reconnect_blocked &&
                (bluetooth_le_audio_peer_is_remembered(stack, backend_peer->peer_id) ||
                 (peer != NULL && peer->pairing_pending));
            bluetooth_le_audio_mark_disconnected(stack, backend_peer->peer_id);
            if (peer != NULL) {
                peer->audio_ready = backend_peer->rfcomm_channel != 0U;
            }
            backend_peer->rfcomm_cid = 0U;
            backend_peer->rfcomm_mtu = 0U;
            backend_peer->connect_requested = reconnect_requested;
            backend_peer->sdp_query_needed =
                reconnect_requested && backend_peer->rfcomm_channel == 0U;
            printf("Bluetooth LE Audio RFCOMM channel closed for peer %u.\n",
                   (unsigned)backend_peer->peer_id);
            break;
        }
        case SDP_EVENT_QUERY_RFCOMM_SERVICE:
            bluetooth_le_audio_backend_note_channel(
                bluetooth_le_audio_backend.sdp_query_peer_id,
                sdp_event_query_rfcomm_service_get_rfcomm_channel(packet),
                sdp_event_query_rfcomm_service_get_name(packet));
            break;
        case SDP_EVENT_QUERY_COMPLETE:
            bluetooth_le_audio_backend.sdp_query_active = false;
            if (bluetooth_le_audio_backend.sdp_query_peer_id != 0U) {
                bluetooth_le_audio_backend_peer_t *backend_peer =
                    bluetooth_le_audio_backend_peer_by_id(
                        bluetooth_le_audio_backend.sdp_query_peer_id, false);
                bluetooth_transport_peer_info_t *peer =
                    bluetooth_le_audio_get_peer(stack, bluetooth_le_audio_backend.sdp_query_peer_id,
                                               false);
                if (backend_peer != NULL && backend_peer->rfcomm_channel == 0U) {
                    const bool should_retry =
                        peer != NULL && peer->sdp_query_attempts < BLUETOOTH_LE_AUDIO_SDP_MAX_ATTEMPTS;
                    backend_peer->sdp_query_needed = should_retry;
                    if (!should_retry) {
                        backend_peer->connect_requested = false;
                        if (peer != NULL) {
                            peer->audio_ready = false;
                            peer->pairing_pending = false;
                            peer->reconnect_blocked = true;
                        }
                        printf("Bluetooth LE Audio headset %u does not expose the required headset service for the current BL audio-oriented session path.\n",
                               (unsigned)bluetooth_le_audio_backend.sdp_query_peer_id);
                    }
                }
            }
            bluetooth_le_audio_backend.sdp_query_peer_id = 0U;
            break;
        default:
            break;
        }
        break;
    case RFCOMM_DATA_PACKET: {
        bluetooth_le_audio_backend_peer_t *backend_peer =
            bluetooth_le_audio_backend_peer_by_cid(channel);
        if (backend_peer == NULL || backend_peer->peer_id == 0U) {
            break;
        }

        (void)bluetooth_transport_queue_packet(&stack->transport, backend_peer->peer_id,
                                               stack->transport.local_peer_id, packet, size);
        break;
    }
    default:
        break;
    }
}

static bool bluetooth_le_audio_backend_init(bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL) {
        return false;
    }

    if (!bluetooth_le_audio_backend.initialized) {
        memset(&bluetooth_le_audio_backend, 0, sizeof(bluetooth_le_audio_backend));
        l2cap_init();
        rfcomm_init();
        sdp_init();

        memset(bluetooth_le_audio_backend.spp_service_buffer, 0,
               sizeof(bluetooth_le_audio_backend.spp_service_buffer));
        spp_create_sdp_record(bluetooth_le_audio_backend.spp_service_buffer,
                              sdp_create_service_record_handle(),
                              BLUETOOTH_LE_AUDIO_RFCOMM_SERVER_CHANNEL, "Pico Intercom");
        sdp_register_service(bluetooth_le_audio_backend.spp_service_buffer);
        rfcomm_register_service(bluetooth_le_audio_backend_packet_handler,
                                BLUETOOTH_LE_AUDIO_RFCOMM_SERVER_CHANNEL, 0xffff);
        bluetooth_le_audio_backend.event_registration.callback =
            &bluetooth_le_audio_backend_packet_handler;
        hci_add_event_handler(&bluetooth_le_audio_backend.event_registration);
        gap_set_class_of_device(BLUETOOTH_LE_AUDIO_DEVICE_CLASS);
        gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
        bluetooth_le_audio_backend_set_local_name();
        bluetooth_le_audio_backend.initialized = true;
        bluetooth_le_audio_backend.service_registered = true;
    }

    bluetooth_le_audio_active_stack = stack;
    return true;
}
#endif

void bluetooth_le_audio_stack_init(bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL) {
        return;
    }

    memset(stack, 0, sizeof(*stack));
    bluetooth_transport_init(&stack->transport);
    stack->initialized = true;
    stack->enabled = true;
    stack->discoverable = true;
    stack->pairing_enabled = true;
#if defined(PICO_INTERCOM_TARGET)
    bluetooth_le_audio_target_initialize();
    bluetooth_le_audio_set_transport_online(stack, false);
#else
    bluetooth_le_audio_set_transport_online(stack, true);
#endif
}

bool bluetooth_le_audio_stack_set_enabled(bluetooth_le_audio_stack_t *stack, bool enabled) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

    stack->enabled = enabled;
    stack->discoverable = enabled;
    stack->pairing_enabled = enabled;

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_le_audio_target_initialize();
    (void)bluetooth_transport_set_enabled(&stack->transport, enabled);
    if (enabled) {
        if (!bluetooth_le_audio_backend_init(stack)) {
            return false;
        }
        bluetooth_le_audio_active_stack = stack;
        gap_discoverable_control(1);
        gap_connectable_control(1);
        if (!bluetooth_le_audio_backend.powered_on) {
            hci_power_control(HCI_POWER_ON);
        }
    } else {
        stack->outbound_packet_count = 0U;
        bluetooth_le_audio_set_transport_online(stack, false);
        bluetooth_le_audio_backend.powered_on = false;
        bluetooth_le_audio_backend.inquiry_active = false;
        bluetooth_le_audio_backend.sdp_query_active = false;
        bluetooth_le_audio_backend.can_send_pending = false;
        bluetooth_le_audio_backend.connect_peer_id = 0U;
        bluetooth_le_audio_backend.sdp_query_peer_id = 0U;
        hci_power_control(HCI_POWER_OFF);
        while (stack->transport.connected_peer_count > 0U) {
            bluetooth_le_audio_mark_disconnected(stack, stack->transport.connected_peers[0]);
        }
    }
#else
    if (!bluetooth_transport_set_enabled(&stack->transport, enabled)) {
        return false;
    }
    bluetooth_le_audio_set_transport_online(stack, enabled);
#endif

    stack->connected = enabled && stack->transport.connected_peer_count > 0U;
    return true;
}

void bluetooth_le_audio_stack_set_local_peer_id(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL) {
        return;
    }

    bluetooth_transport_set_local_peer_id(&stack->transport, peer_id);
}

uint8_t bluetooth_le_audio_stack_local_peer_id(const bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL) {
        return 1U;
    }

    return bluetooth_transport_local_peer_id(&stack->transport);
}

bool bluetooth_le_audio_stack_pair(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

    stack->discoverable = true;
    stack->pairing_enabled = true;
    stack->paired_peer_id = peer_id;
#if defined(PICO_INTERCOM_TARGET)
    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, true);
    if (peer == NULL) {
        return false;
    }
    peer->pairing_pending = true;
    peer->reconnect_blocked = false;
    if (!bluetooth_le_audio_remember_peer(stack, peer_id)) {
        return false;
    }
    bluetooth_le_audio_mark_connecting(stack, peer_id);
    bluetooth_le_audio_backend_peer_t *backend_peer =
        bluetooth_le_audio_backend_peer_by_id(peer_id, true);
    if (backend_peer != NULL) {
        backend_peer->connect_requested = true;
        backend_peer->sdp_query_needed = backend_peer->rfcomm_channel == 0U;
    }
    return true;
#else
    return bluetooth_le_audio_stack_connect(stack, peer_id);
#endif
}

bool bluetooth_le_audio_stack_connect(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_le_audio_stack_is_ready(stack) || !stack->enabled || peer_id == 0U) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, true);
    if (peer == NULL) {
        return false;
    }
    bluetooth_le_audio_backend_peer_t *backend_peer =
        bluetooth_le_audio_backend_peer_by_id(peer_id, true);
    if (backend_peer == NULL) {
        return false;
    }
    backend_peer->connect_requested = true;
    backend_peer->sdp_query_needed = backend_peer->rfcomm_channel == 0U;
    peer->pairing_pending = true;
    peer->disconnect_requested = false;
    peer->reconnect_blocked = false;
    stack->paired_peer_id = peer_id;
    bluetooth_le_audio_mark_connecting(stack, peer_id);
    return true;
#else
    if (!bluetooth_transport_connect(&stack->transport, peer_id)) {
        return false;
    }

    stack->paired_peer_id = peer_id;
    stack->connected = stack->transport.connected_peer_count > 0U;
    return bluetooth_transport_is_connected(&stack->transport, peer_id);
#endif
}

bool bluetooth_le_audio_stack_disconnect(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_le_audio_backend_peer_t *backend_peer =
        bluetooth_le_audio_backend_peer_by_id(peer_id, false);
    if (backend_peer == NULL) {
        return false;
    }
    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, false);
    if (peer != NULL) {
        peer->reconnect_blocked = true;
    }
    backend_peer->connect_requested = false;
    backend_peer->sdp_query_needed = false;
    bluetooth_le_audio_mark_disconnecting(stack, peer_id);
    if (backend_peer->rfcomm_cid != 0U) {
        (void)rfcomm_disconnect(backend_peer->rfcomm_cid);
        return true;
    }
    bluetooth_le_audio_mark_disconnected(stack, peer_id);
    return true;
#else
    if (!bluetooth_transport_disconnect(&stack->transport, peer_id)) {
        return false;
    }

    if (stack->paired_peer_id == peer_id) {
        stack->paired_peer_id = 0U;
    }
    stack->connected = stack->transport.connected_peer_count > 0U;
    return true;
#endif
}

bool bluetooth_le_audio_stack_restore_pairing(bluetooth_le_audio_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    if (!bluetooth_le_audio_remember_peer(stack, peer_id)) {
        return false;
    }
    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, true);
    if (peer != NULL) {
        peer->paired = true;
        peer->pairing_pending = false;
        peer->disconnect_requested = false;
        peer->reconnect_blocked = false;
    }
    bluetooth_le_audio_backend_peer_t *backend_peer =
        bluetooth_le_audio_backend_peer_by_id(peer_id, true);
    if (backend_peer != NULL) {
        backend_peer->connect_requested = false;
        backend_peer->sdp_query_needed = false;
    }
    return true;
#else
    return bluetooth_transport_restore_pairing(&stack->transport, peer_id);
#endif
}

bool bluetooth_le_audio_stack_poll(bluetooth_le_audio_stack_t *stack) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

    stack->transport.last_poll_ms =
#if defined(PICO_INTERCOM_TARGET)
        bluetooth_le_audio_now_ms();
#else
        stack->transport.last_poll_ms + 100U;
#endif

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_le_audio_active_stack = stack;
    /* Drive the CYW43 driver and BTstack run loop so that HCI events,
     * RFCOMM channel changes, SDP results, and inbound data packets are
     * dispatched to bluetooth_le_audio_backend_packet_handler before the
     * backend state machine runs its reconnect and send-request logic. */
    cyw43_arch_poll();
    bluetooth_le_audio_backend_maybe_autoreconnect(stack);
    bluetooth_le_audio_backend_maybe_start_sdp_query();
    bluetooth_le_audio_backend_maybe_start_connect();
    bluetooth_le_audio_backend_maybe_request_send_now(stack);
    if (!bluetooth_le_audio_backend.inquiry_active &&
        stack->transport.connected_peer_count < INTERCOM_MAX_PEERS) {
        bluetooth_le_audio_backend_start_inquiry();
    }
    stack->connected = stack->transport.connected_peer_count > 0U;
    return stack->enabled;
#else
    const bool polled = bluetooth_transport_poll(&stack->transport);
    stack->connected = stack->transport.connected_peer_count > 0U;
    return polled;
#endif
}

bool bluetooth_le_audio_stack_report_headset(bluetooth_le_audio_stack_t *stack, uint8_t peer_id,
                                            const char *name, bool audio_ready) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_transport_peer_info_t *peer = bluetooth_le_audio_get_peer(stack, peer_id, true);
    if (peer == NULL) {
        return false;
    }
    const bool previous_audio_ready = peer->audio_ready;
    peer->audio_ready = audio_ready;
    peer->last_seen_ms = stack->transport.last_poll_ms;
    if (name != NULL && name[0] != '\0') {
        (void)snprintf(peer->name, sizeof(peer->name), "%s", name);
    }
    if (audio_ready) {
        bluetooth_le_audio_backend_peer_t *backend_peer =
            bluetooth_le_audio_backend_peer_by_id(peer_id, true);
        if (backend_peer != NULL && memcmp(backend_peer->address, (bd_addr_t){0}, 6U) != 0) {
            backend_peer->sdp_query_needed = backend_peer->rfcomm_channel == 0U;
        }
    }
    if (previous_audio_ready != audio_ready) {
        printf("Bluetooth LE Audio peer %u %s audio transport is %s.\n", (unsigned)peer_id,
               name != NULL && name[0] != '\0' ? name : "peer",
               audio_ready ? "ready" : "negotiating");
    }
    return true;
#else
    const bool reported =
        bluetooth_transport_report_peer(&stack->transport, peer_id, name, audio_ready);
    if (reported) {
        printf("Bluetooth LE Audio peer %u %s audio transport is %s.\n",
               (unsigned)peer_id, name != NULL && name[0] != '\0' ? name : "peer",
               audio_ready ? "ready" : "negotiating");
    }
    return reported;
#endif
}

bool bluetooth_le_audio_stack_select_pairing_candidate(const bluetooth_le_audio_stack_t *stack,
                                                      uint8_t *peer_id) {
    if (stack == NULL || peer_id == NULL) {
        return false;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        const bluetooth_transport_peer_info_t *peer = &stack->transport.discovered_peers[index];
        if (!peer->valid || peer->peer_id == 0U || peer->pairing_pending || !peer->audio_ready ||
            bluetooth_le_audio_has_connected_peer(stack, peer->peer_id)) {
            continue;
        }

        *peer_id = peer->peer_id;
        return true;
    }

    return false;
}

bool bluetooth_le_audio_stack_queue_packet(bluetooth_le_audio_stack_t *stack, uint8_t source_peer,
                                          uint8_t target_peer, const uint8_t *payload,
                                          size_t payload_len) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    if (!stack->enabled || payload == NULL || payload_len == 0U ||
        !bluetooth_le_audio_has_connected_peer(stack, target_peer)) {
        return false;
    }
    return bluetooth_le_audio_enqueue_outbound(stack, source_peer, target_peer, payload, payload_len);
#else
    return bluetooth_transport_queue_packet(&stack->transport, source_peer, target_peer, payload,
                                            payload_len);
#endif
}

bool bluetooth_le_audio_stack_dequeue_packet(bluetooth_le_audio_stack_t *stack,
                                            bluetooth_le_audio_packet_t *packet) {
    if (!bluetooth_le_audio_stack_is_ready(stack)) {
        return false;
    }

    return bluetooth_transport_dequeue_packet(&stack->transport, packet);
}

size_t bluetooth_le_audio_stack_pending_count(const bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL) {
        return 0U;
    }

#if defined(PICO_INTERCOM_TARGET)
    return bluetooth_transport_pending_count(&stack->transport) + stack->outbound_packet_count;
#else
    return bluetooth_transport_pending_count(&stack->transport);
#endif
}

const char *bluetooth_le_audio_stack_state_name(const bluetooth_le_audio_stack_t *stack) {
    if (stack == NULL || !stack->initialized) {
        return "uninitialized";
    }

    if (!stack->enabled) {
        return "disabled";
    }

    if (stack->connected) {
        return "connected";
    }

    if (stack->paired_peer_id != 0U) {
        return "pairing";
    }

    if (stack->discoverable) {
        return "discoverable";
    }

    return "idle";
}
