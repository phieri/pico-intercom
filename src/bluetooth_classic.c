#include "bluetooth_classic.h"

#include <stdio.h>
#include <string.h>

#if defined(PICO_INTERCOM_TARGET)
#include "btstack.h"
#include "classic/sdp_client_rfcomm.h"
#include "pico/stdlib.h"
#endif

static bool bluetooth_classic_stack_is_ready(const bluetooth_classic_stack_t *stack) {
    return stack != NULL && stack->initialized;
}

static uint32_t bluetooth_classic_now_ms(void) {
#if defined(PICO_INTERCOM_TARGET)
    return to_ms_since_boot(get_absolute_time());
#else
    return 0U;
#endif
}

static void bluetooth_classic_set_transport_online(bluetooth_classic_stack_t *stack, bool enabled) {
    if (stack == NULL) {
        return;
    }

    stack->transport.backend_ready = enabled;
    stack->transport.network_connected = enabled;
}

static void bluetooth_classic_make_peer_name(char *buffer, size_t buffer_len, uint8_t peer_id) {
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }

    (void)snprintf(buffer, buffer_len, "headset-%u", (unsigned)peer_id);
}

static size_t bluetooth_classic_find_connected_index(const bluetooth_classic_stack_t *stack,
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

static bool bluetooth_classic_has_connected_peer(const bluetooth_classic_stack_t *stack,
                                                 uint8_t peer_id) {
    return bluetooth_classic_find_connected_index(stack, peer_id) <
           (stack != NULL ? stack->transport.connected_peer_count : 0U);
}

static bluetooth_transport_peer_info_t *bluetooth_classic_get_peer(bluetooth_classic_stack_t *stack,
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
        bluetooth_classic_make_peer_name(peer->name, sizeof(peer->name), peer_id);
        if (stack->transport.discovered_peer_count < INTERCOM_MAX_PEERS) {
            stack->transport.discovered_peer_count++;
        }
        return peer;
    }

    return NULL;
}

static bool bluetooth_classic_remember_peer(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
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

static bool bluetooth_classic_peer_is_remembered(const bluetooth_classic_stack_t *stack,
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

static void bluetooth_classic_mark_connected(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return;
    }

    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(stack, peer_id, true);
    if (peer == NULL) {
        return;
    }

    peer->paired = true;
    peer->pairing_pending = false;
    peer->audio_ready = true;
    peer->last_connected_ms = stack->transport.last_poll_ms;

    const size_t connected_index = bluetooth_classic_find_connected_index(stack, peer_id);
    if (connected_index < stack->transport.connected_peer_count) {
        stack->transport.peer_states[connected_index] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
        stack->connected = true;
        return;
    }

    if (stack->transport.connected_peer_count >= INTERCOM_MAX_PEERS) {
        stack->transport.error = true;
        return;
    }

    const size_t slot = stack->transport.connected_peer_count++;
    stack->transport.connected_peers[slot] = peer_id;
    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
    stack->connected = true;
}

static void bluetooth_classic_mark_disconnected(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return;
    }

    const size_t connected_index = bluetooth_classic_find_connected_index(stack, peer_id);
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

    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(stack, peer_id, false);
    if (peer != NULL) {
        peer->pairing_pending = false;
        peer->paired = bluetooth_classic_peer_is_remembered(stack, peer_id);
        peer->audio_ready = false;
        peer->last_disconnected_ms = stack->transport.last_poll_ms;
        peer->reconnect_attempts++;
    }

    if (stack->paired_peer_id == peer_id) {
        stack->paired_peer_id = 0U;
    }
    stack->connected = stack->transport.connected_peer_count > 0U;
}

static bool bluetooth_classic_enqueue_outbound(bluetooth_classic_stack_t *stack,
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

    bluetooth_classic_packet_t *packet = &stack->outbound_queue[stack->outbound_packet_count++];
    packet->source_peer = source_peer;
    packet->target_peer = target_peer;
    packet->payload_len = payload_len;
    memcpy(packet->payload, payload, payload_len);

    stack->transport.packets_queued++;
    stack->transport.last_source_peer = source_peer;
    stack->transport.last_target_peer = target_peer;
    return true;
}

static bool bluetooth_classic_remove_outbound_at(bluetooth_classic_stack_t *stack, size_t index,
                                                 bluetooth_classic_packet_t *packet) {
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

#if defined(PICO_INTERCOM_TARGET)
static void bluetooth_classic_backend_packet_handler(uint8_t packet_type, uint16_t channel,
                                                     uint8_t *packet, uint16_t size);

enum {
    BLUETOOTH_CLASSIC_RFCOMM_SERVER_CHANNEL = 17U,
    BLUETOOTH_CLASSIC_SDP_BUFFER_BYTES = 180U,
    BLUETOOTH_CLASSIC_INQUIRY_DURATION = 4U,
    BLUETOOTH_CLASSIC_DEVICE_CLASS = 0x240404U,
};

typedef struct {
    bool valid;
    uint8_t peer_id;
    bd_addr_t address;
    uint16_t rfcomm_cid;
    uint16_t rfcomm_mtu;
    uint8_t rfcomm_channel;
    bool sdp_query_needed;
    bool connect_requested;
} bluetooth_classic_backend_peer_t;

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
    uint8_t spp_service_buffer[BLUETOOTH_CLASSIC_SDP_BUFFER_BYTES];
    btstack_packet_callback_registration_t event_registration;
    bluetooth_classic_backend_peer_t peers[INTERCOM_MAX_PEERS];
} bluetooth_classic_backend_state_t;

static bluetooth_classic_backend_state_t bluetooth_classic_backend;
static bluetooth_classic_stack_t *bluetooth_classic_active_stack = NULL;

static uint8_t bluetooth_classic_peer_id_from_address(const bd_addr_t address) {
    uint8_t crc = 0U;
    for (size_t index = 0; index < 6U; ++index) {
        crc ^= address[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x80U) != 0U ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    return (uint8_t)((crc % 250U) + 1U);
}

static bluetooth_classic_backend_peer_t *bluetooth_classic_backend_peer_by_id(uint8_t peer_id,
                                                                               bool create) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_classic_backend_peer_t *peer = &bluetooth_classic_backend.peers[index];
        if (peer->valid && peer->peer_id == peer_id) {
            return peer;
        }
    }

    if (!create) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_classic_backend_peer_t *peer = &bluetooth_classic_backend.peers[index];
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

static bluetooth_classic_backend_peer_t *bluetooth_classic_backend_peer_by_address(
    const bd_addr_t address, bool create) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_classic_backend_peer_t *peer = &bluetooth_classic_backend.peers[index];
        if (peer->valid && memcmp(peer->address, address, sizeof(peer->address)) == 0) {
            return peer;
        }
    }

    if (!create) {
        return NULL;
    }

    const uint8_t peer_id = bluetooth_classic_peer_id_from_address(address);
    bluetooth_classic_backend_peer_t *peer = bluetooth_classic_backend_peer_by_id(peer_id, true);
    if (peer == NULL) {
        return NULL;
    }
    memcpy(peer->address, address, sizeof(peer->address));
    return peer;
}

static bluetooth_classic_backend_peer_t *bluetooth_classic_backend_peer_by_cid(uint16_t rfcomm_cid) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_classic_backend_peer_t *peer = &bluetooth_classic_backend.peers[index];
        if (peer->valid && peer->rfcomm_cid == rfcomm_cid) {
            return peer;
        }
    }

    return NULL;
}

static bluetooth_transport_peer_info_t *bluetooth_classic_report_backend_peer(
    bluetooth_classic_stack_t *stack, const bd_addr_t address, const char *name, bool audio_ready) {
    if (stack == NULL) {
        return NULL;
    }

    const uint8_t peer_id = bluetooth_classic_peer_id_from_address(address);
    bluetooth_classic_backend_peer_t *backend_peer =
        bluetooth_classic_backend_peer_by_address(address, true);
    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(stack, peer_id, true);
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
        bluetooth_classic_make_peer_name(peer->name, sizeof(peer->name), peer_id);
    }
    if (bluetooth_classic_peer_is_remembered(stack, peer_id)) {
        peer->paired = true;
    }
    return peer;
}

static void bluetooth_classic_backend_start_inquiry(void) {
    if (bluetooth_classic_active_stack == NULL || !bluetooth_classic_active_stack->enabled ||
        bluetooth_classic_backend.inquiry_active || bluetooth_classic_backend.sdp_query_active) {
        return;
    }

    if (gap_inquiry_start(BLUETOOTH_CLASSIC_INQUIRY_DURATION) == ERROR_CODE_SUCCESS) {
        bluetooth_classic_backend.inquiry_active = true;
    }
}

static void bluetooth_classic_backend_note_channel(uint8_t peer_id, uint8_t rfcomm_channel,
                                                   const char *name) {
    bluetooth_classic_backend_peer_t *backend_peer =
        bluetooth_classic_backend_peer_by_id(peer_id, true);
    if (backend_peer == NULL || bluetooth_classic_active_stack == NULL) {
        return;
    }

    backend_peer->rfcomm_channel = rfcomm_channel;
    backend_peer->sdp_query_needed = false;
    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(bluetooth_classic_active_stack,
                                                                       peer_id, true);
    if (peer != NULL) {
        peer->audio_ready = rfcomm_channel != 0U;
        peer->last_seen_ms = bluetooth_classic_active_stack->transport.last_poll_ms;
        if (name != NULL && name[0] != '\0') {
            (void)snprintf(peer->name, sizeof(peer->name), "%s", name);
        }
    }
}

static void bluetooth_classic_backend_request_sdp_query(uint8_t peer_id) {
    bluetooth_classic_backend_peer_t *backend_peer =
        bluetooth_classic_backend_peer_by_id(peer_id, false);
    if (backend_peer == NULL || !backend_peer->valid) {
        return;
    }
    backend_peer->sdp_query_needed = true;
}

static void bluetooth_classic_backend_maybe_start_sdp_query(void) {
    if (bluetooth_classic_active_stack == NULL || bluetooth_classic_backend.sdp_query_active) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_classic_backend_peer_t *backend_peer = &bluetooth_classic_backend.peers[index];
        if (!backend_peer->valid || !backend_peer->sdp_query_needed) {
            continue;
        }

        if (sdp_client_query_rfcomm_channel_and_name_for_uuid(
                bluetooth_classic_backend_packet_handler, backend_peer->address,
                BLUETOOTH_SERVICE_CLASS_SERIAL_PORT) != ERROR_CODE_SUCCESS) {
            continue;
        }

        bluetooth_classic_backend.sdp_query_active = true;
        bluetooth_classic_backend.sdp_query_peer_id = backend_peer->peer_id;
        backend_peer->sdp_query_needed = false;
        return;
    }
}

static void bluetooth_classic_backend_maybe_start_connect(void) {
    if (bluetooth_classic_active_stack == NULL || bluetooth_classic_backend.connect_peer_id != 0U) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_classic_backend_peer_t *backend_peer = &bluetooth_classic_backend.peers[index];
        if (!backend_peer->valid || !backend_peer->connect_requested || backend_peer->rfcomm_cid != 0U ||
            backend_peer->rfcomm_channel == 0U) {
            continue;
        }

        if (rfcomm_create_channel(bluetooth_classic_backend_packet_handler,
                                  backend_peer->address, backend_peer->rfcomm_channel,
                                  &backend_peer->rfcomm_cid) != ERROR_CODE_SUCCESS) {
            continue;
        }

        bluetooth_classic_backend.connect_peer_id = backend_peer->peer_id;
        return;
    }
}

static void bluetooth_classic_backend_maybe_request_send_now(bluetooth_classic_stack_t *stack) {
    if (stack == NULL || stack->outbound_packet_count == 0U || bluetooth_classic_backend.can_send_pending) {
        return;
    }

    for (size_t index = 0; index < stack->outbound_packet_count; ++index) {
        bluetooth_classic_packet_t *packet = &stack->outbound_queue[index];
        bluetooth_classic_backend_peer_t *backend_peer =
            bluetooth_classic_backend_peer_by_id(packet->target_peer, false);
        if (backend_peer == NULL || backend_peer->rfcomm_cid == 0U) {
            continue;
        }

        if (rfcomm_request_can_send_now_event(backend_peer->rfcomm_cid) == ERROR_CODE_SUCCESS) {
            bluetooth_classic_backend.can_send_pending = true;
            bluetooth_classic_backend.pending_send_cid = backend_peer->rfcomm_cid;
        }
        return;
    }
}

static void bluetooth_classic_backend_maybe_autoreconnect(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &stack->transport.discovered_peers[index];
        if (!peer->valid || !peer->audio_ready || bluetooth_classic_has_connected_peer(stack, peer->peer_id)) {
            continue;
        }
        if (!peer->pairing_pending && !bluetooth_classic_peer_is_remembered(stack, peer->peer_id)) {
            continue;
        }

        bluetooth_classic_backend_peer_t *backend_peer =
            bluetooth_classic_backend_peer_by_id(peer->peer_id, false);
        if (backend_peer == NULL) {
            continue;
        }

        if (backend_peer->rfcomm_channel == 0U) {
            bluetooth_classic_backend_request_sdp_query(peer->peer_id);
            continue;
        }

        backend_peer->connect_requested = true;
    }
}

static void bluetooth_classic_backend_handle_can_send_now(bluetooth_classic_stack_t *stack,
                                                          uint16_t rfcomm_cid) {
    if (stack == NULL) {
        return;
    }

    bluetooth_classic_backend.can_send_pending = false;
    bluetooth_classic_backend.pending_send_cid = 0U;

    for (size_t index = 0; index < stack->outbound_packet_count; ++index) {
        const bluetooth_classic_packet_t packet = stack->outbound_queue[index];
        bluetooth_classic_backend_peer_t *backend_peer =
            bluetooth_classic_backend_peer_by_id(packet.target_peer, false);
        if (backend_peer == NULL || backend_peer->rfcomm_cid != rfcomm_cid) {
            bluetooth_classic_packet_t dropped_packet = {0};
            (void)bluetooth_classic_remove_outbound_at(stack, index, &dropped_packet);
            stack->transport.packets_dropped++;
            index--;
            continue;
        }

        if (rfcomm_send(rfcomm_cid, packet.payload, (uint16_t)packet.payload_len) !=
            ERROR_CODE_SUCCESS) {
            stack->transport.packets_dropped++;
            return;
        }

        bluetooth_classic_packet_t delivered_packet = {0};
        (void)bluetooth_classic_remove_outbound_at(stack, index, &delivered_packet);
        stack->transport.packets_delivered++;
        stack->transport.last_source_peer = packet.source_peer;
        stack->transport.last_target_peer = packet.target_peer;
        if (stack->outbound_packet_count > 0U) {
            bluetooth_classic_backend_maybe_request_send_now(stack);
        }
        return;
    }
}

static void bluetooth_classic_backend_packet_handler(uint8_t packet_type, uint16_t channel,
                                                     uint8_t *packet, uint16_t size) {
    UNUSED(channel);

    bluetooth_classic_stack_t *stack = bluetooth_classic_active_stack;
    if (stack == NULL) {
        return;
    }

    bd_addr_t event_addr;
    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bluetooth_classic_set_transport_online(stack, true);
                bluetooth_classic_backend.powered_on = true;
                gap_connectable_control(1);
                gap_discoverable_control(1);
                bluetooth_classic_backend_start_inquiry();
            }
            break;
        case GAP_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE:
            gap_event_inquiry_result_get_bd_addr(packet, event_addr);
            if (memcmp(event_addr, (bd_addr_t){0}, sizeof(event_addr)) == 0) {
                break;
            }
            if (bluetooth_classic_report_backend_peer(stack, event_addr, NULL, false) != NULL) {
                bluetooth_classic_backend_request_sdp_query(
                    bluetooth_classic_peer_id_from_address(event_addr));
            }
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE:
            bluetooth_classic_backend.inquiry_active = false;
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            gap_pin_code_negative(event_addr);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            gap_ssp_confirmation_response(event_addr);
            break;
        case RFCOMM_EVENT_INCOMING_CONNECTION: {
            rfcomm_event_incoming_connection_get_bd_addr(packet, event_addr);
            bluetooth_classic_backend_peer_t *backend_peer =
                bluetooth_classic_backend_peer_by_address(event_addr, true);
            if (backend_peer != NULL) {
                backend_peer->peer_id = bluetooth_classic_peer_id_from_address(event_addr);
                backend_peer->rfcomm_channel =
                    rfcomm_event_incoming_connection_get_server_channel(packet);
                backend_peer->rfcomm_cid =
                    rfcomm_event_incoming_connection_get_rfcomm_cid(packet);
                backend_peer->connect_requested = true;
                (void)bluetooth_classic_report_backend_peer(stack, event_addr, NULL, true);
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
            bluetooth_classic_backend.connect_peer_id = 0U;
            if (status != ERROR_CODE_SUCCESS) {
                stack->transport.error = true;
                break;
            }

            bluetooth_classic_backend_peer_t *backend_peer =
                bluetooth_classic_backend_peer_by_address(event_addr, true);
            if (backend_peer == NULL) {
                break;
            }

            backend_peer->peer_id = bluetooth_classic_peer_id_from_address(event_addr);
            backend_peer->rfcomm_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            backend_peer->rfcomm_mtu = rfcomm_event_channel_opened_get_max_frame_size(packet);
            backend_peer->connect_requested = false;
            (void)bluetooth_classic_report_backend_peer(stack, event_addr, NULL, true);
            bluetooth_classic_mark_connected(stack, backend_peer->peer_id);
            stack->paired_peer_id = backend_peer->peer_id;
            break;
        }
        case RFCOMM_EVENT_CAN_SEND_NOW:
            bluetooth_classic_backend_handle_can_send_now(
                stack, rfcomm_event_can_send_now_get_rfcomm_cid(packet));
            break;
        case RFCOMM_EVENT_CHANNEL_CLOSED: {
            bluetooth_classic_backend.can_send_pending = false;
            bluetooth_classic_backend.pending_send_cid = 0U;
            bluetooth_classic_backend_peer_t *backend_peer =
                bluetooth_classic_backend_peer_by_cid(
                    rfcomm_event_channel_closed_get_rfcomm_cid(packet));
            if (backend_peer == NULL) {
                break;
            }
            bluetooth_classic_mark_disconnected(stack, backend_peer->peer_id);
            backend_peer->rfcomm_cid = 0U;
            backend_peer->rfcomm_mtu = 0U;
            backend_peer->connect_requested = bluetooth_classic_peer_is_remembered(stack,
                                                                                   backend_peer->peer_id);
            backend_peer->sdp_query_needed = backend_peer->connect_requested;
            break;
        }
        case SDP_EVENT_QUERY_RFCOMM_SERVICE:
            bluetooth_classic_backend_note_channel(
                bluetooth_classic_backend.sdp_query_peer_id,
                sdp_event_query_rfcomm_service_get_rfcomm_channel(packet),
                sdp_event_query_rfcomm_service_get_name(packet));
            break;
        case SDP_EVENT_QUERY_COMPLETE:
            bluetooth_classic_backend.sdp_query_active = false;
            if (bluetooth_classic_backend.sdp_query_peer_id != 0U) {
                bluetooth_classic_backend_peer_t *backend_peer =
                    bluetooth_classic_backend_peer_by_id(
                        bluetooth_classic_backend.sdp_query_peer_id, false);
                if (backend_peer != NULL && backend_peer->rfcomm_channel == 0U) {
                    backend_peer->sdp_query_needed = true;
                }
            }
            bluetooth_classic_backend.sdp_query_peer_id = 0U;
            break;
        default:
            break;
        }
        break;
    case RFCOMM_DATA_PACKET: {
        bluetooth_classic_backend_peer_t *backend_peer =
            bluetooth_classic_backend_peer_by_cid(channel);
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

static bool bluetooth_classic_backend_init(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return false;
    }

    if (!bluetooth_classic_backend.initialized) {
        memset(&bluetooth_classic_backend, 0, sizeof(bluetooth_classic_backend));
        l2cap_init();
        rfcomm_init();
        sdp_init();

        memset(bluetooth_classic_backend.spp_service_buffer, 0,
               sizeof(bluetooth_classic_backend.spp_service_buffer));
        spp_create_sdp_record(bluetooth_classic_backend.spp_service_buffer,
                              sdp_create_service_record_handle(),
                              BLUETOOTH_CLASSIC_RFCOMM_SERVER_CHANNEL, "Pico Intercom");
        sdp_register_service(bluetooth_classic_backend.spp_service_buffer);
        rfcomm_register_service(bluetooth_classic_backend_packet_handler,
                                BLUETOOTH_CLASSIC_RFCOMM_SERVER_CHANNEL, 0xffff);
        bluetooth_classic_backend.event_registration.callback =
            &bluetooth_classic_backend_packet_handler;
        hci_add_event_handler(&bluetooth_classic_backend.event_registration);
        gap_set_class_of_device(BLUETOOTH_CLASSIC_DEVICE_CLASS);
        gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
        gap_set_local_name("Pico Intercom 00:00:00:00:00:00");
        bluetooth_classic_backend.initialized = true;
        bluetooth_classic_backend.service_registered = true;
    }

    bluetooth_classic_active_stack = stack;
    return true;
}
#endif

void bluetooth_classic_stack_init(bluetooth_classic_stack_t *stack) {
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
    bluetooth_classic_set_transport_online(stack, false);
#else
    bluetooth_classic_set_transport_online(stack, true);
#endif
}

bool bluetooth_classic_stack_set_enabled(bluetooth_classic_stack_t *stack, bool enabled) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->enabled = enabled;
    stack->discoverable = enabled;
    stack->pairing_enabled = enabled;

#if defined(PICO_INTERCOM_TARGET)
    (void)bluetooth_transport_set_enabled(&stack->transport, enabled);
    if (enabled) {
        if (!bluetooth_classic_backend_init(stack)) {
            return false;
        }
        bluetooth_classic_active_stack = stack;
        gap_discoverable_control(1);
        gap_connectable_control(1);
        if (!bluetooth_classic_backend.powered_on) {
            hci_power_control(HCI_POWER_ON);
        }
    } else {
        stack->outbound_packet_count = 0U;
        bluetooth_classic_set_transport_online(stack, false);
        bluetooth_classic_backend.powered_on = false;
        bluetooth_classic_backend.inquiry_active = false;
        bluetooth_classic_backend.sdp_query_active = false;
        bluetooth_classic_backend.can_send_pending = false;
        bluetooth_classic_backend.connect_peer_id = 0U;
        bluetooth_classic_backend.sdp_query_peer_id = 0U;
        hci_power_control(HCI_POWER_OFF);
        while (stack->transport.connected_peer_count > 0U) {
            bluetooth_classic_mark_disconnected(stack, stack->transport.connected_peers[0]);
        }
    }
#else
    if (!bluetooth_transport_set_enabled(&stack->transport, enabled)) {
        return false;
    }
    bluetooth_classic_set_transport_online(stack, enabled);
#endif

    stack->connected = enabled && stack->transport.connected_peer_count > 0U;
    return true;
}

void bluetooth_classic_stack_set_local_peer_id(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL) {
        return;
    }

    bluetooth_transport_set_local_peer_id(&stack->transport, peer_id);
}

uint8_t bluetooth_classic_stack_local_peer_id(const bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return 1U;
    }

    return bluetooth_transport_local_peer_id(&stack->transport);
}

bool bluetooth_classic_stack_pair(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->discoverable = true;
    stack->pairing_enabled = true;
    stack->paired_peer_id = peer_id;
#if defined(PICO_INTERCOM_TARGET)
    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(stack, peer_id, true);
    if (peer == NULL) {
        return false;
    }
    peer->pairing_pending = true;
    if (!bluetooth_classic_remember_peer(stack, peer_id)) {
        return false;
    }
    bluetooth_classic_backend_peer_t *backend_peer =
        bluetooth_classic_backend_peer_by_id(peer_id, true);
    if (backend_peer != NULL) {
        backend_peer->connect_requested = true;
        backend_peer->sdp_query_needed = backend_peer->rfcomm_channel == 0U;
    }
    return true;
#else
    return bluetooth_classic_stack_connect(stack, peer_id);
#endif
}

bool bluetooth_classic_stack_connect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack) || !stack->enabled || peer_id == 0U) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(stack, peer_id, true);
    if (peer == NULL) {
        return false;
    }
    bluetooth_classic_backend_peer_t *backend_peer =
        bluetooth_classic_backend_peer_by_id(peer_id, true);
    if (backend_peer == NULL) {
        return false;
    }
    backend_peer->connect_requested = true;
    backend_peer->sdp_query_needed = backend_peer->rfcomm_channel == 0U;
    peer->pairing_pending = true;
    stack->paired_peer_id = peer_id;
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

bool bluetooth_classic_stack_disconnect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_classic_backend_peer_t *backend_peer =
        bluetooth_classic_backend_peer_by_id(peer_id, false);
    if (backend_peer == NULL) {
        return false;
    }
    backend_peer->connect_requested = false;
    backend_peer->sdp_query_needed = false;
    if (backend_peer->rfcomm_cid != 0U) {
        (void)rfcomm_disconnect(backend_peer->rfcomm_cid);
    }
    bluetooth_classic_mark_disconnected(stack, peer_id);
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

bool bluetooth_classic_stack_restore_pairing(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    if (!bluetooth_classic_remember_peer(stack, peer_id)) {
        return false;
    }
    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(stack, peer_id, true);
    if (peer != NULL) {
        peer->paired = true;
        peer->pairing_pending = false;
    }
    bluetooth_classic_backend_peer_t *backend_peer =
        bluetooth_classic_backend_peer_by_id(peer_id, true);
    if (backend_peer != NULL) {
        backend_peer->connect_requested = false;
        backend_peer->sdp_query_needed = true;
    }
    return true;
#else
    return bluetooth_transport_restore_pairing(&stack->transport, peer_id);
#endif
}

bool bluetooth_classic_stack_poll(bluetooth_classic_stack_t *stack) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->transport.last_poll_ms =
#if defined(PICO_INTERCOM_TARGET)
        bluetooth_classic_now_ms();
#else
        stack->transport.last_poll_ms + 100U;
#endif

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_classic_active_stack = stack;
    bluetooth_classic_backend_maybe_autoreconnect(stack);
    bluetooth_classic_backend_maybe_start_sdp_query();
    bluetooth_classic_backend_maybe_start_connect();
    bluetooth_classic_backend_maybe_request_send_now(stack);
    if (!bluetooth_classic_backend.inquiry_active &&
        stack->transport.connected_peer_count < INTERCOM_MAX_PEERS) {
        bluetooth_classic_backend_start_inquiry();
    }
    stack->connected = stack->transport.connected_peer_count > 0U;
    return stack->enabled;
#else
    const bool polled = bluetooth_transport_poll(&stack->transport);
    stack->connected = stack->transport.connected_peer_count > 0U;
    return polled;
#endif
}

bool bluetooth_classic_stack_report_headset(bluetooth_classic_stack_t *stack, uint8_t peer_id,
                                            const char *name, bool audio_ready) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_transport_peer_info_t *peer = bluetooth_classic_get_peer(stack, peer_id, true);
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
        bluetooth_classic_backend_peer_t *backend_peer =
            bluetooth_classic_backend_peer_by_id(peer_id, true);
        if (backend_peer != NULL && memcmp(backend_peer->address, (bd_addr_t){0}, 6U) != 0) {
            backend_peer->sdp_query_needed = backend_peer->rfcomm_channel == 0U;
        }
    }
    if (previous_audio_ready != audio_ready) {
        printf("Bluetooth Classic headset %u %s audio transport is %s.\n", (unsigned)peer_id,
               name != NULL && name[0] != '\0' ? name : "peer",
               audio_ready ? "ready" : "negotiating");
    }
    return true;
#else
    const bool reported =
        bluetooth_transport_report_peer(&stack->transport, peer_id, name, audio_ready);
    if (reported) {
        printf("Bluetooth Classic headset %u %s audio transport is %s.\n",
               (unsigned)peer_id, name != NULL && name[0] != '\0' ? name : "peer",
               audio_ready ? "ready" : "negotiating");
    }
    return reported;
#endif
}

bool bluetooth_classic_stack_select_pairing_candidate(const bluetooth_classic_stack_t *stack,
                                                      uint8_t *peer_id) {
    if (stack == NULL || peer_id == NULL) {
        return false;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        const bluetooth_transport_peer_info_t *peer = &stack->transport.discovered_peers[index];
        if (!peer->valid || peer->peer_id == 0U || peer->pairing_pending || !peer->audio_ready ||
            bluetooth_classic_has_connected_peer(stack, peer->peer_id)) {
            continue;
        }

        *peer_id = peer->peer_id;
        return true;
    }

    return false;
}

bool bluetooth_classic_stack_queue_packet(bluetooth_classic_stack_t *stack, uint8_t source_peer,
                                          uint8_t target_peer, const uint8_t *payload,
                                          size_t payload_len) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    if (!stack->enabled || payload == NULL || payload_len == 0U ||
        !bluetooth_classic_has_connected_peer(stack, target_peer)) {
        return false;
    }
    return bluetooth_classic_enqueue_outbound(stack, source_peer, target_peer, payload, payload_len);
#else
    return bluetooth_transport_queue_packet(&stack->transport, source_peer, target_peer, payload,
                                            payload_len);
#endif
}

bool bluetooth_classic_stack_dequeue_packet(bluetooth_classic_stack_t *stack,
                                            bluetooth_classic_packet_t *packet) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    return bluetooth_transport_dequeue_packet(&stack->transport, packet);
}

size_t bluetooth_classic_stack_pending_count(const bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return 0U;
    }

#if defined(PICO_INTERCOM_TARGET)
    return bluetooth_transport_pending_count(&stack->transport) + stack->outbound_packet_count;
#else
    return bluetooth_transport_pending_count(&stack->transport);
#endif
}

const char *bluetooth_classic_stack_state_name(const bluetooth_classic_stack_t *stack) {
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
