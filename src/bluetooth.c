#include "bluetooth.h"

#ifdef PICO_INTERCOM_TARGET
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#endif

#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef PICO_INTERCOM_TARGET
static bool bluetooth_platform_init(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return false;
    }

    if (cyw43_arch_init() != 0) {
        fprintf(stderr, "WARNING: failed to initialize CYW43 architecture for Pico wireless support\n");
        runtime->platform_error = true;
        return false;
    }

    if (!btstack_cyw43_init(NULL)) {
        fprintf(stderr, "WARNING: failed to initialize BTstack CYW43 integration\n");
        runtime->platform_error = true;
        return false;
    }

    runtime->platform_initialized = true;
    return true;
}
#endif

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

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom) {
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->intercom = intercom;

#ifdef PICO_INTERCOM_TARGET
    {
        bool platform_ready = bluetooth_platform_init(runtime);
        if (!platform_ready) {
            return;
        }
    }
#endif

    runtime->enabled = true;
    runtime->advertising = true;
    runtime->scanning = true;
    runtime->initialized = true;
}

bool bluetooth_set_enabled(bluetooth_runtime_t *runtime, bool enabled) {
    bool runtime_ready = runtime != NULL && (runtime->initialized || runtime->platform_initialized);
    if (!runtime_ready) {
        return false;
    }

    runtime->enabled = enabled;
    return true;
}

bool bluetooth_enable(bluetooth_runtime_t *runtime) {
    return bluetooth_set_enabled(runtime, true);
}

bool bluetooth_disable(bluetooth_runtime_t *runtime) {
    return bluetooth_set_enabled(runtime, false);
}

bool bluetooth_toggle(bluetooth_runtime_t *runtime) {
    if (runtime == NULL) {
        return false;
    }

    runtime->enabled = !runtime->enabled;
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
    if (runtime == NULL || !runtime->initialized) {
        return false;
    }

    if (bluetooth_has_peer(runtime, peer_id)) {
        return true;
    }

    if (runtime->connected_peer_count >= INTERCOM_MAX_PEERS) {
        return false;
    }

    if (runtime->intercom != NULL && !intercom_add_peer(runtime->intercom, peer_id)) {
        return false;
    }

    runtime->connected_peers[runtime->connected_peer_count++] = peer_id;
    return true;
}

bool bluetooth_disconnect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    if (runtime == NULL || !runtime->initialized) {
        return false;
    }

    size_t peer_index = bluetooth_find_peer_index(runtime, peer_id);
    if (peer_index >= runtime->connected_peer_count) {
        return false;
    }

    for (size_t shift = peer_index + 1; shift < runtime->connected_peer_count; ++shift) {
        runtime->connected_peers[shift - 1] = runtime->connected_peers[shift];
    }
    runtime->connected_peer_count--;

    if (runtime->intercom != NULL) {
        intercom_remove_peer(runtime->intercom, peer_id);
    }

    return true;
}

bool bluetooth_is_peer_connected(const bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_has_peer(runtime, peer_id);
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
        return false;
    }

    if (!succeeded) {
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
        return false;
    }

    return bluetooth_execute_command(runtime, command_id, peer_id);
}

void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len) {
    if (runtime == NULL || !runtime->initialized || runtime->intercom == NULL) {
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

    runtime->last_relay_count = intercom_rebroadcast(
        runtime->intercom, source_peer, payload, payload_len, bluetooth_relay, runtime);
    runtime->relay_target_count = runtime->pending_relay_target_count;
}
