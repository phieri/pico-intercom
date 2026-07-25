#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intercom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    intercom_state_t *intercom;
    bool initialized;
    size_t last_relay_count;
    size_t relay_invocations;
    size_t packets_received;
    uint8_t last_source_peer;
    size_t last_payload_len;
} bluetooth_runtime_t;

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom);
void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
