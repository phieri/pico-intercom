#include "bluetooth.h"

#if defined(PICO_INTERCOM_TARGET)
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#endif

#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *alias;
    bluetooth_command_id_t command_id;
} bluetooth_command_alias_t;

#define BLUETOOTH_COMMAND_ALIAS_COUNT ((size_t)(sizeof(bluetooth_command_aliases) / sizeof(bluetooth_command_aliases[0])))

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
    BLUETOOTH_ERROR_NOT_READY = 5u
};

/**
 * Parse a human-readable Bluetooth command into an internal command ID.
 *
 * Supported aliases include enable/on/power_on, disable/off/power_off,
 * toggle/switch, connect/pair, disconnect/unpair, and status/info.
 */
static bool bluetooth_command_from_string(const char *command, bluetooth_command_id_t *command_id) {
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

bool bluetooth_runtime_is_operational(const bluetooth_runtime_t *runtime) {
    return runtime != NULL && runtime->initialized && runtime->enabled &&
           runtime->platform_initialized && !runtime->platform_error &&
           runtime->classic_stack.transport.backend_ready;
}

static bool bluetooth_has_pending_target(const bluetooth_runtime_t *runtime, uint8_t target_peer) {
    for (size_t index = 0; index < runtime->pending_relay_target_count; ++index) {
        if (runtime->relay_targets[index] == target_peer) {
            return true;
        }
    }

    return false;
}

static void bluetooth_record_error(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                   uint32_t error_code) {
    if (runtime == NULL) {
        return;
    }

    runtime->last_error_peer_id = peer_id;
    runtime->last_error_code = error_code;
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

    runtime->last_relay_source_peer = source_peer;
    runtime->last_relay_target = target_peer;

    if (runtime->pending_relay_target_count < INTERCOM_MAX_PEERS) {
        runtime->relay_targets[runtime->pending_relay_target_count] = target_peer;
        runtime->pending_relay_target_count++;
    } else {
        fprintf(stderr,
                "WARNING: bluetooth relay target tracking limit (%u) reached, cannot record peer %u\n",
                (unsigned)INTERCOM_MAX_PEERS, (unsigned)target_peer);
        return;
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

static void bluetooth_sync_connected_peers(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

#if defined(PICO_INTERCOM_TARGET)
    runtime->connected_peer_count = runtime->classic_stack.transport.connected_peer_count;
    for (size_t index = 0; index < runtime->connected_peer_count; ++index) {
        runtime->connected_peers[index] = runtime->classic_stack.transport.connected_peers[index];
        runtime->peer_states[index] = BLUETOOTH_PEER_STATE_CONNECTED;
    }
    for (size_t index = runtime->connected_peer_count; index < INTERCOM_MAX_PEERS; ++index) {
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
             * before we compress the result into the firmware's 1-250 peer-ID range. */
            crc = (crc & 0x80U) != 0U ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    /* Keep peer IDs in 1-250 and leave the top values free for future protocol sentinels. */
    return (uint8_t)((crc % 250U) + 1U);
#else
    return 1U;
#endif
}

static void bluetooth_relay(void *context, uint8_t source_peer, uint8_t target_peer,
                            const uint8_t *payload, size_t payload_len) {
    bluetooth_runtime_t *runtime = (bluetooth_runtime_t *)context;
    bluetooth_record_relay(runtime, source_peer, target_peer, payload, payload_len);

    if (!bluetooth_classic_stack_queue_packet(&runtime->classic_stack, source_peer, target_peer,
                                              payload, payload_len)) {
        runtime->transport_packets_dropped++;
    } else {
        runtime->transport_packets_queued++;
    }
}

static void bluetooth_reset_peer_states(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
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
        fprintf(stderr, "Bluetooth backend init failed: cyw43_arch_init returned %d\n", cyw43_status);
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
    runtime->platform_error = false;
    runtime->initialized = true;
    intercom_audio_init(&runtime->audio);
    bluetooth_classic_stack_init(&runtime->classic_stack);
    bluetooth_classic_stack_set_local_peer_id(&runtime->classic_stack, runtime->local_peer_id);
    bluetooth_reset_peer_states(runtime);
    if (!bluetooth_platform_set_enabled(runtime, runtime->enabled) ||
        !bluetooth_classic_stack_set_enabled(&runtime->classic_stack, runtime->enabled)) {
        runtime->platform_error = true;
        runtime->enabled = false;
    }
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
}

bool bluetooth_set_enabled(bluetooth_runtime_t *runtime, bool enabled) {
    if (!bluetooth_runtime_is_ready(runtime)) {
        return false;
    }

    if (!bluetooth_platform_set_enabled(runtime, enabled)) {
        runtime->enabled = false;
        bluetooth_classic_stack_set_enabled(&runtime->classic_stack, false);
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
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_DISABLED);
    } else {
        runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    }
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    return true;
}

bool bluetooth_enable(bluetooth_runtime_t *runtime) {
    return bluetooth_set_enabled(runtime, true);
}

bool bluetooth_disable(bluetooth_runtime_t *runtime) {
    return bluetooth_set_enabled(runtime, false);
}

bool bluetooth_toggle(bluetooth_runtime_t *runtime) {
    if (!bluetooth_runtime_is_ready(runtime)) {
        return false;
    }

    const bool requested_enabled = !runtime->enabled;
    if (!bluetooth_platform_set_enabled(runtime, requested_enabled)) {
        runtime->enabled = false;
        intercom_audio_set_enabled(&runtime->audio, false);
        bluetooth_classic_stack_set_enabled(&runtime->classic_stack, false);
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_DISABLED);
        return false;
    }

    runtime->enabled = requested_enabled;
    intercom_audio_set_enabled(&runtime->audio, runtime->enabled);
    if (!bluetooth_classic_stack_set_enabled(&runtime->classic_stack, runtime->enabled)) {
        runtime->platform_error = true;
        runtime->enabled = false;
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }
    if (!runtime->enabled) {
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_DISABLED);
    } else {
        runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    }
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    return runtime->enabled;
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
    if (!bluetooth_runtime_is_ready(runtime)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    if (!runtime->enabled) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_DISABLED);
        return false;
    }

    if (bluetooth_has_peer(runtime, peer_id)) {
        return true;
    }

    if (runtime->connected_peer_count >= INTERCOM_MAX_PEERS) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    if (!bluetooth_classic_stack_connect(&runtime->classic_stack, peer_id)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

#if defined(PICO_INTERCOM_TARGET)
    runtime->connection_attempts++;
    runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    bluetooth_sync_connected_peers(runtime);
    bluetooth_sync_transport_counters(runtime);
    return true;
#else
    if (runtime->intercom != NULL && !intercom_add_peer(runtime->intercom, peer_id)) {
        if (!bluetooth_classic_stack_disconnect(&runtime->classic_stack, peer_id)) {
            runtime->failed_disconnections++;
            bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        }
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    const size_t peer_index = runtime->connected_peer_count;
    runtime->connected_peers[peer_index] = peer_id;
    runtime->connected_peer_count++;
    runtime->connection_attempts++;
    runtime->successful_connections++;
    runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    runtime->peer_states[peer_index] = BLUETOOTH_PEER_STATE_CONNECTED;
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
    return true;
#endif
}

bool bluetooth_disconnect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (!bluetooth_runtime_is_ready(runtime)) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_NOT_READY);
        return false;
    }

    size_t peer_index = bluetooth_find_peer_index(runtime, peer_id);
    if (peer_index >= runtime->connected_peer_count) {
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_INVALID_INPUT);
        return false;
    }

    for (size_t shift = peer_index + 1; shift < runtime->connected_peer_count; ++shift) {
        runtime->connected_peers[shift - 1] = runtime->connected_peers[shift];
        runtime->peer_states[shift - 1] = runtime->peer_states[shift];
    }
    runtime->connected_peer_count--;
    if (runtime->connected_peer_count < INTERCOM_MAX_PEERS) {
        /* Keep the tail slot clear after removing the final connected peer. */
        runtime->peer_states[runtime->connected_peer_count] = BLUETOOTH_PEER_STATE_DISCONNECTED;
    }

    runtime->disconnect_attempts++;
    runtime->successful_disconnections++;
    runtime->last_error_code = BLUETOOTH_ERROR_NONE;

    (void)bluetooth_classic_stack_disconnect(&runtime->classic_stack, peer_id);

    if (runtime->intercom != NULL) {
        intercom_remove_peer(runtime->intercom, peer_id);
    }

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

/**
 * Execute a Bluetooth control command and record the resulting runtime state.
 *
 * Successful commands update the runtime's command history and last-command
 * fields. Invalid commands or failed state transitions return false.
 */
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
        /* Fall through to the PAIR alias handling. */
    case BLUETOOTH_COMMAND_PAIR:
        succeeded = bluetooth_connect(runtime, peer_id);
        break;
    case BLUETOOTH_COMMAND_DISCONNECT:
        /* Fall through to the UNPAIR alias handling. */
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
        } else if (command == BLUETOOTH_COMMAND_DISCONNECT || command == BLUETOOTH_COMMAND_UNPAIR) {
            runtime->failed_disconnections++;
        }
        return false;
    }

    runtime->command_count++;
    runtime->last_command = command;
    runtime->last_peer_id = peer_id;
    return true;
}

/**
 * Handle a string-based Bluetooth command by translating it to an internal
 * command ID and executing the corresponding control action.
 */
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
        runtime->pairing_in_progress = false;
        return false;
    }

    runtime->pairing_attempts++;
    runtime->pairing_in_progress = true;
    runtime->pairing_error = false;
    runtime->storage_error = false;
    runtime->pairing_peer_id = peer_id;
    runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    runtime->last_error_peer_id = 0U;

    if (!bluetooth_runtime_is_operational(runtime)) {
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

    const bool connected =
        bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_PAIR, selected_peer_id);
    if (!connected) {
        runtime->pairing_in_progress = false;
        runtime->pairing_error = true;
        bluetooth_record_error(runtime, selected_peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    runtime->pairing_in_progress = false;
    runtime->pairing_error = false;
    runtime->pairing_peer_id = selected_peer_id;
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

    bluetooth_sync_transport_counters(runtime);
    return true;
}

void bluetooth_poll(bluetooth_runtime_t *runtime) {
    if (!bluetooth_runtime_is_ready(runtime) || !runtime->enabled) {
        return;
    }

    (void)bluetooth_classic_stack_poll(&runtime->classic_stack);
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);

#if defined(PICO_INTERCOM_TARGET)
    bluetooth_classic_packet_t packet = {0};
    while (bluetooth_classic_stack_dequeue_packet(&runtime->classic_stack, &packet)) {
        if (runtime->intercom != NULL) {
            (void)intercom_add_peer(runtime->intercom, packet.source_peer);
        }
        bluetooth_handle_audio(runtime, packet.source_peer, packet.payload, packet.payload_len);
        bluetooth_sync_transport_counters(runtime);
        bluetooth_sync_connected_peers(runtime);
    }
#endif
}

bool bluetooth_process_local_audio(bluetooth_runtime_t *runtime, uint8_t source_peer) {
    if (!bluetooth_runtime_is_operational(runtime)) {
        return false;
    }

    intercom_audio_frame_t frame;
    if (!intercom_audio_capture_frame(&runtime->audio, &frame)) {
        return false;
    }

    uint8_t payload[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t payload_len = 0U;
    if (!intercom_audio_encode_frame(&frame, payload, sizeof(payload), &payload_len)) {
        return false;
    }

    intercom_audio_note_encoded_frame(&runtime->audio);
    bluetooth_handle_audio(runtime, source_peer, payload, payload_len);
    return true;
}

void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                            const uint8_t *payload, size_t payload_len) {
    if (!bluetooth_runtime_is_operational(runtime)) {
        return;
    }

    /* Fresh audio can still be relayed even if the runtime cannot register the source
     * peer because the peer limit has already been reached; the relay loop still uses the
     * existing intercom peers. */
    (void)bluetooth_connect_peer(runtime, source_peer);

    runtime->packets_received++;
    runtime->last_source_peer = source_peer;
    runtime->last_payload_len = payload_len;
    runtime->pending_relay_target_count = 0U;
    runtime->last_relay_source_peer = 0U;
    runtime->last_relay_target = 0U;
    runtime->last_relay_payload_len = 0U;

    runtime->last_relay_count = 0U;
    if (runtime->intercom != NULL) {
        /* Reconnect any peers that are already known to the intercom core so the
         * relay path can recover from earlier setup gaps without dropping the audio burst. */
        for (size_t index = 0; index < runtime->intercom->peer_count; ++index) {
            const uint8_t intercom_peer = runtime->intercom->peers[index];
            if (!bluetooth_classic_stack_connect(&runtime->classic_stack, intercom_peer)) {
                runtime->failed_connections++;
                bluetooth_record_error(runtime, intercom_peer, BLUETOOTH_ERROR_NOT_READY);
            }
        }
    }

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
        runtime->last_relay_count = intercom_rebroadcast(
            runtime->intercom, source_peer, payload, payload_len, bluetooth_relay, runtime);
    }
    runtime->relay_target_count = runtime->pending_relay_target_count;
    bluetooth_flush_classic_packets(runtime);
    bluetooth_sync_transport_counters(runtime);
    bluetooth_sync_connected_peers(runtime);
}
