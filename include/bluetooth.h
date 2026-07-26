#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intercom.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN 256U

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
    size_t connected_peer_count;
    size_t command_count;
    bluetooth_command_id_t last_command;
    uint8_t last_peer_id;
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
bool bluetooth_execute_command(bluetooth_runtime_t *runtime, bluetooth_command_id_t command,
                              uint8_t peer_id);
bool bluetooth_handle_command(bluetooth_runtime_t *runtime, const char *command,
                              uint8_t peer_id);
void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
