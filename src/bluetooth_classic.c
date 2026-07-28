#include "bluetooth_classic.h"

#include <stdio.h>
#include <string.h>

#if defined(PICO_INTERCOM_TARGET)
#include "btstack.h"
#include "intercom_ble.h"
#include "pico/btstack_cyw43.h"
#include "pico/stdlib.h"
#endif

#if defined(PICO_INTERCOM_TARGET)
enum {
    INTERCOM_SERVICE_UUID16 = 0xFFF0,
    INTERCOM_TX_UUID16 = 0xFFF1,
    INTERCOM_RX_UUID16 = 0xFFF2,
    INTERCOM_ADV_FLAGS = 0x06,
    INTERCOM_MAX_SERVER_OUTBOX = BLUETOOTH_TRANSPORT_QUEUE_DEPTH
};

typedef enum {
    TARGET_ROLE_NONE = 0,
    TARGET_ROLE_CENTRAL,
    TARGET_ROLE_PERIPHERAL
} target_role_t;

typedef enum {
    TARGET_STATE_OFF = 0,
    TARGET_STATE_STARTING,
    TARGET_STATE_SCANNING,
    TARGET_STATE_CONNECTING,
    TARGET_STATE_DISCOVER_SERVICE,
    TARGET_STATE_DISCOVER_TX_CHARACTERISTIC,
    TARGET_STATE_DISCOVER_RX_CHARACTERISTIC,
    TARGET_STATE_ENABLE_NOTIFICATIONS,
    TARGET_STATE_READY
} target_state_t;

typedef struct {
    bool initialized;
    bool event_handler_registered;
    bool att_handler_registered;
    bool gatt_ready;
    bool notifications_enabled;
    bool notification_pending;
    bool listener_registered;
    target_role_t role;
    target_state_t state;
    uint8_t selected_peer_id;
    bd_addr_t selected_address;
    bd_addr_type_t selected_address_type;
    hci_con_handle_t con_handle;
    gatt_client_service_t service;
    gatt_client_characteristic_t tx_characteristic;
    gatt_client_characteristic_t rx_characteristic;
    gatt_client_notification_t notification_listener;
    btstack_packet_callback_registration_t hci_event_callback_registration;
    bluetooth_classic_stack_t *stack;
    bluetooth_transport_packet_t server_outbox[INTERCOM_MAX_SERVER_OUTBOX];
    size_t server_outbox_count;
} bluetooth_target_backend_t;

static bluetooth_target_backend_t bluetooth_target_backend = {0};
extern const uint8_t profile_data[];

static bool transport_has_remembered_peer(const bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL) {
        return false;
    }

    for (size_t index = 0; index < transport->remembered_peer_count; ++index) {
        if (transport->remembered_peers[index] == peer_id) {
            return true;
        }
    }

    return false;
}

static size_t transport_find_discovered_index(const bluetooth_transport_t *transport,
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
transport_get_discovered_peer(bluetooth_transport_t *transport, uint8_t peer_id, bool create) {
    if (transport == NULL || peer_id == 0U || peer_id == transport->local_peer_id) {
        return NULL;
    }

    const size_t found = transport_find_discovered_index(transport, peer_id);
    if (found < INTERCOM_MAX_PEERS) {
        return &transport->discovered_peers[found];
    }

    if (!create) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        if (!transport->discovered_peers[index].valid) {
            bluetooth_transport_peer_info_t *peer = &transport->discovered_peers[index];
            memset(peer, 0, sizeof(*peer));
            peer->valid = true;
            peer->peer_id = peer_id;
            if (transport->discovered_peer_count < INTERCOM_MAX_PEERS) {
                transport->discovered_peer_count++;
            }
            return peer;
        }
    }

    return NULL;
}

static void transport_mark_connected(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL || peer_id == 0U) {
        return;
    }

    bluetooth_transport_peer_info_t *peer = transport_get_discovered_peer(transport, peer_id, true);
    if (peer != NULL) {
        peer->paired = true;
        peer->pairing_pending = false;
    }

    for (size_t index = 0; index < transport->connected_peer_count; ++index) {
        if (transport->connected_peers[index] == peer_id) {
            transport->peer_states[index] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
            transport->pending_pair_peer_id = 0U;
            return;
        }
    }

    if (transport->connected_peer_count >= INTERCOM_MAX_PEERS) {
        transport->error = true;
        transport->last_error_code = 2U;
        return;
    }

    const size_t slot = transport->connected_peer_count++;
    transport->connected_peers[slot] = peer_id;
    transport->peer_states[slot] = BLUETOOTH_TRANSPORT_STATE_CONNECTED;
    transport->pending_pair_peer_id = 0U;
    transport->last_error_code = 0U;
}

static void transport_mark_disconnected(bluetooth_transport_t *transport, uint8_t peer_id) {
    if (transport == NULL) {
        return;
    }

    for (size_t index = 0; index < transport->connected_peer_count; ++index) {
        if (transport->connected_peers[index] != peer_id) {
            continue;
        }

        for (size_t shift = index + 1U; shift < transport->connected_peer_count; ++shift) {
            transport->connected_peers[shift - 1U] = transport->connected_peers[shift];
            transport->peer_states[shift - 1U] = transport->peer_states[shift];
        }
        transport->connected_peer_count--;
        if (transport->connected_peer_count < INTERCOM_MAX_PEERS) {
            transport->peer_states[transport->connected_peer_count] =
                BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
        }
        break;
    }

    const size_t found = transport_find_discovered_index(transport, peer_id);
    if (found < INTERCOM_MAX_PEERS) {
        transport->discovered_peers[found].paired = false;
        transport->discovered_peers[found].pairing_pending = false;
    }
    transport->pending_pair_peer_id = 0U;
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void make_short_name(char *buffer, size_t buffer_len, uint8_t peer_id) {
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }

    (void)snprintf(buffer, buffer_len, "ic%02X", (unsigned)peer_id);
}

static bool advertisement_has_service(uint16_t service_uuid16, const uint8_t *report) {
    const uint8_t *adv_data = gap_event_advertising_report_get_data(report);
    const uint8_t adv_len = gap_event_advertising_report_get_data_length(report);
    ad_context_t context;

    for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        const uint8_t type = ad_iterator_get_data_type(&context);
        const uint8_t size = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);
        if ((type == BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS ||
             type == BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS) &&
            size >= 2U) {
            for (uint8_t offset = 0U; (offset + 1U) < size; offset += 2U) {
                if (little_endian_read_16(data, offset) == service_uuid16) {
                    return true;
                }
            }
        }
    }

    return false;
}

static bool advertisement_get_peer_id(const uint8_t *report, uint8_t *peer_id) {
    if (report == NULL || peer_id == NULL) {
        return false;
    }

    const uint8_t *adv_data = gap_event_advertising_report_get_data(report);
    const uint8_t adv_len = gap_event_advertising_report_get_data_length(report);
    ad_context_t context;

    for (ad_iterator_init(&context, adv_len, adv_data); ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        const uint8_t type = ad_iterator_get_data_type(&context);
        const uint8_t size = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);
        if (type == BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA && size >= 3U &&
            data[0] == 0xFFU && data[1] == 0xFFU) {
            *peer_id = data[2];
            return *peer_id != 0U;
        }
    }

    return false;
}

static void update_advertising_data(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return;
    }

    char short_name[8] = {0};
    make_short_name(short_name, sizeof(short_name), stack->transport.local_peer_id);
    const uint8_t name_len = (uint8_t)strlen(short_name);
    uint8_t adv_data[31] = {
        0x02, BLUETOOTH_DATA_TYPE_FLAGS, INTERCOM_ADV_FLAGS,
        0x03, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS,
        (uint8_t)(INTERCOM_SERVICE_UUID16 & 0xFFU),
        (uint8_t)((INTERCOM_SERVICE_UUID16 >> 8) & 0xFFU),
        0x04, BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA, 0xFF, 0xFF,
        stack->transport.local_peer_id,
    };
    size_t adv_len = 11U;

    if (name_len > 0U && adv_len + (size_t)name_len + 2U <= sizeof(adv_data)) {
        adv_data[adv_len++] = (uint8_t)(name_len + 1U);
        adv_data[adv_len++] = BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME;
        memcpy(&adv_data[adv_len], short_name, name_len);
        adv_len += name_len;
    }

    uint16_t adv_int_min = 0x0030;
    uint16_t adv_int_max = 0x0030;
    bd_addr_t null_addr = {0};
    gap_advertisements_set_params(adv_int_min, adv_int_max, 0, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data((uint8_t)adv_len, adv_data);
    gap_advertisements_enable(1);
}

static void start_scanning(void) {
    gap_set_scan_parameters(0, 0x0030, 0x0030);
    gap_start_scan();
    bluetooth_target_backend.state = TARGET_STATE_SCANNING;
}

static bluetooth_transport_peer_info_t *find_discovered_by_address(bluetooth_transport_t *transport,
                                                                   const bd_addr_t address,
                                                                   bd_addr_type_t address_type) {
    if (transport == NULL) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_transport_peer_info_t *peer = &transport->discovered_peers[index];
        if (!peer->valid || peer->address_type != (uint8_t)address_type) {
            continue;
        }
        if (memcmp(peer->address, address, sizeof(peer->address)) == 0) {
            return peer;
        }
    }

    return NULL;
}

static void connect_to_peer(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL || bluetooth_target_backend.state == TARGET_STATE_CONNECTING ||
        bluetooth_target_backend.con_handle != HCI_CON_HANDLE_INVALID) {
        return;
    }

    bluetooth_transport_peer_info_t *peer =
        transport_get_discovered_peer(&stack->transport, peer_id, false);
    if (peer == NULL) {
        return;
    }

    memcpy(bluetooth_target_backend.selected_address, peer->address,
           sizeof(bluetooth_target_backend.selected_address));
    bluetooth_target_backend.selected_address_type = (bd_addr_type_t)peer->address_type;
    bluetooth_target_backend.selected_peer_id = peer_id;
    peer->pairing_pending = true;
    stack->transport.pending_pair_peer_id = peer_id;
    gap_stop_scan();
    bluetooth_target_backend.state = TARGET_STATE_CONNECTING;
    gap_connect(bluetooth_target_backend.selected_address,
                bluetooth_target_backend.selected_address_type);
}

static void server_request_can_send(void) {
    if (bluetooth_target_backend.con_handle == HCI_CON_HANDLE_INVALID ||
        bluetooth_target_backend.server_outbox_count == 0U ||
        bluetooth_target_backend.role != TARGET_ROLE_PERIPHERAL ||
        !bluetooth_target_backend.notifications_enabled ||
        bluetooth_target_backend.notification_pending) {
        return;
    }

    bluetooth_target_backend.notification_pending = true;
    att_server_request_can_send_now_event(bluetooth_target_backend.con_handle);
}

static bool enqueue_server_notification(const bluetooth_transport_packet_t *packet) {
    if (packet == NULL ||
        bluetooth_target_backend.server_outbox_count >= INTERCOM_MAX_SERVER_OUTBOX) {
        return false;
    }

    bluetooth_target_backend.server_outbox[bluetooth_target_backend.server_outbox_count++] = *packet;
    server_request_can_send();
    return true;
}

static void dequeue_server_notification(void) {
    if (bluetooth_target_backend.server_outbox_count == 0U) {
        return;
    }

    for (size_t index = 1U; index < bluetooth_target_backend.server_outbox_count; ++index) {
        bluetooth_target_backend.server_outbox[index - 1U] =
            bluetooth_target_backend.server_outbox[index];
    }
    bluetooth_target_backend.server_outbox_count--;
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                                     uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    bluetooth_classic_stack_t *stack = bluetooth_target_backend.stack;
    if (stack == NULL) {
        return;
    }

    switch (bluetooth_target_backend.state) {
    case TARGET_STATE_DISCOVER_SERVICE:
        switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet, &bluetooth_target_backend.service);
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) {
                gap_disconnect(bluetooth_target_backend.con_handle);
                break;
            }
            bluetooth_target_backend.state = TARGET_STATE_DISCOVER_TX_CHARACTERISTIC;
            gatt_client_discover_characteristics_for_service_by_uuid16(
                handle_gatt_client_event, bluetooth_target_backend.con_handle,
                &bluetooth_target_backend.service, INTERCOM_TX_UUID16);
            break;
        default:
            break;
        }
        break;
    case TARGET_STATE_DISCOVER_TX_CHARACTERISTIC:
        switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(
                packet, &bluetooth_target_backend.tx_characteristic);
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) {
                gap_disconnect(bluetooth_target_backend.con_handle);
                break;
            }
            bluetooth_target_backend.state = TARGET_STATE_DISCOVER_RX_CHARACTERISTIC;
            gatt_client_discover_characteristics_for_service_by_uuid16(
                handle_gatt_client_event, bluetooth_target_backend.con_handle,
                &bluetooth_target_backend.service, INTERCOM_RX_UUID16);
            break;
        default:
            break;
        }
        break;
    case TARGET_STATE_DISCOVER_RX_CHARACTERISTIC:
        switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(
                packet, &bluetooth_target_backend.rx_characteristic);
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) {
                gap_disconnect(bluetooth_target_backend.con_handle);
                break;
            }
            bluetooth_target_backend.listener_registered = true;
            gatt_client_listen_for_characteristic_value_updates(
                &bluetooth_target_backend.notification_listener, handle_gatt_client_event,
                bluetooth_target_backend.con_handle, &bluetooth_target_backend.tx_characteristic);
            bluetooth_target_backend.state = TARGET_STATE_ENABLE_NOTIFICATIONS;
            gatt_client_write_client_characteristic_configuration(
                handle_gatt_client_event, bluetooth_target_backend.con_handle,
                &bluetooth_target_backend.tx_characteristic,
                GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            break;
        default:
            break;
        }
        break;
    case TARGET_STATE_ENABLE_NOTIFICATIONS:
        if (hci_event_packet_get_type(packet) == GATT_EVENT_QUERY_COMPLETE &&
            gatt_event_query_complete_get_att_status(packet) == ATT_ERROR_SUCCESS) {
            bluetooth_target_backend.state = TARGET_STATE_READY;
            stack->connected = true;
            stack->transport.backend_ready = true;
            printf("Bluetooth LE link ready for peer %u.\n",
                   (unsigned)bluetooth_target_backend.selected_peer_id);
        }
        break;
    case TARGET_STATE_READY:
        if (hci_event_packet_get_type(packet) == GATT_EVENT_NOTIFICATION) {
            const uint8_t *value = gatt_event_notification_get_value(packet);
            const uint16_t value_length = gatt_event_notification_get_value_length(packet);
            if (value != NULL && value_length > 0U) {
                (void)bluetooth_transport_queue_packet(
                    &stack->transport, bluetooth_target_backend.selected_peer_id,
                    stack->transport.local_peer_id, value, value_length);
            }
        }
        break;
    default:
        break;
    }
}

static void att_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                               uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    bluetooth_classic_stack_t *stack = bluetooth_target_backend.stack;
    if (stack == NULL) {
        return;
    }

    if (hci_event_packet_get_type(packet) == ATT_EVENT_CAN_SEND_NOW) {
        bluetooth_target_backend.notification_pending = false;
        if (bluetooth_target_backend.server_outbox_count == 0U) {
            return;
        }

        const bluetooth_transport_packet_t *next = &bluetooth_target_backend.server_outbox[0];
        att_server_notify(bluetooth_target_backend.con_handle,
                          ATT_CHARACTERISTIC_FFF1_01_VALUE_HANDLE, next->payload,
                          (uint16_t)next->payload_len);
        stack->transport.packets_delivered++;
        dequeue_server_notification();
        server_request_can_send();
    }
}

static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle,
                                  uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    UNUSED(connection_handle);
    UNUSED(att_handle);
    UNUSED(offset);
    UNUSED(buffer);
    UNUSED(buffer_size);
    return 0;
}

static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle,
                              uint16_t transaction_mode, uint16_t offset, uint8_t *buffer,
                              uint16_t buffer_size) {
    UNUSED(transaction_mode);
    UNUSED(offset);

    bluetooth_classic_stack_t *stack = bluetooth_target_backend.stack;
    if (stack == NULL || buffer == NULL || buffer_size == 0U) {
        return 0;
    }

    if (att_handle == ATT_CHARACTERISTIC_FFF1_01_CLIENT_CONFIGURATION_HANDLE) {
        bluetooth_target_backend.notifications_enabled =
            little_endian_read_16(buffer, 0) ==
            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
        bluetooth_target_backend.con_handle = connection_handle;
        bluetooth_target_backend.role = TARGET_ROLE_PERIPHERAL;
        bluetooth_target_backend.state = TARGET_STATE_READY;
        server_request_can_send();
        return 0;
    }

    if (att_handle == ATT_CHARACTERISTIC_FFF2_01_VALUE_HANDLE &&
        bluetooth_target_backend.selected_peer_id != 0U) {
        (void)bluetooth_transport_queue_packet(&stack->transport,
                                               bluetooth_target_backend.selected_peer_id,
                                               stack->transport.local_peer_id, buffer,
                                               buffer_size);
    }

    return 0;
}

static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                              uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    bluetooth_classic_stack_t *stack = bluetooth_target_backend.stack;
    if (stack == NULL) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            stack->initialized = true;
            stack->transport.backend_ready = true;
            update_advertising_data(stack);
            start_scanning();
            printf("Bluetooth LE runtime active as peer %u.\n",
                   (unsigned)stack->transport.local_peer_id);
        } else {
            stack->transport.backend_ready = false;
            bluetooth_target_backend.state = TARGET_STATE_OFF;
        }
        break;
    case GAP_EVENT_ADVERTISING_REPORT:
        if (!advertisement_has_service(INTERCOM_SERVICE_UUID16, packet)) {
            return;
        }
        {
            uint8_t peer_id = 0U;
            if (!advertisement_get_peer_id(packet, &peer_id) || peer_id == stack->transport.local_peer_id) {
                return;
            }

            bluetooth_transport_peer_info_t *peer =
                transport_get_discovered_peer(&stack->transport, peer_id, true);
            if (peer == NULL) {
                return;
            }

            gap_event_advertising_report_get_address(packet, peer->address);
            peer->address_type = (uint8_t)gap_event_advertising_report_get_address_type(packet);
            peer->last_seen_ms = now_ms();
            if (peer->name[0] == '\0') {
                make_short_name(peer->name, sizeof(peer->name), peer_id);
            }

            if ((stack->transport.pending_pair_peer_id == peer_id ||
                 transport_has_remembered_peer(&stack->transport, peer_id)) &&
                bluetooth_target_backend.con_handle == HCI_CON_HANDLE_INVALID &&
                bluetooth_target_backend.state == TARGET_STATE_SCANNING) {
                connect_to_peer(stack, peer_id);
            }
        }
        break;
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            bd_addr_t peer_address;
            gap_subevent_le_connection_complete_get_peer_address(packet, peer_address);
            const bd_addr_type_t peer_address_type =
                gap_subevent_le_connection_complete_get_peer_address_type(packet);
            bluetooth_transport_peer_info_t *peer =
                find_discovered_by_address(&stack->transport, peer_address, peer_address_type);

            bluetooth_target_backend.con_handle =
                gap_subevent_le_connection_complete_get_connection_handle(packet);
            bluetooth_target_backend.role =
                gap_subevent_le_connection_complete_get_role(packet) == 0U ? TARGET_ROLE_CENTRAL
                                                                           : TARGET_ROLE_PERIPHERAL;
            if (peer != NULL) {
                bluetooth_target_backend.selected_peer_id = peer->peer_id;
            }
            if (bluetooth_target_backend.selected_peer_id == 0U) {
                bluetooth_target_backend.selected_peer_id = stack->transport.pending_pair_peer_id;
            }
            if (bluetooth_target_backend.selected_peer_id == 0U) {
                gap_disconnect(bluetooth_target_backend.con_handle);
                return;
            }

            transport_mark_connected(&stack->transport, bluetooth_target_backend.selected_peer_id);
            stack->paired_peer_id = bluetooth_target_backend.selected_peer_id;
            stack->connected = true;

            if (bluetooth_target_backend.role == TARGET_ROLE_CENTRAL) {
                bluetooth_target_backend.state = TARGET_STATE_DISCOVER_SERVICE;
                gatt_client_discover_primary_services_by_uuid16(
                    handle_gatt_client_event, bluetooth_target_backend.con_handle,
                    INTERCOM_SERVICE_UUID16);
            } else {
                bluetooth_target_backend.state = TARGET_STATE_READY;
                printf("Accepted Bluetooth LE connection from peer %u.\n",
                       (unsigned)bluetooth_target_backend.selected_peer_id);
            }
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (bluetooth_target_backend.listener_registered) {
            bluetooth_target_backend.listener_registered = false;
            gatt_client_stop_listening_for_characteristic_value_updates(
                &bluetooth_target_backend.notification_listener);
        }
        transport_mark_disconnected(&stack->transport, bluetooth_target_backend.selected_peer_id);
        bluetooth_target_backend.con_handle = HCI_CON_HANDLE_INVALID;
        bluetooth_target_backend.role = TARGET_ROLE_NONE;
        bluetooth_target_backend.notifications_enabled = false;
        bluetooth_target_backend.notification_pending = false;
        bluetooth_target_backend.server_outbox_count = 0U;
        bluetooth_target_backend.state = TARGET_STATE_SCANNING;
        stack->connected = false;
        stack->transport.backend_ready = true;
        update_advertising_data(stack);
        start_scanning();
        printf("Bluetooth LE peer %u disconnected.\n",
               (unsigned)bluetooth_target_backend.selected_peer_id);
        bluetooth_target_backend.selected_peer_id = 0U;
        break;
    case SM_EVENT_JUST_WORKS_REQUEST:
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        break;
    default:
        break;
    }
}

static bool target_backend_start(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return false;
    }

    bluetooth_target_backend.stack = stack;
    stack->transport.network_connected = true;
    stack->transport.backend_ready = false;

    if (bluetooth_target_backend.initialized) {
        hci_power_control(HCI_POWER_ON);
        bluetooth_target_backend.state = TARGET_STATE_STARTING;
        return true;
    }

    memset(&bluetooth_target_backend, 0, sizeof(bluetooth_target_backend));
    bluetooth_target_backend.stack = stack;
    bluetooth_target_backend.con_handle = HCI_CON_HANDLE_INVALID;
    bluetooth_target_backend.state = TARGET_STATE_STARTING;

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    att_server_init(profile_data, att_read_callback, att_write_callback);
    gatt_client_init();

    bluetooth_target_backend.hci_event_callback_registration.callback = hci_event_handler;
    hci_add_event_handler(&bluetooth_target_backend.hci_event_callback_registration);
    bluetooth_target_backend.event_handler_registered = true;
    att_server_register_packet_handler(att_packet_handler);
    bluetooth_target_backend.att_handler_registered = true;
    bluetooth_target_backend.initialized = true;

    hci_power_control(HCI_POWER_ON);
    return true;
}

static bool target_backend_stop(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return false;
    }

    if (bluetooth_target_backend.initialized) {
        gap_stop_scan();
        gap_advertisements_enable(0);
        if (bluetooth_target_backend.con_handle != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(bluetooth_target_backend.con_handle);
        }
        hci_power_control(HCI_POWER_OFF);
    }

    stack->connected = false;
    stack->transport.backend_ready = false;
    stack->transport.network_connected = false;
    bluetooth_target_backend.con_handle = HCI_CON_HANDLE_INVALID;
    bluetooth_target_backend.selected_peer_id = 0U;
    bluetooth_target_backend.role = TARGET_ROLE_NONE;
    bluetooth_target_backend.notifications_enabled = false;
    bluetooth_target_backend.notification_pending = false;
    bluetooth_target_backend.server_outbox_count = 0U;
    return true;
}
#endif

static bool bluetooth_classic_stack_is_ready(const bluetooth_classic_stack_t *stack) {
    return stack != NULL && stack->initialized;
}

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
}

bool bluetooth_classic_stack_set_enabled(bluetooth_classic_stack_t *stack, bool enabled) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->enabled = enabled;
    if (!bluetooth_transport_set_enabled(&stack->transport, enabled)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    return enabled ? target_backend_start(stack) : target_backend_stop(stack);
#else
    return true;
#endif
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
    return bluetooth_classic_stack_connect(stack, peer_id);
}

bool bluetooth_classic_stack_connect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack) || !stack->enabled || peer_id == 0U) {
        return false;
    }

    if (!bluetooth_transport_connect(&stack->transport, peer_id)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    connect_to_peer(stack, peer_id);
    return true;
#else
    if (bluetooth_transport_is_connected(&stack->transport, peer_id)) {
        stack->connected = true;
    }
    return bluetooth_transport_is_connected(&stack->transport, peer_id);
#endif
}

bool bluetooth_classic_stack_disconnect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    if (bluetooth_target_backend.con_handle != HCI_CON_HANDLE_INVALID &&
        bluetooth_target_backend.selected_peer_id == peer_id) {
        gap_disconnect(bluetooth_target_backend.con_handle);
    }
#endif

    if (!bluetooth_transport_disconnect(&stack->transport, peer_id)) {
        return false;
    }

    stack->connected = false;
    return true;
}

bool bluetooth_classic_stack_restore_pairing(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    return bluetooth_transport_restore_pairing(&stack->transport, peer_id);
}

bool bluetooth_classic_stack_poll(bluetooth_classic_stack_t *stack) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->transport.last_poll_ms =
#if defined(PICO_INTERCOM_TARGET)
        now_ms();
#else
        0U;
#endif

    return bluetooth_transport_poll(&stack->transport);
}

bool bluetooth_classic_stack_select_pairing_candidate(const bluetooth_classic_stack_t *stack,
                                                      uint8_t *peer_id) {
    if (stack == NULL) {
        return false;
    }

    return bluetooth_transport_select_pairing_candidate(&stack->transport, peer_id);
}

bool bluetooth_classic_stack_queue_packet(bluetooth_classic_stack_t *stack, uint8_t source_peer,
                                          uint8_t target_peer, const uint8_t *payload,
                                          size_t payload_len) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    if (!stack->enabled || !stack->connected || payload == NULL || payload_len == 0U ||
        payload_len > BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN ||
        target_peer != bluetooth_target_backend.selected_peer_id) {
        return false;
    }

    bluetooth_transport_packet_t packet = {
        .source_peer = source_peer,
        .target_peer = target_peer,
        .payload_len = payload_len,
    };
    memcpy(packet.payload, payload, payload_len);

    stack->transport.packets_queued++;
    stack->transport.last_source_peer = source_peer;
    stack->transport.last_target_peer = target_peer;

    if (bluetooth_target_backend.role == TARGET_ROLE_CENTRAL) {
        const int status = gatt_client_write_value_of_characteristic_without_response(
            bluetooth_target_backend.con_handle, &bluetooth_target_backend.rx_characteristic,
            packet.payload, (uint16_t)payload_len);
        if (status != ERROR_CODE_SUCCESS) {
            stack->transport.packets_dropped++;
            stack->transport.last_error_code = (uint32_t)status;
            return false;
        }
        stack->transport.packets_delivered++;
        stack->transport.last_error_code = 0U;
        return true;
    }

    if (!enqueue_server_notification(&packet)) {
        stack->transport.packets_dropped++;
        stack->transport.last_error_code = 4U;
        return false;
    }

    stack->transport.last_error_code = 0U;
    return true;
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

    return bluetooth_transport_pending_count(&stack->transport);
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

    if (stack->discoverable) {
        return "discoverable";
    }

    return "idle";
}
