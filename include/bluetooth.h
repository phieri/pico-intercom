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
} bluetooth_runtime_t;

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom);
void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
