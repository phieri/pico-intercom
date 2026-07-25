#include "bluetooth.h"
#include "intercom.h"
#include "pairings.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct relay_context {
    size_t calls;
    uint8_t targets[INTERCOM_MAX_PEERS];
};

static void relay_callback(void *context, uint8_t source_peer, uint8_t target_peer,
                           const uint8_t *payload, size_t payload_len) {
    struct relay_context *ctx = (struct relay_context *)context;
    ctx->targets[ctx->calls] = target_peer;
    ctx->calls++;

    (void)source_peer;
    (void)payload;
    (void)payload_len;
}

int main(void) {
    intercom_state_t state;
    bluetooth_runtime_t runtime = {0};
    struct relay_context ctx = {0};
    static const uint8_t payload[] = {0x01, 0x02, 0x03};

    intercom_init(&state);
    intercom_enable(&state, true);
    assert(intercom_add_peer(&state, 2U));
    assert(intercom_add_peer(&state, 3U));
    intercom_set_ptt(&state, true);

    size_t relayed = intercom_rebroadcast(&state, 1U, payload, sizeof(payload),
                                         relay_callback, &ctx);
    assert(relayed == 2U);
    assert(ctx.calls == 2U);
    assert(ctx.targets[0] == 2U);
    assert(ctx.targets[1] == 3U);

    intercom_set_ptt(&state, false);
    relayed = intercom_rebroadcast(&state, 1U, payload, sizeof(payload), relay_callback,
                                  &ctx);
    assert(relayed == 0U);
    assert(ctx.calls == 2U);

    intercom_set_ptt(&state, true);
    bluetooth_init(&runtime, &state);
    bluetooth_handle_audio(&runtime, 1U, payload, sizeof(payload));
    assert(runtime.packets_received == 1U);
    assert(runtime.last_source_peer == 1U);
    assert(runtime.last_payload_len == sizeof(payload));
    assert(runtime.last_relay_count == 2U);
    assert(runtime.relay_invocations == 2U);

    intercom_set_ptt(&state, false);
    bluetooth_handle_audio(&runtime, 2U, payload, sizeof(payload));
    assert(runtime.packets_received == 2U);
    assert(runtime.last_relay_count == 0U);
    assert(runtime.relay_invocations == 2U);
    assert(runtime.connected_peer_count == 2U);

    assert(bluetooth_connect_peer(&runtime, 4U));
    assert(bluetooth_is_peer_connected(&runtime, 4U));
    assert(runtime.connected_peer_count == 3U);
    assert(bluetooth_connect_peer(&runtime, 4U));
    assert(runtime.connected_peer_count == 3U);
    assert(bluetooth_disconnect_peer(&runtime, 4U));
    assert(!bluetooth_is_peer_connected(&runtime, 4U));

    pairing_store_t store = {0};
    pairing_t persisted[8] = {{0}};
    pairing_t new_pairing = {.peer_id = 4U, .name = "headset-4"};
    size_t persisted_count = 0U;

    if (remove("pairings_test.txt") != 0 && errno != ENOENT) {
        assert(false);
    }

    assert(pairing_store_init(&store, "pairings_test.txt"));
    assert(pairing_store_clear(&store));
    assert(pairing_store_save(&store, &new_pairing));
    persisted_count = 0U;
    assert(pairing_store_load(&store, persisted, &persisted_count));
    assert(persisted_count == 1U);
    assert(persisted[0].peer_id == 4U);
    assert(strcmp(persisted[0].name, "headset-4") == 0);

    return 0;
}
