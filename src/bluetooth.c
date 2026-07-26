#include "bluetooth.h"

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
    return runtime != NULL && runtime->initialized && runtime->enabled && !runtime->platform_error;
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

static void bluetooth_relay(void *context, uint8_t source_peer, uint8_t target_peer,
                            const uint8_t *payload, size_t payload_len) {
    bluetooth_record_relay((bluetooth_runtime_t *)context, source_peer, target_peer, payload,
                           payload_len);
}

static void bluetooth_reset_peer_states(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        runtime->peer_states[index] = BLUETOOTH_PEER_STATE_DISCONNECTED;
    }
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
    runtime->platform_initialized = true;
    runtime->platform_error = false;
    runtime->initialized = true;
    bluetooth_reset_peer_states(runtime);
}

bool bluetooth_set_enabled(bluetooth_runtime_t *runtime, bool enabled) {
    if (!bluetooth_runtime_is_ready(runtime)) {
        return false;
    }

    runtime->enabled = enabled;
    if (!enabled) {
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_DISABLED);
    } else {
        runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    }
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

    runtime->enabled = !runtime->enabled;
    if (!runtime->enabled) {
        bluetooth_record_error(runtime, 0U, BLUETOOTH_ERROR_DISABLED);
    } else {
        runtime->last_error_code = BLUETOOTH_ERROR_NONE;
    }
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

    if (runtime->intercom != NULL && !intercom_add_peer(runtime->intercom, peer_id)) {
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
    return true;
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

    if (runtime->intercom != NULL) {
        intercom_remove_peer(runtime->intercom, peer_id);
    }

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

    const bool connected = bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_PAIR, peer_id);
    if (!connected) {
        runtime->pairing_in_progress = false;
        runtime->pairing_error = true;
        bluetooth_record_error(runtime, peer_id, BLUETOOTH_ERROR_PEER_LIMIT);
        return false;
    }

    runtime->pairing_in_progress = false;
    runtime->pairing_error = false;
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
    if (runtime->intercom != NULL && payload != NULL && payload_len > 0U) {
        runtime->last_relay_count = intercom_rebroadcast(
            runtime->intercom, source_peer, payload, payload_len, bluetooth_relay, runtime);
    }
    runtime->relay_target_count = runtime->pending_relay_target_count;
}
