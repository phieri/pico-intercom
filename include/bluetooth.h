#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bluetooth_classic.h"
#include "intercom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLUETOOTH_COMMAND_NONE = 0,
    BLUETOOTH_COMMAND_ENABLE,
    BLUETOOTH_COMMAND_DISABLE,
    BLUETOOTH_COMMAND_TOGGLE,
    BLUETOOTH_COMMAND_CONNECT,
    BLUETOOTH_COMMAND_DISCONNECT,
    /* PAIR/UNPAIR are distinct enum values that are treated the same as
     * CONNECT/DISCONNECT in the command executor. */
    BLUETOOTH_COMMAND_PAIR,
    BLUETOOTH_COMMAND_UNPAIR,
    BLUETOOTH_COMMAND_STATUS
} bluetooth_command_id_t;

typedef enum {
    BLUETOOTH_PEER_STATE_DISCONNECTED = 0,
    BLUETOOTH_PEER_STATE_CONNECTING,
    BLUETOOTH_PEER_STATE_CONNECTED,
    BLUETOOTH_PEER_STATE_DISCONNECTING
} bluetooth_peer_state_t;

typedef struct {
    intercom_state_t *intercom;
    bool initialized;
    bool enabled;
    bool advertising;
    bool scanning;
    /* Tracks whether the target-specific wireless platform was initialized successfully. */
    bool platform_initialized;
    /* Tracks whether the target-specific wireless platform failed during startup. */
    bool platform_error;
    size_t last_relay_count;
    size_t relay_invocations;
    size_t packets_received;
    uint8_t last_source_peer;
    size_t last_payload_len;
    uint8_t last_relay_source_peer;
    uint8_t last_relay_target;
    uint8_t relay_targets[INTERCOM_MAX_PEERS];
    size_t relay_target_count;
    size_t pending_relay_target_count;
    uint8_t last_relay_payload[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN];
    size_t last_relay_payload_len;
    uint8_t connected_peers[INTERCOM_MAX_PEERS];
    bluetooth_peer_state_t peer_states[INTERCOM_MAX_PEERS];
    size_t connected_peer_count;
    size_t command_count;
    size_t pairing_attempts;
    size_t connection_attempts;
    size_t disconnect_attempts;
    size_t successful_connections;
    size_t successful_disconnections;
    size_t failed_connections;
    size_t failed_disconnections;
    bool pairing_in_progress;
    bool pairing_error;
    bool storage_error;
    uint8_t pairing_peer_id;
    uint8_t last_error_peer_id;
    uint32_t last_error_code;
    bluetooth_command_id_t last_command;
    uint8_t last_peer_id;
    size_t transport_packets_queued;
    size_t transport_packets_delivered;
    size_t transport_packets_dropped;
    uint8_t last_transport_source_peer;
    uint8_t last_transport_target_peer;
    bluetooth_classic_stack_t classic_stack;
} bluetooth_runtime_t;

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom);
bool bluetooth_set_enabled(bluetooth_runtime_t *runtime, bool enabled);
bool bluetooth_enable(bluetooth_runtime_t *runtime);
bool bluetooth_disable(bluetooth_runtime_t *runtime);
bool bluetooth_toggle(bluetooth_runtime_t *runtime);
bool bluetooth_runtime_is_operational(const bluetooth_runtime_t *runtime);
bool bluetooth_is_enabled(const bluetooth_runtime_t *runtime);
bool bluetooth_connect(bluetooth_runtime_t *runtime, uint8_t peer_id);
bool bluetooth_disconnect(bluetooth_runtime_t *runtime, uint8_t peer_id);
bool bluetooth_connect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id);
bool bluetooth_disconnect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id);
bool bluetooth_is_peer_connected(const bluetooth_runtime_t *runtime, uint8_t peer_id);
bool bluetooth_get_peer_state(const bluetooth_runtime_t *runtime, uint8_t peer_id,
                              bluetooth_peer_state_t *state);
const char *bluetooth_peer_state_name(bluetooth_peer_state_t state);
const char *bluetooth_error_name(uint32_t error_code);
bool bluetooth_execute_command(bluetooth_runtime_t *runtime, bluetooth_command_id_t command,
                              uint8_t peer_id);
bool bluetooth_handle_command(bluetooth_runtime_t *runtime, const char *command,
                              uint8_t peer_id);
bool bluetooth_handle_pairing_button(bluetooth_runtime_t *runtime, uint8_t peer_id,
                                    bool button_pressed);
void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
