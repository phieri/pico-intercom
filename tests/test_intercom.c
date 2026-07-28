#include "audio.h"
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

typedef struct {
    size_t calls;
    size_t sample_count;
    int16_t last_sample;
} playback_context_t;

static void playback_callback(void *context, const int16_t *samples, size_t sample_count) {
    playback_context_t *ctx = (playback_context_t *)context;
    ctx->calls++;
    ctx->sample_count = sample_count;
    if (sample_count > 0U) {
        ctx->last_sample = samples[sample_count - 1U];
    }
}

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
    enum {
        TEST_PAIRING_PEER_ID = 4U
    };

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

    intercom_audio_subsystem_t audio = {0};
    intercom_audio_init(&audio);
    intercom_audio_frame_t captured_frame = {0};
    assert(intercom_audio_capture_frame(&audio, &captured_frame));
    assert(captured_frame.sample_count == INTERCOM_AUDIO_SAMPLES_PER_FRAME);
    assert(captured_frame.sequence == 1U);
    assert(captured_frame.samples[0] == -1800);
    assert(captured_frame.samples[1] == -1664);
    uint8_t encoded_audio[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t encoded_len = 0U;
    assert(intercom_audio_encode_frame(&captured_frame, encoded_audio, sizeof(encoded_audio),
                                       &encoded_len));
    assert(encoded_len == intercom_audio_frame_bytes(&captured_frame));
    intercom_audio_frame_t decoded_frame = {0};
    assert(intercom_audio_decode_frame(encoded_audio, encoded_len, &decoded_frame));
    assert(decoded_frame.sample_count == captured_frame.sample_count);
    assert(decoded_frame.sequence == captured_frame.sequence);
    assert(decoded_frame.samples[0] == captured_frame.samples[0]);
    playback_context_t playback_ctx = {0};
    intercom_audio_set_playback_callback(&audio, playback_callback, &playback_ctx);
    assert(intercom_audio_playback_frame(&audio, &decoded_frame));
    assert(intercom_audio_drain_playback_queue(&audio));
    assert(playback_ctx.calls == 1U);
    assert(playback_ctx.sample_count == decoded_frame.sample_count);
    assert(playback_ctx.last_sample == decoded_frame.samples[decoded_frame.sample_count - 1U]);

    intercom_audio_subsystem_t overflow_audio = {0};
    intercom_audio_init(&overflow_audio);
    intercom_audio_frame_t overflow_frame = {0};
    assert(intercom_audio_capture_frame(&overflow_audio, &overflow_frame));
    for (size_t index = 0; index < INTERCOM_AUDIO_PLAYBACK_QUEUE_DEPTH; ++index) {
        assert(intercom_audio_playback_frame(&overflow_audio, &overflow_frame));
    }
    assert(!intercom_audio_playback_frame(&overflow_audio, &overflow_frame));
    assert(overflow_audio.overruns == 1U);
    assert(overflow_audio.queued_frame_count == INTERCOM_AUDIO_PLAYBACK_QUEUE_DEPTH);

    intercom_set_ptt(&state, true);
    bluetooth_init(&runtime, &state);
    assert(bluetooth_is_enabled(&runtime));
    intercom_state_t local_audio_state;
    intercom_init(&local_audio_state);
    intercom_enable(&local_audio_state, true);
    assert(intercom_add_peer(&local_audio_state, 2U));
    assert(intercom_add_peer(&local_audio_state, 3U));
    intercom_set_ptt(&local_audio_state, true);

    bluetooth_runtime_t local_audio_runtime = {0};
    bluetooth_init(&local_audio_runtime, &local_audio_state);
    intercom_audio_set_playback_callback(&local_audio_runtime.audio, playback_callback, &playback_ctx);
    assert(bluetooth_process_local_audio(&local_audio_runtime, 7U));
    assert(local_audio_runtime.audio.encoded_frames == 1U);
    assert(local_audio_runtime.audio.decoded_frames == 1U);
    assert(local_audio_runtime.audio.played_frames == 1U);
    assert(local_audio_runtime.last_relay_count == 2U);
    bluetooth_runtime_t toggle_runtime = {0};
    bluetooth_init(&toggle_runtime, &state);
    assert(!bluetooth_toggle(&toggle_runtime));
    assert(!bluetooth_is_enabled(&toggle_runtime));
    assert(bluetooth_toggle(&toggle_runtime));
    assert(bluetooth_is_enabled(&toggle_runtime));

    bluetooth_runtime_t pairing_runtime = {0};
    bluetooth_init(&pairing_runtime, &state);
    assert(!bluetooth_handle_pairing_button(NULL, TEST_PAIRING_PEER_ID, true));
    bluetooth_runtime_t uninitialized_runtime = {0};
    assert(!bluetooth_handle_pairing_button(&uninitialized_runtime, TEST_PAIRING_PEER_ID, true));
    bluetooth_runtime_t disabled_runtime = {0};
    bluetooth_init(&disabled_runtime, &state);
    assert(bluetooth_disable(&disabled_runtime));
    assert(!bluetooth_handle_pairing_button(&disabled_runtime, TEST_PAIRING_PEER_ID, true));
    assert(!bluetooth_handle_pairing_button(&pairing_runtime, TEST_PAIRING_PEER_ID, false));
    assert(bluetooth_handle_pairing_button(&pairing_runtime, TEST_PAIRING_PEER_ID, true));
    assert(!bluetooth_handle_pairing_button(&pairing_runtime, TEST_PAIRING_PEER_ID, false));
    assert(bluetooth_is_peer_connected(&pairing_runtime, TEST_PAIRING_PEER_ID));
    assert(pairing_runtime.command_count == 1U);
    assert(pairing_runtime.last_command == BLUETOOTH_COMMAND_PAIR);
    assert(pairing_runtime.last_peer_id == TEST_PAIRING_PEER_ID);
    assert(pairing_runtime.pairing_attempts == 1U);
    assert(pairing_runtime.connection_attempts == 1U);
    bluetooth_peer_state_t queried_peer_state = BLUETOOTH_PEER_STATE_DISCONNECTED;
    assert(bluetooth_get_peer_state(&pairing_runtime, TEST_PAIRING_PEER_ID, &queried_peer_state));
    assert(queried_peer_state == BLUETOOTH_PEER_STATE_CONNECTED);
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
    assert(runtime.transport_packets_queued == 2U);
    assert(runtime.transport_packets_delivered == 2U);
    assert(runtime.last_transport_source_peer == 1U);
    assert(runtime.last_transport_target_peer == 3U);
    assert(bluetooth_classic_stack_pending_count(&runtime.classic_stack) == 0U);

    bluetooth_runtime_t runtime_to_disable = {0};
    bluetooth_init(&runtime_to_disable, &state);
    assert(bluetooth_runtime_is_operational(&runtime_to_disable));
    assert(bluetooth_disable(&runtime_to_disable));
    assert(!bluetooth_is_enabled(&runtime_to_disable));
    assert(!runtime_to_disable.enabled);
    assert(!bluetooth_runtime_is_operational(&runtime_to_disable));
    assert(!bluetooth_runtime_is_operational(NULL));

    bluetooth_runtime_t runtime_with_error = {0};
    bluetooth_init(&runtime_with_error, &state);
    runtime_with_error.platform_error = true;
    assert(!bluetooth_runtime_is_operational(&runtime_with_error));

    const size_t initial_packets = runtime_to_disable.packets_received;
    const size_t initial_relay_count = runtime_to_disable.last_relay_count;
    const size_t initial_relay_payload_len = runtime_to_disable.last_relay_payload_len;
    const uint8_t initial_source_peer = runtime_to_disable.last_source_peer;
    const size_t initial_payload_len = runtime_to_disable.last_payload_len;
    uint8_t initial_relay_payload_buf[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    memcpy(initial_relay_payload_buf, runtime_to_disable.last_relay_payload,
           sizeof(initial_relay_payload_buf));
    bluetooth_handle_audio(&runtime_to_disable, 5U, payload, sizeof(payload));
    assert(runtime_to_disable.packets_received == initial_packets);
    assert(runtime_to_disable.last_source_peer == initial_source_peer);
    assert(runtime_to_disable.last_payload_len == initial_payload_len);
    assert(runtime_to_disable.last_relay_count == initial_relay_count);
    assert(runtime_to_disable.last_relay_payload_len == initial_relay_payload_len);
    assert(memcmp(runtime_to_disable.last_relay_payload, initial_relay_payload_buf,
                  sizeof(runtime_to_disable.last_relay_payload)) == 0);

    bluetooth_runtime_t invalid_runtime = {0};
    assert(!bluetooth_toggle(&invalid_runtime));
    assert(!bluetooth_connect_peer(&invalid_runtime, 5U));
    assert(!bluetooth_disconnect_peer(&invalid_runtime, 5U));

    bluetooth_runtime_t audio_runtime = runtime;
    const size_t packets_before = audio_runtime.packets_received;
    bluetooth_handle_audio(&audio_runtime, 2U, NULL, 0U);
    assert(audio_runtime.packets_received == packets_before + 1U);
    assert(audio_runtime.last_source_peer == 2U);
    assert(audio_runtime.last_payload_len == 0U);
    assert(audio_runtime.last_relay_count == 0U);
    assert(audio_runtime.relay_target_count == 0U);

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
    assert(bluetooth_get_peer_state(&runtime, 2U, &queried_peer_state));
    assert(queried_peer_state == BLUETOOTH_PEER_STATE_CONNECTED);
    assert(!bluetooth_get_peer_state(&runtime, 99U, &queried_peer_state));

    bluetooth_classic_stack_t classic_stack = {0};
    bluetooth_classic_stack_init(&classic_stack);
    assert(bluetooth_classic_stack_pair(&classic_stack, 5U));
    assert(bluetooth_classic_stack_queue_packet(&classic_stack, 1U, 5U, payload,
                                                sizeof(payload)));
    assert(bluetooth_classic_stack_pending_count(&classic_stack) == 1U);
    bluetooth_classic_packet_t classic_packet = {0};
    assert(bluetooth_classic_stack_dequeue_packet(&classic_stack, &classic_packet));
    assert(classic_packet.target_peer == 5U);
    assert(classic_packet.payload_len == sizeof(payload));
    assert(classic_packet.source_peer == 1U);
    assert(bluetooth_classic_stack_pending_count(&classic_stack) == 0U);

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

    assert(remove_test_file("pairings_test.txt") == 0);

    assert(pairing_store_init(&store, "pairings_test.txt"));
    assert(pairing_store_clear(&store));
    assert(pairing_store_save(&store, &new_pairing));
    assert(pairing_store_save(&store, &duplicate_pairing));
    persisted_count = 0U;
    assert(pairing_store_load(&store, persisted, &persisted_count));
    assert(persisted_count == 1U);
    assert(persisted[0].peer_id == 4U);
    assert(strcmp(persisted[0].name, "headset-4") == 0);
    assert(remove_test_file("pairings_test.txt") == 0);

    pairing_store_t missing_store = {0};
    pairing_t empty_pairings[8] = {{0}};
    size_t empty_count = 0U;
    assert(remove_test_file("missing_pairings_test.txt") == 0);
    assert(pairing_store_init(&missing_store, "missing_pairings_test.txt"));
    assert(pairing_store_load(&missing_store, empty_pairings, &empty_count));
    assert(empty_count == 0U);

    pairing_store_t update_store = {0};
    pairing_t update_pairing = {.peer_id = 5U, .name = "headset-5"};
    pairing_t updated_pairing = {.peer_id = 5U, .name = "headset-5-updated"};
    pairing_t loaded_updates[8] = {{0}};
    size_t update_count = 0U;
    assert(remove_test_file("pairings_update_test.txt") == 0);

    assert(pairing_store_init(&update_store, "pairings_update_test.txt"));
    assert(pairing_store_clear(&update_store));
    assert(pairing_store_save(&update_store, &update_pairing));
    assert(pairing_store_save(&update_store, &updated_pairing));
    assert(pairing_store_load(&update_store, loaded_updates, &update_count));
    assert(update_count == 1U);
    assert(loaded_updates[0].peer_id == 5U);
    assert(strcmp(loaded_updates[0].name, "headset-5-updated") == 0);
    assert(remove_test_file("pairings_update_test.txt") == 0);

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
