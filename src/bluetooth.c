#include "bluetooth.h"

#if defined(PICO_INTERCOM_TARGET)
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#endif

#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *alias;
    bluetooth_command_id_t command_id;
} bluetooth_command_alias_t;

#define BLUETOOTH_COMMAND_ALIAS_COUNT \
    ((size_t)(sizeof(bluetooth_command_aliases) / sizeof(bluetooth_command_aliases[0])))

static const bluetooth_command_alias_t bluetooth_command_aliases[] = {
    {"enable", BLUETOOTH_COMMAND_ENABLE},
    {"on", BLUETOOTH_COMMAND_ENABLE},
    {"power_on", BLUETOOTH_COMMAND_ENABLE},
    {"disable", BLUETOOTH_COMMAND_DISABLE},
    {"off", BLUETOOTH_COMMAND_DISABLE},
    {"power_off", BLUETOOTH_COMMAND_DISABLE},
    {"toggle", BLUETOOTH_COMMAND_TOGGLE},
    {"switch", BLUETOOTH_COMMAND_TOGGLE},
    {"connect", BLUETOOTH_COMMAND_CONNECT},
    {"pair", BLUETOOTH_COMMAND_CONNECT},
    {"disconnect", BLUETOOTH_COMMAND_DISCONNECT},
    {"unpair", BLUETOOTH_COMMAND_DISCONNECT},
    {"status", BLUETOOTH_COMMAND_STATUS},
    {"info", BLUETOOTH_COMMAND_STATUS},
};

enum {
    BLUETOOTH_ERROR_NONE = 0u,
    BLUETOOTH_ERROR_DISABLED = 1u,
    BLUETOOTH_ERROR_PEER_LIMIT = 2u,
    BLUETOOTH_ERROR_INVALID_INPUT = 3u,
    BLUETOOTH_ERROR_STORAGE = 4u,
    BLUETOOTH_ERROR_NOT_READY = 5u,
    BLUETOOTH_ERROR_PROTOCOL = 6u,
    BLUETOOTH_ERROR_SATURATED = 7u,
};

enum {
    BLUETOOTH_HOST_POLL_TICK_MS = 100U,
    BLUETOOTH_HANDSHAKE_RETRY_MS = 1000U,
    BLUETOOTH_KEEPALIVE_MS = 1500U,
    BLUETOOTH_SESSION_TIMEOUT_MS = 5000U,
};

static bool bluetooth_command_from_string(const char *command,
                                          bluetooth_command_id_t *command_id) {
    if (command == NULL || command_id == NULL) {
        return false;
    }

    for (size_t index = 0; index < BLUETOOTH_COMMAND_ALIAS_COUNT; ++index) {
        if (strcasecmp(command, bluetooth_command_aliases[index].alias) == 0) {
            *command_id = bluetooth_command_aliases[index].command_id;
            return true;
        }
    }

    return false;
}

static bool bluetooth_runtime_is_ready(const bluetooth_runtime_t *runtime) {
    return runtime != NULL && runtime->initialized;
}

bool bluetooth_runtime_has_transport(const bluetooth_runtime_t *runtime) {
    return runtime != NULL && runtime->initialized && runtime->enabled &&
           runtime->platform_initialized && !runtime->platform_error &&
           runtime->classic_stack.transport.backend_ready;
}

bool bluetooth_runtime_is_operational(const bluetooth_runtime_t *runtime) {
    return bluetooth_runtime_has_transport(runtime) && runtime->session_ready_peer_count > 0U;
}

static uint32_t bluetooth_now_ms(const bluetooth_runtime_t *runtime) {
#if defined(PICO_INTERCOM_TARGET)
    (void)runtime;
    return to_ms_since_boot(get_absolute_time());
#else
    return runtime != NULL ? runtime->last_status_ms : 0U;
#endif
}

static void bluetooth_advance_poll_clock(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

#if defined(PICO_INTERCOM_TARGET)
    runtime->last_status_ms = bluetooth_now_ms(runtime);
#else
    runtime->last_status_ms += BLUETOOTH_HOST_POLL_TICK_MS;
#endif
}

static void bluetooth_make_peer_identity(char *buffer, size_t buffer_len, uint8_t peer_id) {
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }

    (void)snprintf(buffer, buffer_len, "pico-intercom-%u", (unsigned)peer_id);
}

static bluetooth_peer_link_t *bluetooth_find_peer_link(bluetooth_runtime_t *runtime,
                                                       uint8_t peer_id) {
    if (runtime == NULL || peer_id == 0U) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_peer_link_t *link = &runtime->peer_links[index];
        if (link->valid && link->peer_id == peer_id) {
            return link;
        }
    }

    return NULL;
}

static const bluetooth_peer_link_t *bluetooth_find_peer_link_const(const bluetooth_runtime_t *runtime,
                                                                   uint8_t peer_id) {
    return bluetooth_find_peer_link((bluetooth_runtime_t *)runtime, peer_id);
}

static bluetooth_peer_link_t *bluetooth_get_peer_link(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                                      bool create) {
    bluetooth_peer_link_t *existing = bluetooth_find_peer_link(runtime, peer_id);
    if (existing != NULL || !create || runtime == NULL || peer_id == 0U) {
        return existing;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_peer_link_t *link = &runtime->peer_links[index];
        if (link->valid) {
            continue;
        }

        memset(link, 0, sizeof(*link));
        link->valid = true;
        link->peer_id = peer_id;
        link->next_tx_sequence = 1U;
        link->link_state = BLUETOOTH_LINK_STATE_IDLE;
        return link;
    }

    return NULL;
}

static void bluetooth_release_peer_link(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    bluetooth_peer_link_t *link = bluetooth_find_peer_link(runtime, peer_id);
    if (link == NULL || link->remembered) {
        return;
    }

    memset(link, 0, sizeof(*link));
}

static void bluetooth_refresh_session_ready_count(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

    runtime->session_ready_peer_count = 0U;
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        const bluetooth_peer_link_t *link = &runtime->peer_links[index];
        if (link->valid && link->session_active && link->link_state == BLUETOOTH_LINK_STATE_READY) {
            runtime->session_ready_peer_count++;
        }
    }
}

static void bluetooth_record_error(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                   uint32_t error_code) {
    if (runtime == NULL) {
        return;
    }

    runtime->last_error_peer_id = peer_id;
    runtime->last_error_code = error_code;
}

static void bluetooth_mark_pairing_completed(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (runtime == NULL || !runtime->pairing_in_progress || runtime->pairing_peer_id != peer_id) {
        return;
    }

    runtime->pairing_in_progress = false;
    runtime->pairing_completed = true;
    runtime->pairing_error = false;
    runtime->completed_pairing_peer_id = peer_id;
}

static uint32_t bluetooth_generate_session_id(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (runtime == NULL) {
        return 0U;
    }

    if (runtime->next_session_id == 0U) {
        runtime->next_session_id = ((uint32_t)runtime->local_peer_id << 24U) | 1U;
    }

    uint32_t session_id = runtime->next_session_id++;
    session_id ^= ((uint32_t)peer_id << 16U);
    if (session_id == 0U) {
        session_id = 1U;
    }
    return session_id;
}

static bool bluetooth_has_pending_target(const bluetooth_runtime_t *runtime, uint8_t target_peer) {
    for (size_t index = 0; index < runtime->pending_relay_target_count; ++index) {
        if (runtime->relay_targets[index] == target_peer) {
            return true;
        }
    }

    return false;
}

static void bluetooth_record_relay(bluetooth_runtime_t *runtime, uint8_t source_peer,
                                   uint8_t target_peer, const uint8_t *payload,
                                   size_t payload_len) {
    if (runtime == NULL) {
        return;
    }

    runtime->relay_invocations++;
    if (bluetooth_has_pending_target(runtime, target_peer)) {
        return;
    }

    if (runtime->pending_relay_target_count >= INTERCOM_MAX_PEERS) {
        fprintf(stderr,
                "WARNING: bluetooth relay target tracking limit (%u) reached, cannot record peer %u\n",
                (unsigned)INTERCOM_MAX_PEERS, (unsigned)target_peer);
        return;
    }

    runtime->last_relay_source_peer = source_peer;
    runtime->last_relay_target = target_peer;
    runtime->relay_targets[runtime->pending_relay_target_count++] = target_peer;

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
    return bluetooth_find_peer_index(runtime, peer_id) <
           (runtime != NULL ? runtime->connected_peer_count : 0U);
}

static bool bluetooth_is_link_connected(const bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_has_peer(runtime, peer_id);
}

static void bluetooth_flush_classic_packets(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

#if !defined(PICO_INTERCOM_TARGET)
    bluetooth_classic_packet_t packet;
    while (bluetooth_classic_stack_dequeue_packet(&runtime->classic_stack, &packet)) {
        runtime->transport_packets_delivered++;
        runtime->last_transport_source_peer = packet.source_peer;
        runtime->last_transport_target_peer = packet.target_peer;
    }
#endif
}

static void bluetooth_sync_transport_counters(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

    runtime->transport_packets_queued = runtime->classic_stack.transport.packets_queued;
    runtime->transport_packets_delivered = runtime->classic_stack.transport.packets_delivered;
    runtime->transport_packets_dropped = runtime->classic_stack.transport.packets_dropped;
    runtime->last_transport_source_peer = runtime->classic_stack.transport.last_source_peer;
    runtime->last_transport_target_peer = runtime->classic_stack.transport.last_target_peer;
}

static bluetooth_peer_state_t bluetooth_peer_state_from_transport(
    bluetooth_transport_state_t transport_state) {
    switch (transport_state) {
    case BLUETOOTH_TRANSPORT_STATE_CONNECTING:
        return BLUETOOTH_PEER_STATE_CONNECTING;
    case BLUETOOTH_TRANSPORT_STATE_CONNECTED:
        return BLUETOOTH_PEER_STATE_CONNECTED;
    case BLUETOOTH_TRANSPORT_STATE_DISCONNECTING:
        return BLUETOOTH_PEER_STATE_DISCONNECTING;
    case BLUETOOTH_TRANSPORT_STATE_DISCONNECTED:
    default:
        return BLUETOOTH_PEER_STATE_DISCONNECTED;
    }
}

static void bluetooth_sync_connected_peers(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

#if defined(PICO_INTERCOM_TARGET)
    runtime->connected_peer_count = runtime->classic_stack.transport.connected_peer_count;
    for (size_t index = 0; index < runtime->connected_peer_count; ++index) {
        runtime->connected_peers[index] = runtime->classic_stack.transport.connected_peers[index];
        runtime->peer_states[index] = bluetooth_peer_state_from_transport(
            runtime->classic_stack.transport.peer_states[index]);
    }
    for (size_t index = runtime->connected_peer_count; index < INTERCOM_MAX_PEERS; ++index) {
        runtime->connected_peers[index] = 0U;
        runtime->peer_states[index] = BLUETOOTH_PEER_STATE_DISCONNECTED;
    }
#else
    (void)runtime;
#endif
}

static uint8_t bluetooth_derive_local_peer_id(void) {
#if defined(PICO_INTERCOM_TARGET)
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    uint8_t crc = 0U;
    for (size_t index = 0; index < sizeof(board_id.id); ++index) {
        crc ^= board_id.id[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            /* CRC-8 with polynomial 0x07 gives a better spread than a raw XOR fold
             * before we compress the result into the firmware's 1-250 peer-ID range.
             * The final modulo and +1 keep the ID non-zero even if the CRC is zero. */
            crc = (crc & 0x80U) != 0U ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    /* Keep peer IDs in 1-250 and leave the top values free for future protocol sentinels. */
    return (uint8_t)((crc % 250U) + 1U);
#else
    return 1U;
#endif
}

static bool bluetooth_queue_protocol_message(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                             intercom_protocol_message_type_t message_type,
                                             const uint8_t *payload, size_t payload_len,
                                             uint16_t flags) {
    if (!bluetooth_runtime_has_transport(runtime) || peer_id == 0U) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    bluetooth_peer_link_t *link = bluetooth_get_peer_link(runtime, peer_id, true);
    if (link == NULL) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    if (!bluetooth_is_link_connected(runtime, peer_id)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    if (message_type == INTERCOM_PROTOCOL_MESSAGE_HELLO && link->session_id == 0U) {
        link->session_id = bluetooth_generate_session_id(runtime, peer_id);
    }

    if (message_type != INTERCOM_PROTOCOL_MESSAGE_HELLO && link->session_id == 0U) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PROTOCOL);
        return false;
    }

    uint8_t encoded[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    intercom_protocol_message_t message = {
        .version = INTERCOM_PROTOCOL_VERSION,
        .message_type = message_type,
        .session_id = link->session_id,
        .sequence = link->next_tx_sequence++,
        .ack_sequence = link->last_rx_sequence,
        .source_peer = runtime->local_peer_id,
        .target_peer = peer_id,
        .payload_len = (uint16_t)payload_len,
        .flags = flags,
        .payload = payload,
    };
    size_t encoded_len = 0U;
    if (!intercom_protocol_encode(encoded, sizeof(encoded), &message, &encoded_len)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PROTOCOL);
        return false;
    }

    if (!bluetooth_classic_stack_queue_packet(&runtime->classic_stack, runtime->local_peer_id,
                                              peer_id, encoded, encoded_len)) {
        runtime->protocol_messages_dropped++;
        link->dropped_messages++;
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_SATURATED);
        return false;
    }
    runtime->protocol_messages_sent++;
    if (message_type == INTERCOM_PROTOCOL_MESSAGE_HELLO ||
        message_type == INTERCOM_PROTOCOL_MESSAGE_HELLO_ACK) {
        link->last_handshake_ms = bluetooth_now_ms(runtime);
        link->hello_sent = true;
        if (link->link_state != BLUETOOTH_LINK_STATE_READY) {
            link->link_state = BLUETOOTH_LINK_STATE_HANDSHAKING;
        }
    }
    bluetooth_sync_transport_counters(runtime);
    return true;
}

static bool bluetooth_send_protocol_error(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                          intercom_protocol_error_code_t error_code,
                                          const char *reason) {
    uint8_t payload[64] = {0};
    size_t payload_len = 0U;
    if (!intercom_protocol_build_error_payload(payload, sizeof(payload), error_code, reason,
                                               &payload_len)) {
        return false;
    }

    return bluetooth_queue_protocol_message(runtime, peer_id, INTERCOM_PROTOCOL_MESSAGE_ERROR,
                                            payload, payload_len, 0U);
}

static bool bluetooth_send_hello(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                 intercom_protocol_message_type_t message_type) {
    char identity[INTERCOM_PROTOCOL_MAX_IDENTITY_LEN] = {0};
    bluetooth_make_peer_identity(identity, sizeof(identity), runtime->local_peer_id);
    return bluetooth_queue_protocol_message(runtime, peer_id, message_type,
                                            (const uint8_t *)identity, strlen(identity), 0U);
}

static void bluetooth_mark_session_ready(bluetooth_runtime_t *runtime, bluetooth_peer_link_t *link,
                                         uint8_t peer_id, uint32_t session_id) {
    if (runtime == NULL || link == NULL) {
        return;
    }

    link->valid = true;
    link->peer_id = peer_id;
    link->session_id = session_id != 0U ? session_id : link->session_id;
    link->session_active = true;
    link->hello_received = true;
    link->hello_sent = true;
    link->link_state = BLUETOOTH_LINK_STATE_READY;
    link->last_activity_ms = bluetooth_now_ms(runtime);
    bluetooth_refresh_session_ready_count(runtime);
    bluetooth_mark_pairing_completed(runtime, peer_id);
}

static void bluetooth_note_disconnected_link(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    bluetooth_peer_link_t *link = bluetooth_find_peer_link(runtime, peer_id);
    if (link == NULL) {
        return;
    }

    link->session_active = false;
    link->hello_received = false;
    link->hello_sent = false;
    link->last_rx_sequence = 0U;
    link->last_audio_sequence = 0U;
    link->link_state = link->remembered ? BLUETOOTH_LINK_STATE_IDLE : BLUETOOTH_LINK_STATE_DEGRADED;
    bluetooth_refresh_session_ready_count(runtime);
}

static void bluetooth_reconcile_peer_links(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_peer_link_t *link = &runtime->peer_links[index];
        if (!link->valid) {
            continue;
        }

        if (bluetooth_is_link_connected(runtime, link->peer_id)) {
            if (!link->session_active && link->link_state == BLUETOOTH_LINK_STATE_IDLE) {
                link->link_state = BLUETOOTH_LINK_STATE_CONNECTING;
            }
            continue;
        }

        bluetooth_note_disconnected_link(runtime, link->peer_id);
        if (!link->remembered) {
            bluetooth_release_peer_link(runtime, link->peer_id);
        }
    }
}

static void bluetooth_service_protocol_links(bluetooth_runtime_t *runtime) {
    if (!bluetooth_runtime_has_transport(runtime)) {
        return;
    }

    const uint32_t now_ms = bluetooth_now_ms(runtime);
    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_peer_link_t *link = &runtime->peer_links[index];
        if (!link->valid || !bluetooth_is_link_connected(runtime, link->peer_id)) {
            continue;
        }

        if (!link->session_active) {
            if (now_ms - link->last_handshake_ms >= BLUETOOTH_HANDSHAKE_RETRY_MS) {
                const bool first_handshake_attempt = !link->hello_sent;
                if (bluetooth_send_hello(runtime, link->peer_id, INTERCOM_PROTOCOL_MESSAGE_HELLO)) {
                    if (first_handshake_attempt) {
                        printf("Bluetooth session handshake with peer %u in progress.\n",
                               (unsigned)link->peer_id);
                    }
                }
            }
            continue;
        }

        if (now_ms - link->last_activity_ms >= BLUETOOTH_SESSION_TIMEOUT_MS) {
            printf("Bluetooth session with peer %u timed out; disconnecting stale link.\n",
                   (unsigned)link->peer_id);
            link->link_state = BLUETOOTH_LINK_STATE_DEGRADED;
            link->session_active = false;
            bluetooth_refresh_session_ready_count(runtime);
            bluetooth_record_error(runtime, link->peer_id, BLUETOOTH_ERROR_NOT_READY);
            (void)bluetooth_classic_stack_disconnect(&runtime->classic_stack, link->peer_id);
        } else if (now_ms - link->last_activity_ms >= BLUETOOTH_KEEPALIVE_MS) {
            (void)bluetooth_queue_protocol_message(runtime, link->peer_id,
                                                   INTERCOM_PROTOCOL_MESSAGE_KEEPALIVE, NULL, 0U,
                                                   0U);
        }
    }
}

static void bluetooth_relay(void *context, uint8_t source_peer, uint8_t target_peer,
                            const uint8_t *payload, size_t payload_len) {
    bluetooth_runtime_t *runtime = (bluetooth_runtime_t *)context;
    bluetooth_record_relay(runtime, source_peer, target_peer, payload, payload_len);
    if (!bluetooth_queue_protocol_message(runtime, target_peer, INTERCOM_PROTOCOL_MESSAGE_AUDIO,
                                          payload, payload_len, 0U)) {
        runtime->transport_packets_dropped++;
    }
}

static void bluetooth_reset_peer_states(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        runtime->connected_peers[index] = 0U;
        runtime->peer_states[index] = BLUETOOTH_PEER_STATE_DISCONNECTED;
    }
}

#if defined(PICO_INTERCOM_TARGET)
static bool bluetooth_platform_initialize_target(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return false;
    }

    if (runtime->platform_initialized && !runtime->platform_error) {
        return true;
    }

    const int cyw43_status = cyw43_arch_init();
    if (cyw43_status != 0) {
        runtime->platform_error = true;
        fprintf(stderr, "Bluetooth backend init failed: cyw43_arch_init returned %d\n",
                cyw43_status);
        return false;
    }

    runtime->platform_initialized = true;
    runtime->platform_error = false;
    runtime->advertising = true;
    runtime->scanning = false;
    printf("Bluetooth radio backend active; CYW43 transport initialized.\n");
    return true;
}
#endif

static bool bluetooth_platform_set_enabled(bluetooth_runtime_t *runtime, bool enabled) {
#if defined(PICO_INTERCOM_TARGET)
    if (runtime == NULL) {
        return false;
    }

    if (enabled) {
        return bluetooth_platform_initialize_target(runtime);
    }

    if (runtime->platform_initialized) {
        cyw43_arch_deinit();
    }

    runtime->platform_initialized = false;
    runtime->platform_error = false;
    runtime->advertising = false;
    runtime->scanning = false;
    return true;
#else
    (void)runtime;
    return true;
#endif
}

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom) {
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->intercom = intercom;
    runtime->enabled = true;
    runtime->advertising = true;
    runtime->scanning = true;
    runtime->local_peer_id = bluetooth_derive_local_peer_id();
#if defined(PICO_INTERCOM_TARGET)
    runtime->platform_initialized = false;
#else
    runtime->platform_initialized = true;
#endif
    runtime->initialized = true;
    runtime->next_session_id = ((uint32_t)runtime->local_peer_id << 24U) | 1U;
    intercom_audio_init(&runtime->audio);
    bluetooth_classic_stack_init(&runtime->classic_stack);
    bluetooth_classic_stack_set_local_peer_id(&runtime->classic_stack, runtime->local_peer_id);
    bluetooth_reset_peer_states(runtime);
    if (!bluetooth_platform_set_enabled(runtime, runtime->enabled) ||
        !bluetooth_classic_stack_set_enabled(&runtime->classic_stack, runtime->enabled)) {
        runtime->platform_error = true;
        runtime->enabled = false;
    }
    runtime->last_status_ms = 0U;
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
}

bool bluetooth_set_enabled(bluetooth_runtime_t *runtime, bool enabled) {
    if (!bluetooth_runtime_is_ready(runtime)) {
        return false;
    }

    if (!bluetooth_platform_set_enabled(runtime, enabled)) {
        runtime->enabled = false;
        (void)bluetooth_classic_stack_set_enabled(&runtime->classic_stack, false);
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_DISABLED);
        return false;
    }

    runtime->enabled = enabled;
    intercom_audio_set_enabled(&runtime->audio, enabled);
    if (!bluetooth_classic_stack_set_enabled(&runtime->classic_stack, enabled)) {
        runtime->platform_error = true;
        runtime->enabled = false;
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    if (!enabled) {
        runtime->pairing_in_progress = false;
        runtime->pairing_completed = false;
        runtime->session_ready_peer_count = 0U;
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_DISABLED);
    } else {
        runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    }

    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    bluetooth_reconcile_peer_links(runtime);
    return true;
}

bool bluetooth_enable(bluetooth_runtime_t *runtime) { return bluetooth_set_enabled(runtime, true); }

bool bluetooth_disable(bluetooth_runtime_t *runtime) { return bluetooth_set_enabled(runtime, false); }

bool bluetooth_toggle(bluetooth_runtime_t *runtime) {
    if (!bluetooth_runtime_is_ready(runtime)) {
        return false;
    }

    return bluetooth_set_enabled(runtime, !runtime->enabled) && runtime->enabled;
}

bool bluetooth_is_enabled(const bluetooth_runtime_t *runtime) {
    return runtime != NULL && runtime->enabled;
}

bool bluetooth_connect(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_connect_peer(runtime, peer_id);
}

bool bluetooth_disconnect(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_disconnect_peer(runtime, peer_id);
}

bool bluetooth_connect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (!bluetooth_runtime_is_ready(runtime) || peer_id == 0U) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    if (!runtime->enabled) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_DISABLED);
        return false;
    }

    bluetooth_peer_link_t *link = bluetooth_get_peer_link(runtime, peer_id, true);
    if (link == NULL) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    if (link->session_active && bluetooth_is_link_connected(runtime, peer_id)) {
        return true;
    }

    if (!bluetooth_classic_stack_connect(&runtime->classic_stack, peer_id)) {
        runtime->failed_connections++;
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    runtime->connection_attempts++;
    link->valid = true;
    link->peer_id = peer_id;
    link->link_state = BLUETOOTH_LINK_STATE_CONNECTING;
    link->last_handshake_ms = 0U;
    runtime->last_error_code = BLUETOOTH_ERROR_NONE;

#if !defined(PICO_INTERCOM_TARGET)
    if (runtime->intercom != NULL && !intercom_add_peer(runtime->intercom, peer_id)) {
        (void)bluetooth_classic_stack_disconnect(&runtime->classic_stack, peer_id);
        runtime->failed_disconnections++;
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    if (!bluetooth_has_peer(runtime, peer_id) && runtime->connected_peer_count < INTERCOM_MAX_PEERS) {
        const size_t peer_index = runtime->connected_peer_count++;
        runtime->connected_peers[peer_index] = peer_id;
        runtime->peer_states[peer_index] = BLUETOOTH_PEER_STATE_CONNECTED;
    }
    bluetooth_mark_session_ready(runtime, link, peer_id,
                                 bluetooth_generate_session_id(runtime, peer_id));
    runtime->successful_connections++;
#endif

    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    return true;
}

bool bluetooth_disconnect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (!bluetooth_runtime_is_ready(runtime)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    const size_t peer_index = bluetooth_find_peer_index(runtime, peer_id);
    if (peer_index >= runtime->connected_peer_count) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_INVALID_INPUT);
        return false;
    }

    bluetooth_peer_link_t *link = bluetooth_find_peer_link(runtime, peer_id);
    if (link != NULL && link->session_active) {
        (void)bluetooth_queue_protocol_message(runtime, peer_id, INTERCOM_PROTOCOL_MESSAGE_GOODBYE,
                                               NULL, 0U, 0U);
    }

    for (size_t shift = peer_index + 1U; shift < runtime->connected_peer_count; ++shift) {
        runtime->connected_peers[shift - 1U] = runtime->connected_peers[shift];
        runtime->peer_states[shift - 1U] = runtime->peer_states[shift];
    }
    runtime->connected_peer_count--;
    runtime->connected_peers[runtime->connected_peer_count] = 0U;
    runtime->peer_states[runtime->connected_peer_count] = BLUETOOTH_PEER_STATE_DISCONNECTED;

    runtime->disconnect_attempts++;
    runtime->successful_disconnections++;
    runtime->last_error_code = BLUETOOTH_ERROR_NONE;

    (void)bluetooth_classic_stack_disconnect(&runtime->classic_stack, peer_id);
    bluetooth_note_disconnected_link(runtime, peer_id);
    if (runtime->intercom != NULL) {
        (void)intercom_remove_peer(runtime->intercom, peer_id);
    }
    bluetooth_release_peer_link(runtime, peer_id);
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    return true;
}

bool bluetooth_is_peer_connected(const bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_has_peer(runtime, peer_id);
}

const char *bluetooth_error_name(uint32_t error_code) {
    switch (error_code) {
    case BLUETOOTH_ERROR_DISABLED:
        return "disabled";
    case BLUETOOTH_ERROR_PEER_LIMIT:
        return "peer_limit";
    case BLUETOOTH_ERROR_INVALID_INPUT:
        return "invalid_input";
    case BLUETOOTH_ERROR_STORAGE:
        return "storage";
    case BLUETOOTH_ERROR_NOT_READY:
        return "not_ready";
    case BLUETOOTH_ERROR_PROTOCOL:
        return "protocol";
    case BLUETOOTH_ERROR_SATURATED:
        return "saturated";
    case BLUETOOTH_ERROR_NONE:
    default:
        return "none";
    }
}

bool bluetooth_get_peer_state(const bluetooth_runtime_t *runtime, uint8_t peer_id,
                              bluetooth_peer_state_t *state) {
    if (runtime == NULL || state == NULL) {
        return false;
    }

    const size_t peer_index = bluetooth_find_peer_index(runtime, peer_id);
    if (peer_index >= runtime->connected_peer_count) {
        *state = BLUETOOTH_PEER_STATE_DISCONNECTED;
        return false;
    }

    *state = runtime->peer_states[peer_index];
    return true;
}

const char *bluetooth_peer_state_name(bluetooth_peer_state_t state) {
    switch (state) {
    case BLUETOOTH_PEER_STATE_CONNECTING:
        return "connecting";
    case BLUETOOTH_PEER_STATE_CONNECTED:
        return "connected";
    case BLUETOOTH_PEER_STATE_DISCONNECTING:
        return "disconnecting";
    case BLUETOOTH_PEER_STATE_DISCONNECTED:
    default:
        return "disconnected";
    }
}

bool bluetooth_execute_command(bluetooth_runtime_t *runtime, bluetooth_command_id_t command,
                               uint8_t peer_id) {
    if (runtime == NULL) {
        return false;
    }

    bool succeeded = false;
    switch (command) {
    case BLUETOOTH_COMMAND_ENABLE:
        succeeded = bluetooth_enable(runtime);
        break;
    case BLUETOOTH_COMMAND_DISABLE:
        succeeded = bluetooth_disable(runtime);
        break;
    case BLUETOOTH_COMMAND_TOGGLE:
        succeeded = bluetooth_toggle(runtime);
        break;
    case BLUETOOTH_COMMAND_CONNECT:
    case BLUETOOTH_COMMAND_PAIR:
        succeeded = bluetooth_connect(runtime, peer_id);
        break;
    case BLUETOOTH_COMMAND_DISCONNECT:
    case BLUETOOTH_COMMAND_UNPAIR:
        succeeded = bluetooth_disconnect(runtime, peer_id);
        break;
    case BLUETOOTH_COMMAND_STATUS:
        succeeded = true;
        break;
    case BLUETOOTH_COMMAND_NONE:
    default:
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_INVALID_INPUT);
        return false;
    }

    if (!succeeded) {
        if (command == BLUETOOTH_COMMAND_CONNECT || command == BLUETOOTH_COMMAND_PAIR) {
            runtime->failed_connections++;
        } else if (command == BLUETOOTH_COMMAND_DISCONNECT ||
                   command == BLUETOOTH_COMMAND_UNPAIR) {
            runtime->failed_disconnections++;
        }
        return false;
    }

    runtime->command_count++;
    runtime->last_command = command;
    runtime->last_peer_id = peer_id;
    return true;
}

bool bluetooth_handle_command(bluetooth_runtime_t *runtime, const char *command, uint8_t peer_id) {
    if (runtime == NULL || command == NULL) {
        return false;
    }

    bluetooth_command_id_t command_id = BLUETOOTH_COMMAND_NONE;
    if (!bluetooth_command_from_string(command, &command_id)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_INVALID_INPUT);
        return false;
    }

    return bluetooth_execute_command(runtime, command_id, peer_id);
}

bool bluetooth_handle_pairing_button(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                     bool button_pressed) {
    if (runtime == NULL) {
        return false;
    }

    if (!button_pressed) {
        return false;
    }

    runtime->pairing_attempts++;
    runtime->pairing_in_progress = true;
    runtime->pairing_completed = false;
    runtime->pairing_error = false;
    runtime->storage_error = false;
    runtime->completed_pairing_peer_id = 0U;
    runtime->pairing_peer_id = peer_id;
    runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    runtime->last_error_peer_id = 0U;

    if (!bluetooth_runtime_has_transport(runtime)) {
        runtime->pairing_in_progress = false;
        runtime->pairing_error = true;
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_DISABLED);
        return false;
    }

    uint8_t selected_peer_id = peer_id;
    if (!bluetooth_classic_stack_select_pairing_candidate(&runtime->classic_stack,
                                                          &selected_peer_id)) {
        selected_peer_id = peer_id;
    }

    runtime->pairing_peer_id = selected_peer_id;
    if (!bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_PAIR, selected_peer_id)) {
        runtime->pairing_in_progress = false;
        runtime->pairing_error = true;
        bluetooth_record_error(runtime, selected_peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

#if !defined(PICO_INTERCOM_TARGET)
    bluetooth_mark_pairing_completed(runtime, selected_peer_id);
#endif
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    return true;
}

bool bluetooth_restore_pairing(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (!bluetooth_runtime_is_ready(runtime) || peer_id == 0U) {
        return false;
    }

    if (!bluetooth_classic_stack_restore_pairing(&runtime->classic_stack, peer_id)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_STORAGE);
        return false;
    }

    bluetooth_peer_link_t *link = bluetooth_get_peer_link(runtime, peer_id, true);
    if (link == NULL) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }
    link->remembered = true;
    link->link_state = BLUETOOTH_LINK_STATE_IDLE;
    bluetooth_sync_transport_counters(runtime);
    return true;
}

static void bluetooth_handle_protocol_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                                            const uint8_t *payload, size_t payload_len) {
    if (runtime == NULL || payload == NULL || payload_len == 0U) {
        return;
    }

    runtime->packets_received++;
    runtime->last_source_peer = source_peer;
    runtime->last_payload_len = payload_len;
    runtime->pending_relay_target_count = 0U;
    runtime->last_relay_source_peer = 0U;
    runtime->last_relay_target = 0U;
    runtime->last_relay_payload_len = 0U;
    runtime->last_relay_count = 0U;

    intercom_audio_frame_t decoded_frame;
    if (payload != NULL && payload_len > 0U &&
        intercom_audio_decode_frame(payload, payload_len, &decoded_frame)) {
        intercom_audio_note_decoded_frame(&runtime->audio);
        if (!intercom_audio_playback_frame(&runtime->audio, &decoded_frame)) {
            intercom_audio_note_playback_dropped(&runtime->audio);
        }
        (void)intercom_audio_drain_playback_queue(&runtime->audio);
    }

    if (runtime->intercom != NULL && payload != NULL && payload_len > 0U) {
        runtime->last_relay_count = intercom_rebroadcast(runtime->intercom, source_peer, payload,
                                                         payload_len, bluetooth_relay, runtime);
    }
    runtime->relay_target_count = runtime->pending_relay_target_count;
    bluetooth_flush_classic_packets(runtime);
    bluetooth_sync_transport_counters(runtime);
}

bool bluetooth_handle_transport_payload(bluetooth_runtime_t *runtime, uint8_t source_peer,
                                        const uint8_t *payload, size_t payload_len) {
    if (!bluetooth_runtime_has_transport(runtime) || payload == NULL || payload_len == 0U) {
        return false;
    }

    intercom_protocol_message_t message = {0};
    if (!intercom_protocol_decode(payload, payload_len, &message)) {
        runtime->protocol_messages_dropped++;
        bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
        (void)bluetooth_send_protocol_error(runtime, source_peer,
                                            INTERCOM_PROTOCOL_ERROR_DECODE, "invalid packet");
        return false;
    }

    if (message.source_peer != source_peer ||
        (message.target_peer != 0U && message.target_peer != runtime->local_peer_id)) {
        runtime->protocol_messages_dropped++;
        bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
        (void)bluetooth_send_protocol_error(runtime, source_peer,
                                            INTERCOM_PROTOCOL_ERROR_TARGET,
                                            "peer routing mismatch");
        return false;
    }

    bluetooth_peer_link_t *link = bluetooth_get_peer_link(runtime, source_peer, true);
    if (link == NULL) {
        runtime->protocol_messages_dropped++;
        bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    runtime->protocol_messages_received++;
    link->last_activity_ms = bluetooth_now_ms(runtime);
    if (message.sequence > link->last_rx_sequence) {
        link->last_rx_sequence = message.sequence;
    } else if (message.message_type == INTERCOM_PROTOCOL_MESSAGE_AUDIO) {
        link->dropped_messages++;
        runtime->protocol_messages_dropped++;
        return false;
    }

    switch (message.message_type) {
    case INTERCOM_PROTOCOL_MESSAGE_HELLO:
        if (message.session_id == 0U) {
            runtime->protocol_messages_dropped++;
            bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
            (void)bluetooth_send_protocol_error(runtime, source_peer,
                                                INTERCOM_PROTOCOL_ERROR_SESSION,
                                                "missing session id");
            return false;
        }
        link->session_id = message.session_id;
        link->hello_received = true;
        link->link_state = BLUETOOTH_LINK_STATE_HANDSHAKING;
        bluetooth_mark_session_ready(runtime, link, source_peer, link->session_id);
        printf("Bluetooth session hello received from peer %u.\n", (unsigned)source_peer);
        return bluetooth_send_hello(runtime, source_peer, INTERCOM_PROTOCOL_MESSAGE_HELLO_ACK);
    case INTERCOM_PROTOCOL_MESSAGE_HELLO_ACK:
        if (link->session_id == 0U || message.session_id != link->session_id) {
            runtime->protocol_messages_dropped++;
            bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
            return false;
        }
        bluetooth_mark_session_ready(runtime, link, source_peer, link->session_id);
        runtime->successful_connections++;
        printf("Bluetooth session established with peer %u.\n", (unsigned)source_peer);
        return true;
    case INTERCOM_PROTOCOL_MESSAGE_KEEPALIVE:
        if (link->session_id != message.session_id) {
            runtime->protocol_messages_dropped++;
            bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
            return false;
        }
        bluetooth_mark_session_ready(runtime, link, source_peer, link->session_id);
        return true;
    case INTERCOM_PROTOCOL_MESSAGE_AUDIO:
        if (!link->session_active || link->session_id != message.session_id) {
            runtime->protocol_messages_dropped++;
            bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
            (void)bluetooth_send_protocol_error(runtime, source_peer,
                                                INTERCOM_PROTOCOL_ERROR_SESSION,
                                                "session not ready");
            return false;
        }
        if (link->last_audio_sequence != 0U) {
            const uint16_t expected_next_sequence = (uint16_t)(link->last_audio_sequence + 1U);
            const uint16_t audio_gap =
                (uint16_t)((message.sequence - expected_next_sequence) & 0xFFFFU);
            if (audio_gap > 0U && audio_gap < 0x8000U) {
                link->missing_audio_frames += (uint32_t)audio_gap;
            }
        }
        link->last_audio_sequence = message.sequence;
        bluetooth_handle_protocol_audio(runtime, source_peer, message.payload, message.payload_len);
        return true;
    case INTERCOM_PROTOCOL_MESSAGE_ERROR:
        runtime->protocol_messages_dropped++;
        link->link_state = BLUETOOTH_LINK_STATE_DEGRADED;
        bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
        if (message.payload_len > 0U) {
            printf("Bluetooth peer %u reported protocol error %u (%s).\n",
                   (unsigned)source_peer, (unsigned)message.payload[0],
                   intercom_protocol_error_name((intercom_protocol_error_code_t)message.payload[0]));
        }
        return false;
    case INTERCOM_PROTOCOL_MESSAGE_GOODBYE:
        printf("Bluetooth peer %u ended the session.\n", (unsigned)source_peer);
        bluetooth_note_disconnected_link(runtime, source_peer);
        (void)bluetooth_classic_stack_disconnect(&runtime->classic_stack, source_peer);
        return true;
    case INTERCOM_PROTOCOL_MESSAGE_INVALID:
    default:
        runtime->protocol_messages_dropped++;
        bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
        return false;
    }
}

void bluetooth_poll(bluetooth_runtime_t *runtime) {
    if (!bluetooth_runtime_is_ready(runtime) || !runtime->enabled) {
        return;
    }

    bluetooth_advance_poll_clock(runtime);
    (void)bluetooth_classic_stack_poll(&runtime->classic_stack);
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    bluetooth_reconcile_peer_links(runtime);
    bluetooth_service_protocol_links(runtime);

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_classic_packet_t packet = {0};
    while (bluetooth_classic_stack_dequeue_packet(&runtime->classic_stack, &packet)) {
        (void)bluetooth_handle_transport_payload(runtime, packet.source_peer, packet.payload,
                                                 packet.payload_len);
        bluetooth_sync_transport_counters(runtime);
        bluetooth_sync_connected_peers(runtime);
        bluetooth_reconcile_peer_links(runtime);
    }
#endif
}

bool bluetooth_process_local_audio(bluetooth_runtime_t *runtime, uint8_t source_peer) {
    if (!bluetooth_runtime_is_operational(runtime)) {
        bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    intercom_audio_frame_t frame;
    if (!intercom_audio_capture_frame(&runtime->audio, &frame)) {
        return false;
    }

    uint8_t payload[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t payload_len = 0U;
    if (!intercom_audio_encode_frame(&frame, payload, sizeof(payload), &payload_len)) {
        bluetooth_record_error(runtime, source_peer, BLUETOOTH_ERROR_PROTOCOL);
        return false;
    }

    runtime->pending_relay_target_count = 0U;
    runtime->last_relay_count = 0U;
    runtime->last_relay_source_peer = 0U;
    runtime->last_relay_target = 0U;
    runtime->last_relay_payload_len = 0U;
    intercom_audio_note_encoded_frame(&runtime->audio);
    if (runtime->intercom != NULL) {
        runtime->last_relay_count = intercom_rebroadcast(runtime->intercom, source_peer, payload,
                                                         payload_len, bluetooth_relay, runtime);
    }
    runtime->relay_target_count = runtime->pending_relay_target_count;
    bluetooth_flush_classic_packets(runtime);
    bluetooth_sync_transport_counters(runtime);
    return runtime->last_relay_count > 0U;
}

void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                            const uint8_t *payload, size_t payload_len) {
    if (!bluetooth_runtime_has_transport(runtime)) {
        return;
    }

    bluetooth_handle_protocol_audio(runtime, source_peer, payload, payload_len);
}
