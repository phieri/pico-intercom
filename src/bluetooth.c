#include "bluetooth.h"

#include <stdio.h>
#include <string.h>

static bool bluetooth_is_case_insensitive_match(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        unsigned char left_char = (unsigned char)*left;
        unsigned char right_char = (unsigned char)*right;
        if (left_char >= 'A' && left_char <= 'Z') {
            left_char = (unsigned char)(left_char - 'A' + 'a');
        }
        if (right_char >= 'A' && right_char <= 'Z') {
            right_char = (unsigned char)(right_char - 'A' + 'a');
        }
        if (left_char != right_char) {
            return false;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
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
    runtime->enabled = true;
    runtime->advertising = true;
    runtime->scanning = true;
    runtime->initialized = true;
}

bool bluetooth_set_enabled(bluetooth_runtime_t *runtime, bool enabled) {
    if (runtime == NULL) {
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

bool bluetooth_command_enable(bluetooth_runtime_t *runtime) {
    return bluetooth_enable(runtime);
}

bool bluetooth_command_disable(bluetooth_runtime_t *runtime) {
    return bluetooth_disable(runtime);
}

bool bluetooth_command_toggle(bluetooth_runtime_t *runtime) {
    return bluetooth_toggle(runtime);
}

bool bluetooth_command_connect(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_connect(runtime, peer_id);
}

bool bluetooth_command_disconnect(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    return bluetooth_disconnect(runtime, peer_id);
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

bool bluetooth_execute_command(bluetooth_runtime_t *runtime, bluetooth_command_id_t command,
                              uint8_t peer_id) {
    if (runtime == NULL) {
        return false;
    }

    runtime->command_count++;
    runtime->last_command = command;
    runtime->last_peer_id = peer_id;

    switch (command) {
    case BLUETOOTH_COMMAND_ENABLE:
        return bluetooth_enable(runtime);
    case BLUETOOTH_COMMAND_DISABLE:
        return bluetooth_disable(runtime);
    case BLUETOOTH_COMMAND_TOGGLE:
        return bluetooth_toggle(runtime);
    case BLUETOOTH_COMMAND_CONNECT:
    case BLUETOOTH_COMMAND_PAIR:
        return bluetooth_connect(runtime, peer_id);
    case BLUETOOTH_COMMAND_DISCONNECT:
    case BLUETOOTH_COMMAND_UNPAIR:
        return bluetooth_disconnect(runtime, peer_id);
    case BLUETOOTH_COMMAND_STATUS:
        return bluetooth_is_enabled(runtime);
    case BLUETOOTH_COMMAND_NONE:
    default:
        return false;
    }
}

bool bluetooth_handle_command(bluetooth_runtime_t *runtime, const char *command, uint8_t peer_id) {
    if (runtime == NULL || command == NULL) {
        return false;
    }

    if (bluetooth_is_case_insensitive_match(command, "enable") ||
        bluetooth_is_case_insensitive_match(command, "on") ||
        bluetooth_is_case_insensitive_match(command, "power_on")) {
        return bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_ENABLE, peer_id);
    }

    if (bluetooth_is_case_insensitive_match(command, "disable") ||
        bluetooth_is_case_insensitive_match(command, "off") ||
        bluetooth_is_case_insensitive_match(command, "power_off")) {
        return bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_DISABLE, peer_id);
    }

    if (bluetooth_is_case_insensitive_match(command, "toggle") ||
        bluetooth_is_case_insensitive_match(command, "switch")) {
        return bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_TOGGLE, peer_id);
    }

    if (bluetooth_is_case_insensitive_match(command, "connect") ||
        bluetooth_is_case_insensitive_match(command, "pair")) {
        return bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_CONNECT, peer_id);
    }

    if (bluetooth_is_case_insensitive_match(command, "disconnect") ||
        bluetooth_is_case_insensitive_match(command, "unpair")) {
        return bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_DISCONNECT, peer_id);
    }

    if (bluetooth_is_case_insensitive_match(command, "status") ||
        bluetooth_is_case_insensitive_match(command, "info")) {
        return bluetooth_execute_command(runtime, BLUETOOTH_COMMAND_STATUS, peer_id);
    }

    return false;
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
