#include "bluetooth_headset_profile.h"

#if defined(PICO_INTERCOM_TARGET)
#include "btstack.h"
#include "classic/hfp_ag.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#endif

#include <stdio.h>
#include <string.h>

#if defined(PICO_INTERCOM_TARGET)

enum {
    BLUETOOTH_HEADSET_PROFILE_HFP_RFCOMM_CHANNEL = 1U,
    BLUETOOTH_HEADSET_PROFILE_INQUIRY_DURATION = 4U,
    BLUETOOTH_HEADSET_PROFILE_SERVICE_BUFFER_BYTES = 256U,
    BLUETOOTH_HEADSET_PROFILE_DEVICE_CLASS = 0x240404U,
};

typedef struct {
    bool valid;
    bool connect_requested;
    bool reconnect_blocked;
    bool pairing_pending;
    uint8_t peer_id;
    bd_addr_t address;
    hci_con_handle_t acl_handle;
    hci_con_handle_t sco_handle;
} bluetooth_headset_backend_peer_t;

typedef struct {
    bool initialized;
    bool powered_on;
    bool inquiry_active;
    bool can_send_pending;
    uint16_t supported_features;
    uint8_t service_buffer[BLUETOOTH_HEADSET_PROFILE_SERVICE_BUFFER_BYTES];
    btstack_packet_callback_registration_t event_registration;
    bluetooth_headset_backend_peer_t peers[INTERCOM_MAX_PEERS];
} bluetooth_headset_backend_t;

static bluetooth_headset_backend_t bluetooth_headset_backend;
static bluetooth_classic_stack_t *bluetooth_headset_active_stack = NULL;

static const uint8_t bluetooth_headset_codecs[] = {HFP_CODEC_CVSD};

static void bluetooth_headset_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                                             uint16_t size);

static uint32_t bluetooth_headset_now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void bluetooth_headset_set_transport_online(bluetooth_classic_stack_t *stack, bool enabled) {
    if (stack == NULL) {
        return;
    }
    stack->transport.backend_ready = enabled;
    stack->transport.network_connected = enabled;
}

static void bluetooth_headset_refresh_connected_flag(bluetooth_classic_stack_t *stack) {
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

static size_t bluetooth_headset_find_connected_index(const bluetooth_classic_stack_t *stack,
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

static bool bluetooth_headset_has_connected_peer(const bluetooth_classic_stack_t *stack,
                                                 uint8_t peer_id) {
    return bluetooth_headset_find_connected_index(stack, peer_id) <
           (stack != NULL ? stack->transport.connected_peer_count : 0U);
}

static bluetooth_transport_peer_info_t *bluetooth_headset_get_transport_peer(
    bluetooth_classic_stack_t *stack, uint8_t peer_id, bool create) {
    if (stack == NULL || peer_id == 0U || peer_id == stack->transport.local_peer_id) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &stack->transport.discovered_peers[index];
        if (peer->valid && peer->peer_id == peer_id) {
            return peer;
        }
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
        snprintf(peer->name, sizeof(peer->name), "headset-%u", (unsigned)peer_id);
        if (stack->transport.discovered_peer_count < INTERCOM_MAX_PEERS) {
            stack->transport.discovered_peer_count++;
        }
        return peer;
    }
    return NULL;
}

static bool bluetooth_headset_remember_peer(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
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

static bool bluetooth_headset_peer_is_remembered(const bluetooth_classic_stack_t *stack,
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

static size_t bluetooth_headset_reserve_connected_slot(bluetooth_classic_stack_t *stack,
                                                       uint8_t peer_id) {
    const size_t connected_index = bluetooth_headset_find_connected_index(stack, peer_id);
    if (connected_index < stack->transport.connected_peer_count) {
        return connected_index;
    }
    if (stack == NULL || stack->transport.connected_peer_count >= INTERCOM_MAX_PEERS) {
        if (stack != NULL) {
            stack->transport.error = true;
        }
        return INTERCOM_MAX_PEERS;
    }
    const size_t slot = stack->transport.connected_peer_count++;
    stack->transport.connected_peers[slot] = peer_id;
    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
    return slot;
}

static void bluetooth_headset_mark_connecting(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, peer_id, true);
    const size_t slot = bluetooth_headset_reserve_connected_slot(stack, peer_id);
    if (peer == NULL || slot >= INTERCOM_MAX_PEERS) {
        return;
    }
    peer->pairing_pending = true;
    peer->disconnect_requested = false;
    peer->reconnect_blocked = false;
    peer->last_state_change_ms = stack->transport.last_poll_ms;
    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_CONNECTING;
    bluetooth_headset_refresh_connected_flag(stack);
}

static void bluetooth_headset_mark_connected(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, peer_id, true);
    const size_t slot = bluetooth_headset_reserve_connected_slot(stack, peer_id);
    if (peer == NULL || slot >= INTERCOM_MAX_PEERS) {
        return;
    }
    peer->paired = true;
    peer->pairing_pending = false;
    peer->disconnect_requested = false;
    peer->reconnect_blocked = false;
    peer->audio_ready = true;
    peer->last_connected_ms = stack->transport.last_poll_ms;
    peer->last_state_change_ms = stack->transport.last_poll_ms;
    stack->transport.peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
    bluetooth_headset_refresh_connected_flag(stack);
}

static void bluetooth_headset_mark_disconnected(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    const size_t connected_index = bluetooth_headset_find_connected_index(stack, peer_id);
    if (stack != NULL && connected_index < stack->transport.connected_peer_count) {
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

    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, peer_id, false);
    if (peer != NULL) {
        peer->pairing_pending = false;
        peer->paired = bluetooth_headset_peer_is_remembered(stack, peer_id);
        peer->audio_ready = false;
        peer->last_disconnected_ms = stack->transport.last_poll_ms;
        peer->last_state_change_ms = stack->transport.last_poll_ms;
        peer->reconnect_attempts++;
    }
    bluetooth_headset_refresh_connected_flag(stack);
}

static bluetooth_headset_backend_peer_t *bluetooth_headset_backend_peer_by_id(uint8_t peer_id,
                                                                              bool create) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_headset_backend_peer_t *peer = &bluetooth_headset_backend.peers[index];
        if (peer->valid && peer->peer_id == peer_id) {
            return peer;
        }
    }
    if (!create) {
        return NULL;
    }
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_headset_backend_peer_t *peer = &bluetooth_headset_backend.peers[index];
        if (peer->valid) {
            continue;
        }
        memset(peer, 0, sizeof(*peer));
        peer->valid = true;
        peer->peer_id = peer_id;
        peer->acl_handle = HCI_CON_HANDLE_INVALID;
        peer->sco_handle = HCI_CON_HANDLE_INVALID;
        return peer;
    }
    return NULL;
}

static bluetooth_headset_backend_peer_t *bluetooth_headset_backend_peer_by_address(
    const bd_addr_t address, bool create) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_headset_backend_peer_t *peer = &bluetooth_headset_backend.peers[index];
        if (peer->valid && memcmp(peer->address, address, sizeof(peer->address)) == 0) {
            return peer;
        }
    }
    if (!create || bluetooth_headset_active_stack == NULL) {
        return NULL;
    }
    const uint8_t peer_id =
        (uint8_t)(((address[0] ^ address[1] ^ address[2] ^ address[3] ^ address[4] ^ address[5]) %
                   250U) +
                  1U);
    bluetooth_headset_backend_peer_t *peer = bluetooth_headset_backend_peer_by_id(peer_id, true);
    if (peer != NULL) {
        memcpy(peer->address, address, sizeof(peer->address));
    }
    return peer;
}

static bluetooth_headset_backend_peer_t *bluetooth_headset_backend_peer_by_acl(
    hci_con_handle_t acl_handle) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_headset_backend_peer_t *peer = &bluetooth_headset_backend.peers[index];
        if (peer->valid && peer->acl_handle == acl_handle) {
            return peer;
        }
    }
    return NULL;
}

static bluetooth_headset_backend_peer_t *bluetooth_headset_backend_peer_by_sco(
    hci_con_handle_t sco_handle) {
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_headset_backend_peer_t *peer = &bluetooth_headset_backend.peers[index];
        if (peer->valid && peer->sco_handle == sco_handle) {
            return peer;
        }
    }
    return NULL;
}

static void bluetooth_headset_report_peer(bluetooth_classic_stack_t *stack, const bd_addr_t address,
                                          const char *name, bool audio_ready) {
    bluetooth_headset_backend_peer_t *backend_peer =
        bluetooth_headset_backend_peer_by_address(address, true);
    if (backend_peer == NULL) {
        return;
    }
    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, backend_peer->peer_id, true);
    if (peer == NULL) {
        return;
    }
    memcpy(peer->address, address, sizeof(peer->address));
    peer->audio_ready = audio_ready;
    peer->last_seen_ms = stack->transport.last_poll_ms;
    if (name != NULL && name[0] != '\0') {
        snprintf(peer->name, sizeof(peer->name), "%s", name);
    }
    if (bluetooth_headset_peer_is_remembered(stack, peer->peer_id)) {
        peer->paired = true;
    }
}

static void bluetooth_headset_start_inquiry(void) {
    if (bluetooth_headset_active_stack == NULL || !bluetooth_headset_active_stack->enabled ||
        bluetooth_headset_backend.inquiry_active) {
        return;
    }
    if (gap_inquiry_start(BLUETOOTH_HEADSET_PROFILE_INQUIRY_DURATION) == ERROR_CODE_SUCCESS) {
        bluetooth_headset_backend.inquiry_active = true;
    }
}

static void bluetooth_headset_maybe_start_connect(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return;
    }
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_headset_backend_peer_t *backend_peer = &bluetooth_headset_backend.peers[index];
        if (!backend_peer->valid || !backend_peer->connect_requested ||
            memcmp(backend_peer->address, (bd_addr_t){0}, sizeof(backend_peer->address)) == 0 ||
            backend_peer->acl_handle != HCI_CON_HANDLE_INVALID) {
            continue;
        }

        bluetooth_transport_peer_info_t *peer =
            bluetooth_headset_get_transport_peer(stack, backend_peer->peer_id, false);
        if (peer != NULL && peer->last_connect_attempt_ms != 0U &&
            (stack->transport.last_poll_ms - peer->last_connect_attempt_ms) <
                BLUETOOTH_TRANSPORT_CONNECT_RETRY_MS) {
            continue;
        }

        if (hfp_ag_establish_service_level_connection(backend_peer->address) !=
            ERROR_CODE_SUCCESS) {
            continue;
        }

        if (peer != NULL) {
            peer->connect_attempts++;
            peer->last_connect_attempt_ms = stack->transport.last_poll_ms;
        }
        bluetooth_headset_mark_connecting(stack, backend_peer->peer_id);
        return;
    }
}

static bool bluetooth_headset_remove_outbound_at(bluetooth_classic_stack_t *stack, size_t index,
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

static void bluetooth_headset_maybe_request_send_now(bluetooth_classic_stack_t *stack) {
    if (stack == NULL || stack->outbound_packet_count == 0U || bluetooth_headset_backend.can_send_pending) {
        return;
    }

    for (size_t index = 0; index < stack->outbound_packet_count; ++index) {
        bluetooth_classic_packet_t *packet = &stack->outbound_queue[index];
        bluetooth_headset_backend_peer_t *backend_peer =
            bluetooth_headset_backend_peer_by_id(packet->target_peer, false);
        if (backend_peer == NULL || backend_peer->sco_handle == HCI_CON_HANDLE_INVALID) {
            continue;
        }

        bluetooth_headset_backend.can_send_pending = true;
        hci_request_sco_can_send_now_event_for_con_handle(backend_peer->sco_handle);
        return;
    }
}

static void bluetooth_headset_handle_can_send_now(bluetooth_classic_stack_t *stack,
                                                  hci_con_handle_t sco_handle) {
    bluetooth_headset_backend.can_send_pending = false;
    if (stack == NULL) {
        return;
    }

    const int sco_packet_length = hci_get_sco_packet_length_for_connection(sco_handle);
    if (sco_packet_length <= 3) {
        return;
    }
    const size_t sco_payload_length = (size_t)sco_packet_length - 3U;

    for (size_t index = 0; index < stack->outbound_packet_count; ++index) {
        const bluetooth_classic_packet_t packet = stack->outbound_queue[index];
        bluetooth_headset_backend_peer_t *backend_peer =
            bluetooth_headset_backend_peer_by_id(packet.target_peer, false);
        if (backend_peer == NULL || backend_peer->sco_handle != sco_handle) {
            continue;
        }

        hci_reserve_packet_buffer();
        uint8_t *sco_packet = hci_get_outgoing_packet_buffer();
        const size_t copy_len = packet.payload_len < sco_payload_length ? packet.payload_len : sco_payload_length;
        memset(&sco_packet[3], 0, sco_payload_length);
        memcpy(&sco_packet[3], packet.payload, copy_len);
        little_endian_store_16(sco_packet, 0, sco_handle);
        sco_packet[2] = (uint8_t)sco_payload_length;
        if (hci_send_sco_packet_buffer(sco_packet_length) != ERROR_CODE_SUCCESS) {
            stack->transport.error = true;
            return;
        }

        bluetooth_classic_packet_t delivered = {0};
        (void)bluetooth_headset_remove_outbound_at(stack, index, &delivered);
        stack->transport.packets_delivered++;
        stack->transport.last_source_peer = packet.source_peer;
        stack->transport.last_target_peer = packet.target_peer;
        bluetooth_headset_maybe_request_send_now(stack);
        return;
    }
}

static void bluetooth_headset_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                                             uint16_t size) {
    UNUSED(channel);
    bluetooth_classic_stack_t *stack = bluetooth_headset_active_stack;
    if (stack == NULL) {
        return;
    }

    bd_addr_t addr;
    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bluetooth_headset_backend.powered_on = true;
                bluetooth_headset_set_transport_online(stack, true);
                gap_connectable_control(1);
                gap_discoverable_control(1);
                bluetooth_headset_start_inquiry();
            }
            break;
        case GAP_EVENT_INQUIRY_RESULT: {
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            char name_buffer[BLUETOOTH_TRANSPORT_PEER_NAME_LEN] = {0};
            const char *name = NULL;
            if (gap_event_inquiry_result_get_name_available(packet)) {
                int name_len = gap_event_inquiry_result_get_name_len(packet);
                if (name_len > (int)(sizeof(name_buffer) - 1U)) {
                    name_len = (int)sizeof(name_buffer) - 1;
                }
                memcpy(name_buffer, gap_event_inquiry_result_get_name(packet), (size_t)name_len);
                name_buffer[name_len] = '\0';
                name = name_buffer;
            }
            bluetooth_headset_report_peer(stack, addr, name, true);
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE:
            bluetooth_headset_backend.inquiry_active = false;
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            gap_ssp_confirmation_response(addr);
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            gap_pin_code_response(addr, "0000");
            break;
        case HCI_EVENT_HFP_META:
            switch (hci_event_hfp_meta_get_subevent_code(packet)) {
            case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED: {
                const uint8_t status =
                    hfp_subevent_service_level_connection_established_get_status(packet);
                hfp_subevent_service_level_connection_established_get_bd_addr(packet, addr);
                bluetooth_headset_backend_peer_t *backend_peer =
                    bluetooth_headset_backend_peer_by_address(addr, true);
                if (backend_peer == NULL) {
                    break;
                }
                if (status != ERROR_CODE_SUCCESS) {
                    backend_peer->connect_requested = true;
                    bluetooth_headset_mark_disconnected(stack, backend_peer->peer_id);
                    stack->transport.error = true;
                    break;
                }
                backend_peer->acl_handle =
                    hfp_subevent_service_level_connection_established_get_acl_handle(packet);
                backend_peer->connect_requested = false;
                bluetooth_headset_mark_connecting(stack, backend_peer->peer_id);
                (void)hfp_ag_establish_audio_connection(backend_peer->acl_handle);
                break;
            }
            case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED: {
                bluetooth_headset_backend_peer_t *backend_peer =
                    bluetooth_headset_backend_peer_by_acl(
                        hfp_subevent_service_level_connection_released_get_acl_handle(packet));
                if (backend_peer == NULL) {
                    break;
                }
                backend_peer->acl_handle = HCI_CON_HANDLE_INVALID;
                backend_peer->sco_handle = HCI_CON_HANDLE_INVALID;
                backend_peer->connect_requested =
                    !backend_peer->reconnect_blocked &&
                    bluetooth_headset_peer_is_remembered(stack, backend_peer->peer_id);
                bluetooth_headset_mark_disconnected(stack, backend_peer->peer_id);
                break;
            }
            case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
                const uint8_t status =
                    hfp_subevent_audio_connection_established_get_status(packet);
                hfp_subevent_audio_connection_established_get_bd_addr(packet, addr);
                bluetooth_headset_backend_peer_t *backend_peer =
                    bluetooth_headset_backend_peer_by_address(addr, true);
                if (backend_peer == NULL) {
                    break;
                }
                if (status != ERROR_CODE_SUCCESS) {
                    backend_peer->connect_requested = true;
                    stack->transport.error = true;
                    break;
                }
                backend_peer->acl_handle =
                    hfp_subevent_audio_connection_established_get_acl_handle(packet);
                backend_peer->sco_handle =
                    hfp_subevent_audio_connection_established_get_sco_handle(packet);
                bluetooth_headset_mark_connected(stack, backend_peer->peer_id);
                hci_request_sco_can_send_now_event_for_con_handle(backend_peer->sco_handle);
                break;
            }
            case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED: {
                bluetooth_headset_backend_peer_t *backend_peer =
                    bluetooth_headset_backend_peer_by_sco(
                        hfp_subevent_audio_connection_released_get_sco_handle(packet));
                if (backend_peer == NULL) {
                    backend_peer = bluetooth_headset_backend_peer_by_acl(
                        hfp_subevent_audio_connection_released_get_acl_handle(packet));
                }
                if (backend_peer == NULL) {
                    break;
                }
                backend_peer->sco_handle = HCI_CON_HANDLE_INVALID;
                backend_peer->connect_requested =
                    !backend_peer->reconnect_blocked &&
                    bluetooth_headset_peer_is_remembered(stack, backend_peer->peer_id);
                bluetooth_headset_mark_disconnected(stack, backend_peer->peer_id);
                break;
            }
            default:
                break;
            }
            break;
        case HCI_EVENT_SCO_CAN_SEND_NOW:
            bluetooth_headset_handle_can_send_now(
                stack, hci_event_sco_can_send_now_get_handle(packet));
            break;
        default:
            break;
        }
        break;
    case HCI_SCO_DATA_PACKET: {
        const hci_con_handle_t sco_handle = READ_SCO_CONNECTION_HANDLE(packet);
        bluetooth_headset_backend_peer_t *backend_peer =
            bluetooth_headset_backend_peer_by_sco(sco_handle);
        if (backend_peer == NULL || size <= 3U) {
            break;
        }
        (void)bluetooth_transport_queue_packet(&stack->transport, backend_peer->peer_id,
                                               stack->transport.local_peer_id, &packet[3],
                                               size - 3U);
        break;
    }
    default:
        break;
    }
}

static bool bluetooth_headset_backend_init(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return false;
    }
    if (!bluetooth_headset_backend.initialized) {
        memset(&bluetooth_headset_backend, 0, sizeof(bluetooth_headset_backend));
        bluetooth_headset_backend.supported_features =
            (1U << HFP_AGSF_ESCO_S4) | (1U << HFP_AGSF_CODEC_NEGOTIATION) |
            (1U << HFP_AGSF_EC_NR_FUNCTION) | (1U << HFP_AGSF_ABILITY_TO_REJECT_A_CALL);
        l2cap_init();
        rfcomm_init();
        hfp_ag_init(BLUETOOTH_HEADSET_PROFILE_HFP_RFCOMM_CHANNEL);
        hfp_ag_init_supported_features(bluetooth_headset_backend.supported_features);
        hfp_ag_init_codecs((uint8_t)sizeof(bluetooth_headset_codecs), bluetooth_headset_codecs);
        sdp_init();
        memset(bluetooth_headset_backend.service_buffer, 0,
               sizeof(bluetooth_headset_backend.service_buffer));
        hfp_ag_create_sdp_record_with_codecs(
            bluetooth_headset_backend.service_buffer, sdp_create_service_record_handle(),
            BLUETOOTH_HEADSET_PROFILE_HFP_RFCOMM_CHANNEL, "Pico Intercom Audio Gateway", 1,
            bluetooth_headset_backend.supported_features,
            (uint8_t)sizeof(bluetooth_headset_codecs), bluetooth_headset_codecs);
        sdp_register_service(bluetooth_headset_backend.service_buffer);
        bluetooth_headset_backend.event_registration.callback = &bluetooth_headset_packet_handler;
        hci_add_event_handler(&bluetooth_headset_backend.event_registration);
        hci_register_sco_packet_handler(&bluetooth_headset_packet_handler);
        hfp_ag_register_packet_handler(&bluetooth_headset_packet_handler);
        gap_set_class_of_device(BLUETOOTH_HEADSET_PROFILE_DEVICE_CLASS);
        gap_set_local_name("Pico Intercom Audio Gateway 00:00:00:00:00:00");
        gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
        bluetooth_headset_backend.initialized = true;
    }
    bluetooth_headset_active_stack = stack;
    return true;
}

void bluetooth_headset_profile_init(bluetooth_classic_stack_t *stack) {
    bluetooth_headset_set_transport_online(stack, false);
}

bool bluetooth_headset_profile_set_enabled(bluetooth_classic_stack_t *stack, bool enabled) {
    if (stack == NULL) {
        return false;
    }
    if (!bluetooth_transport_set_enabled(&stack->transport, enabled)) {
        return false;
    }
    if (enabled) {
        if (!bluetooth_headset_backend_init(stack)) {
            return false;
        }
        bluetooth_headset_active_stack = stack;
        gap_discoverable_control(1);
        gap_connectable_control(1);
        if (!bluetooth_headset_backend.powered_on) {
            hci_power_control(HCI_POWER_ON);
        }
    } else {
        bluetooth_headset_set_transport_online(stack, false);
        bluetooth_headset_backend.powered_on = false;
        bluetooth_headset_backend.inquiry_active = false;
        bluetooth_headset_backend.can_send_pending = false;
        hci_power_control(HCI_POWER_OFF);
        while (stack->transport.connected_peer_count > 0U) {
            bluetooth_headset_mark_disconnected(stack, stack->transport.connected_peers[0]);
        }
    }
    bluetooth_headset_refresh_connected_flag(stack);
    return true;
}

bool bluetooth_headset_profile_pair(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_headset_remember_peer(stack, peer_id)) {
        return false;
    }
    return bluetooth_headset_profile_connect(stack, peer_id);
}

bool bluetooth_headset_profile_connect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || !stack->enabled || peer_id == 0U) {
        return false;
    }
    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, peer_id, true);
    bluetooth_headset_backend_peer_t *backend_peer =
        bluetooth_headset_backend_peer_by_id(peer_id, true);
    if (peer == NULL || backend_peer == NULL) {
        return false;
    }
    peer->pairing_pending = true;
    peer->disconnect_requested = false;
    peer->reconnect_blocked = false;
    peer->last_seen_ms = stack->transport.last_poll_ms;
    backend_peer->connect_requested = true;
    backend_peer->reconnect_blocked = false;
    backend_peer->pairing_pending = true;
    bluetooth_headset_mark_connecting(stack, peer_id);
    return true;
}

bool bluetooth_headset_profile_disconnect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || peer_id == 0U) {
        return false;
    }
    bluetooth_headset_backend_peer_t *backend_peer =
        bluetooth_headset_backend_peer_by_id(peer_id, false);
    if (backend_peer == NULL) {
        return false;
    }
    backend_peer->connect_requested = false;
    backend_peer->reconnect_blocked = true;
    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, peer_id, false);
    if (peer != NULL) {
        peer->reconnect_blocked = true;
    }
    if (backend_peer->sco_handle != HCI_CON_HANDLE_INVALID) {
        (void)hfp_ag_release_audio_connection(backend_peer->acl_handle);
    }
    if (backend_peer->acl_handle != HCI_CON_HANDLE_INVALID) {
        (void)hfp_ag_release_service_level_connection(backend_peer->acl_handle);
    } else {
        bluetooth_headset_mark_disconnected(stack, peer_id);
    }
    return true;
}

bool bluetooth_headset_profile_restore_pairing(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_headset_remember_peer(stack, peer_id)) {
        return false;
    }
    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, peer_id, true);
    if (peer != NULL) {
        peer->paired = true;
        peer->reconnect_blocked = false;
        peer->pairing_pending = false;
    }
    bluetooth_headset_backend_peer_t *backend_peer =
        bluetooth_headset_backend_peer_by_id(peer_id, true);
    if (backend_peer != NULL) {
        backend_peer->connect_requested = true;
        backend_peer->reconnect_blocked = false;
    }
    return true;
}

bool bluetooth_headset_profile_poll(bluetooth_classic_stack_t *stack) {
    if (stack == NULL || !stack->enabled) {
        return false;
    }
    stack->transport.last_poll_ms = bluetooth_headset_now_ms();
    bluetooth_headset_active_stack = stack;
    cyw43_arch_poll();
    bluetooth_headset_maybe_start_connect(stack);
    bluetooth_headset_maybe_request_send_now(stack);
    if (!bluetooth_headset_backend.inquiry_active &&
        stack->transport.connected_peer_count < INTERCOM_MAX_PEERS) {
        bluetooth_headset_start_inquiry();
    }
    bluetooth_headset_refresh_connected_flag(stack);
    return stack->enabled;
}

bool bluetooth_headset_profile_report_headset(bluetooth_classic_stack_t *stack, uint8_t peer_id,
                                              const char *name, bool audio_ready) {
    bluetooth_transport_peer_info_t *peer =
        bluetooth_headset_get_transport_peer(stack, peer_id, true);
    if (peer == NULL) {
        return false;
    }
    peer->audio_ready = audio_ready;
    peer->last_seen_ms = stack->transport.last_poll_ms;
    if (name != NULL && name[0] != '\0') {
        snprintf(peer->name, sizeof(peer->name), "%s", name);
    }
    return true;
}

bool bluetooth_headset_profile_queue_packet(bluetooth_classic_stack_t *stack, uint8_t source_peer,
                                            uint8_t target_peer, const uint8_t *payload,
                                            size_t payload_len) {
    if (stack == NULL || !stack->enabled || payload == NULL || payload_len == 0U ||
        !bluetooth_headset_has_connected_peer(stack, target_peer)) {
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

size_t bluetooth_headset_profile_pending_count(const bluetooth_classic_stack_t *stack) {
    return stack != NULL ? bluetooth_transport_pending_count(&stack->transport) +
                               stack->outbound_packet_count
                         : 0U;
}

#else

void bluetooth_headset_profile_init(bluetooth_classic_stack_t *stack) { (void)stack; }
bool bluetooth_headset_profile_set_enabled(bluetooth_classic_stack_t *stack, bool enabled) {
    (void)stack;
    (void)enabled;
    return false;
}
bool bluetooth_headset_profile_pair(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    (void)stack;
    (void)peer_id;
    return false;
}
bool bluetooth_headset_profile_connect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    (void)stack;
    (void)peer_id;
    return false;
}
bool bluetooth_headset_profile_disconnect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    (void)stack;
    (void)peer_id;
    return false;
}
bool bluetooth_headset_profile_restore_pairing(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    (void)stack;
    (void)peer_id;
    return false;
}
bool bluetooth_headset_profile_poll(bluetooth_classic_stack_t *stack) {
    (void)stack;
    return false;
}
bool bluetooth_headset_profile_report_headset(bluetooth_classic_stack_t *stack, uint8_t peer_id,
                                              const char *name, bool audio_ready) {
    (void)stack;
    (void)peer_id;
    (void)name;
    (void)audio_ready;
    return false;
}
bool bluetooth_headset_profile_queue_packet(bluetooth_classic_stack_t *stack, uint8_t source_peer,
                                            uint8_t target_peer, const uint8_t *payload,
                                            size_t payload_len) {
    (void)stack;
    (void)source_peer;
    (void)target_peer;
    (void)payload;
    (void)payload_len;
    return false;
}
size_t bluetooth_headset_profile_pending_count(const bluetooth_classic_stack_t *stack) {
    (void)stack;
    return 0U;
}

#endif
