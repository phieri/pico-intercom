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

typedef struct {
    intercom_state_t *intercom;
    bool initialized;
    size_t last_relay_count;
    size_t relay_invocations;
    size_t packets_received;
    uint8_t last_source_peer;
    size_t last_payload_len;
    uint8_t last_relay_source_peer;
    uint8_t last_relay_target;
    uint8_t relay_targets[INTERCOM_MAX_PEERS];
    size_t relay_target_count;
    uint8_t last_relay_payload[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN];
    size_t last_relay_payload_len;
    uint8_t connected_peers[INTERCOM_MAX_PEERS];
    size_t connected_peer_count;
} bluetooth_runtime_t;

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom);
bool bluetooth_connect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id);
bool bluetooth_disconnect_peer(bluetooth_runtime_t *runtime, uint8_t peer_id);
bool bluetooth_is_peer_connected(const bluetooth_runtime_t *runtime, uint8_t peer_id);
void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
