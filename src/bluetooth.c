#include "bluetooth.h"

#include <string.h>

static void bluetooth_relay(void *context, uint8_t source_peer, uint8_t target_peer,
                            const uint8_t *payload, size_t payload_len) {
    bluetooth_runtime_t *runtime = (bluetooth_runtime_t *)context;
    (void)source_peer;
    (void)target_peer;
    (void)payload;
    (void)payload_len;

    if (runtime != NULL) {
        runtime->last_relay_count++;
    }
}

void bluetooth_init(bluetooth_runtime_t *runtime, intercom_state_t *intercom) {
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->intercom = intercom;
    runtime->initialized = true;
}

void bluetooth_handle_audio(bluetooth_runtime_t *runtime, uint8_t source_peer,
                           const uint8_t *payload, size_t payload_len) {
    if (runtime == NULL || !runtime->initialized || runtime->intercom == NULL) {
        return;
    }

    runtime->packets_received++;
    runtime->last_source_peer = source_peer;
    runtime->last_payload_len = payload_len;
    runtime->last_relay_count = 0U;

    runtime->last_relay_count = intercom_rebroadcast(runtime->intercom, source_peer,
                                                   payload, payload_len,
                                                   bluetooth_relay, runtime);
}
