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

static int remove_test_file(const char *path) {
    if (remove(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "remove(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    return 0;
}

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
    assert(bluetooth_is_enabled(&runtime));
    assert(bluetooth_disable(&runtime));
    assert(!bluetooth_is_enabled(&runtime));
    assert(bluetooth_toggle(&runtime));
    assert(bluetooth_is_enabled(&runtime));
    assert(bluetooth_execute_command(&runtime, BLUETOOTH_COMMAND_CONNECT, 4U));
    assert(bluetooth_is_peer_connected(&runtime, 4U));
    assert(runtime.command_count == 1U);
    assert(runtime.last_command == BLUETOOTH_COMMAND_CONNECT);
    assert(runtime.last_peer_id == 4U);
    assert(bluetooth_handle_command(&runtime, "disconnect", 4U));
    assert(!bluetooth_is_peer_connected(&runtime, 4U));
    assert(runtime.command_count == 2U);
    assert(runtime.last_command == BLUETOOTH_COMMAND_DISCONNECT);
    assert(runtime.last_peer_id == 4U);
    assert(bluetooth_handle_command(&runtime, "status", 0U));
    assert(runtime.command_count == 3U);
    assert(runtime.last_command == BLUETOOTH_COMMAND_STATUS);
    assert(runtime.last_peer_id == 0U);
    assert(bluetooth_is_enabled(&runtime));
    bluetooth_handle_audio(&runtime, 1U, payload, sizeof(payload));
    assert(runtime.packets_received == 1U);
    assert(runtime.last_source_peer == 1U);
    assert(runtime.last_payload_len == sizeof(payload));
    assert(runtime.last_relay_count == 2U);
    assert(runtime.relay_invocations == 2U);
    assert(runtime.relay_target_count == 2U);
    assert(runtime.relay_targets[0] == 2U);
    assert(runtime.relay_targets[1] == 3U);
    assert(runtime.last_relay_source_peer == 1U);
    assert(runtime.last_relay_target == 3U);
    assert(runtime.last_relay_payload_len == sizeof(payload));
    assert(memcmp(runtime.last_relay_payload, payload, sizeof(payload)) == 0);

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
    assert(!bluetooth_disconnect_peer(&runtime, 99U));
    assert(runtime.connected_peer_count == 2U);

    intercom_state_t limit_state;
    bluetooth_runtime_t limit_runtime = {0};
    intercom_init(&limit_state);
    bluetooth_init(&limit_runtime, &limit_state);
    for (uint8_t peer_id = 1U; peer_id <= INTERCOM_MAX_PEERS; ++peer_id) {
        assert(bluetooth_connect_peer(&limit_runtime, peer_id));
    }
    assert(limit_runtime.connected_peer_count == INTERCOM_MAX_PEERS);
    assert(!bluetooth_connect_peer(&limit_runtime, 9U));

    pairing_store_t store = {0};
    pairing_t persisted[8] = {{0}};
    pairing_t new_pairing = {.peer_id = 4U, .name = "headset-4"};
    pairing_t duplicate_pairing = {.peer_id = 4U, .name = "headset-4"};
    size_t persisted_count = 0U;

    if (remove("pairings_test.txt") != 0 && errno != ENOENT) {
        assert(false);
    }

    assert(pairing_store_init(&store, "pairings_test.txt"));
    assert(pairing_store_clear(&store));
    assert(pairing_store_save(&store, &new_pairing));
    assert(pairing_store_save(&store, &duplicate_pairing));
    persisted_count = 0U;
    assert(pairing_store_load(&store, persisted, &persisted_count));
    assert(persisted_count == 1U);
    assert(persisted[0].peer_id == 4U);
    assert(strcmp(persisted[0].name, "headset-4") == 0);
    if (remove_test_file("pairings_test.txt") != 0) {
        return 1;
    }

    pairing_store_t missing_store = {0};
    pairing_t empty_pairings[8] = {{0}};
    size_t empty_count = 0U;
    if (remove("missing_pairings_test.txt") != 0 && errno != ENOENT) {
        assert(false);
    }
    assert(pairing_store_init(&missing_store, "missing_pairings_test.txt"));
    assert(pairing_store_load(&missing_store, empty_pairings, &empty_count));
    assert(empty_count == 0U);

    pairing_store_t crlf_store = {0};
    pairing_t crlf_pairings[8] = {{0}};
    size_t crlf_count = 0U;
    FILE *crlf_handle = fopen("crlf_pairings_test.txt", "w");
    assert(crlf_handle != NULL);
    assert(fprintf(crlf_handle, "8,headset-8\r\n") >= 0);
    fclose(crlf_handle);

    assert(pairing_store_init(&crlf_store, "crlf_pairings_test.txt"));
    assert(pairing_store_load(&crlf_store, crlf_pairings, &crlf_count));
    assert(crlf_count == 1U);
    assert(crlf_pairings[0].peer_id == 8U);
    assert(strcmp(crlf_pairings[0].name, "headset-8") == 0);
    if (remove_test_file("crlf_pairings_test.txt") != 0) {
        return 1;
    }

    return 0;
}
