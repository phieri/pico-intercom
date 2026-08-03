#include "audio.h"
#include "intercom_bluetooth.h"
#include "intercom.h"
#include "intercom_protocol.h"
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

static bool encode_protocol_message(uint8_t *buffer, size_t buffer_len,
                                    intercom_protocol_message_type_t message_type,
                                    uint32_t session_id, uint16_t sequence,
                                    uint8_t source_peer, uint8_t target_peer,
                                    const uint8_t *payload, uint16_t payload_len,
                                    size_t *encoded_len) {
    intercom_protocol_message_t message = {
        .version = INTERCOM_PROTOCOL_VERSION,
        .message_type = message_type,
        .session_id = session_id,
        .sequence = sequence,
        .ack_sequence = 0U,
        .source_peer = source_peer,
        .target_peer = target_peer,
        .payload_len = payload_len,
        .flags = 0U,
        .payload = payload,
    };
    return intercom_protocol_encode(buffer, buffer_len, &message, encoded_len);
}

static bluetooth_peer_link_t *find_peer_link_internal(bluetooth_runtime_t *runtime,
                                                     uint8_t peer_id) {
    if (runtime == NULL) {
        return NULL;
    }

    for (size_t index = 0; index < INTERCOM_MAX_PEERS; ++index) {
        bluetooth_peer_link_t *link = &runtime->peer_links[index];
        if (link->valid && link->peer_id == peer_id) {
            return link;
        }
    }

    return NULL;
}

static const bluetooth_peer_link_t *find_peer_link(bluetooth_runtime_t *runtime,
                                                   uint8_t peer_id) {
    return (const bluetooth_peer_link_t *)find_peer_link_internal(runtime, peer_id);
}

static bluetooth_peer_link_t *find_peer_link_writable(bluetooth_runtime_t *runtime,
                                                      uint8_t peer_id) {
    return find_peer_link_internal(runtime, peer_id);
}

static void complete_handshake(bluetooth_runtime_t *runtime, uint8_t peer_id) {
    assert(runtime != NULL);
    for (size_t attempt = 0; attempt < 3U && !bluetooth_is_peer_connected(runtime, peer_id);
         ++attempt) {
        bluetooth_poll(runtime);
    }

    bluetooth_peer_link_t *link = find_peer_link_writable(runtime, peer_id);
    assert(link != NULL);
    uint8_t hello_packet[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t hello_len = 0U;
    const uint32_t session_id = 0x10000000U | peer_id;
    char identity[16] = {0};
    assert(snprintf(identity, sizeof(identity), "peer-%u", (unsigned)peer_id) > 0);
    assert(encode_protocol_message(hello_packet, sizeof(hello_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_HELLO, session_id,
                                   1U, peer_id, runtime->local_peer_id,
                                   (const uint8_t *)identity, (uint16_t)strlen(identity),
                                   &hello_len));
    assert(bluetooth_handle_transport_payload(runtime, peer_id, hello_packet, hello_len));
    assert(link->session_active);
}

int main(void) {
    intercom_state_t state;
    bluetooth_runtime_t runtime = {0};
    struct relay_context ctx = {0};
    static const uint8_t payload[] = {0x01, 0x02, 0x03};
    enum {
        TEST_PAIRING_PEER_ID = 4U,
        TEST_PROTOCOL_RUNTIME_PEER_ID = 6U,
        TEST_TIMEOUT_RUNTIME_PEER_ID = 7U,
        /* 60 host-side polls at 100 ms each exceed the 5 s session timeout. */
        TIMEOUT_TEST_POLL_ITERATIONS = 60U,
    };

    intercom_init(&state);
    assert(state.ptt_pressed);
    intercom_enable(&state, true);
    assert(intercom_add_peer(&state, 2U));
    assert(intercom_add_peer(&state, 3U));

    intercom_state_t ptt_state;
    intercom_init(&ptt_state);
    assert(ptt_state.ptt_pressed);
    assert(intercom_toggle_ptt(&ptt_state));
    assert(!ptt_state.ptt_pressed);
    assert(intercom_toggle_ptt(&ptt_state));
    assert(ptt_state.ptt_pressed);
    assert(!intercom_toggle_ptt(NULL));

    intercom_set_ptt(&state, true);

    size_t relayed = intercom_rebroadcast(&state, 1U, payload, sizeof(payload), relay_callback,
                                          &ctx);
    assert(relayed == 1U);
    assert(ctx.calls == 1U);
    assert(ctx.targets[0] == 2U);

    intercom_set_ptt(&state, false);
    relayed = intercom_rebroadcast(&state, 1U, payload, sizeof(payload), relay_callback, &ctx);
    assert(relayed == 0U);
    assert(ctx.calls == 1U);

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

    uint8_t protocol_packet[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t protocol_packet_len = 0U;
    assert(encode_protocol_message(protocol_packet, sizeof(protocol_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_HELLO, 0x12345678U, 1U, 2U, 1U,
                                   (const uint8_t *)"peer-two", 8U, &protocol_packet_len));
    intercom_protocol_message_t decoded_message = {0};
    assert(intercom_protocol_decode(protocol_packet, protocol_packet_len, &decoded_message));
    assert(decoded_message.message_type == INTERCOM_PROTOCOL_MESSAGE_HELLO);
    assert(decoded_message.session_id == 0x12345678U);
    assert(decoded_message.sequence == 1U);
    assert(decoded_message.source_peer == 2U);
    assert(decoded_message.target_peer == 1U);
    assert(decoded_message.payload_len == 8U);
    assert(memcmp(decoded_message.payload, "peer-two", 8U) == 0);
    uint8_t error_payload[32] = {0};
    size_t error_payload_len = 0U;
    assert(intercom_protocol_build_error_payload(error_payload, sizeof(error_payload),
                                                 INTERCOM_PROTOCOL_ERROR_SESSION,
                                                 "session mismatch", &error_payload_len));
    assert(error_payload[0] == INTERCOM_PROTOCOL_ERROR_SESSION);
    assert(error_payload_len > 1U);

    intercom_set_ptt(&state, true);
    bluetooth_init(&runtime, &state);
    assert(bluetooth_is_enabled(&runtime));
    assert(bluetooth_runtime_has_transport(&runtime));
    assert(!bluetooth_runtime_is_operational(&runtime));

    intercom_state_t local_audio_state;
    intercom_init(&local_audio_state);
    intercom_enable(&local_audio_state, true);
    assert(intercom_add_peer(&local_audio_state, 2U));
    assert(intercom_add_peer(&local_audio_state, 3U));
    intercom_set_ptt(&local_audio_state, true);

    bluetooth_runtime_t local_audio_runtime = {0};
    bluetooth_init(&local_audio_runtime, &local_audio_state);
    assert(bluetooth_connect_peer(&local_audio_runtime, 2U));
    assert(bluetooth_connect_peer(&local_audio_runtime, 3U));
    complete_handshake(&local_audio_runtime, 2U);
    complete_handshake(&local_audio_runtime, 3U);
    assert(bluetooth_runtime_is_operational(&local_audio_runtime));
    const size_t protocol_sent_before = local_audio_runtime.protocol_messages_sent;
    const size_t transport_queued_before = local_audio_runtime.transport_packets_queued;
    const size_t transport_delivered_before = local_audio_runtime.transport_packets_delivered;
    assert(bluetooth_process_local_audio(&local_audio_runtime, local_audio_runtime.local_peer_id));
    assert(local_audio_runtime.audio.encoded_frames == 1U);
    assert(local_audio_runtime.audio.decoded_frames == 0U);
    assert(local_audio_runtime.audio.played_frames == 0U);
    assert(local_audio_runtime.last_relay_count == 1U);
    assert(local_audio_runtime.protocol_messages_sent >= protocol_sent_before + 1U);
    assert(local_audio_runtime.transport_packets_queued >= transport_queued_before + 1U);
    assert(local_audio_runtime.transport_packets_delivered >= transport_delivered_before + 1U);

    bluetooth_runtime_t toggle_runtime = {0};
    bluetooth_init(&toggle_runtime, &state);
    assert(!bluetooth_toggle(&toggle_runtime));
    assert(!bluetooth_is_enabled(&toggle_runtime));
    assert(bluetooth_toggle(&toggle_runtime));
    assert(bluetooth_is_enabled(&toggle_runtime));

    bluetooth_runtime_t pairing_runtime = {0};
    bluetooth_init(&pairing_runtime, &state);
    assert(bluetooth_restore_pairing(&pairing_runtime, TEST_PAIRING_PEER_ID));
    assert(!bluetooth_handle_pairing_button(NULL, TEST_PAIRING_PEER_ID, true));
    bluetooth_runtime_t uninitialized_runtime = {0};
    assert(!bluetooth_handle_pairing_button(&uninitialized_runtime, TEST_PAIRING_PEER_ID, true));
    bluetooth_runtime_t disabled_runtime = {0};
    bluetooth_init(&disabled_runtime, &state);
    assert(bluetooth_disable(&disabled_runtime));
    assert(!bluetooth_handle_pairing_button(&disabled_runtime, TEST_PAIRING_PEER_ID, true));
    assert(!bluetooth_handle_pairing_button(&pairing_runtime, TEST_PAIRING_PEER_ID, false));
    assert(bluetooth_handle_pairing_button(&pairing_runtime, TEST_PAIRING_PEER_ID, true));
    complete_handshake(&pairing_runtime, TEST_PAIRING_PEER_ID);
    assert(bluetooth_is_peer_connected(&pairing_runtime, TEST_PAIRING_PEER_ID));
    assert(pairing_runtime.command_count == 1U);
    assert(pairing_runtime.last_command == BLUETOOTH_COMMAND_PAIR);
    assert(pairing_runtime.last_peer_id == TEST_PAIRING_PEER_ID);
    assert(pairing_runtime.pairing_attempts == 1U);
    assert(pairing_runtime.connection_attempts == 1U);
    assert(pairing_runtime.pairing_completed);
    assert(pairing_runtime.completed_pairing_peer_id == TEST_PAIRING_PEER_ID);
    assert(pairing_runtime.session_ready_peer_count == 1U);
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

    bluetooth_runtime_t operational_runtime = {0};
    bluetooth_init(&operational_runtime, &state);
    assert(bluetooth_connect_peer(&operational_runtime, 2U));
    assert(bluetooth_connect_peer(&operational_runtime, 3U));
    complete_handshake(&operational_runtime, 2U);
    complete_handshake(&operational_runtime, 3U);
    assert(bluetooth_runtime_is_operational(&operational_runtime));
    intercom_audio_set_playback_callback(&operational_runtime.audio, playback_callback,
                                         &playback_ctx);
    const size_t received_before = operational_runtime.packets_received;
    bluetooth_handle_audio(&operational_runtime, 9U, encoded_audio, encoded_len);
    assert(operational_runtime.packets_received == received_before + 1U);
    assert(operational_runtime.last_source_peer == 9U);
    assert(operational_runtime.last_payload_len == encoded_len);
    assert(operational_runtime.last_relay_count == 1U);
    assert(operational_runtime.relay_invocations == 1U);
    assert(operational_runtime.relay_target_count == 1U);
    assert(operational_runtime.last_relay_source_peer == 9U);
    assert(operational_runtime.last_relay_payload_len == encoded_len);
    assert(operational_runtime.audio.decoded_frames == 1U);
    assert(operational_runtime.audio.played_frames == 1U);

    bluetooth_runtime_t runtime_to_disable = {0};
    bluetooth_init(&runtime_to_disable, &state);
    assert(!bluetooth_runtime_is_operational(&runtime_to_disable));
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
    bluetooth_handle_audio(&runtime_to_disable, 5U, payload, sizeof(payload));
    assert(runtime_to_disable.packets_received == initial_packets);

    bluetooth_runtime_t invalid_runtime = {0};
    assert(!bluetooth_toggle(&invalid_runtime));
    assert(!bluetooth_connect_peer(&invalid_runtime, 5U));
    assert(!bluetooth_disconnect_peer(&invalid_runtime, 5U));

    bluetooth_runtime_t audio_runtime = {0};
    bluetooth_init(&audio_runtime, &state);
    assert(bluetooth_connect_peer(&audio_runtime, 2U));
    const size_t packets_before = audio_runtime.packets_received;
    bluetooth_handle_audio(&audio_runtime, 2U, NULL, 0U);
    assert(audio_runtime.packets_received == packets_before);

    intercom_set_ptt(&state, false);
    bluetooth_handle_audio(&operational_runtime, 2U, encoded_audio, encoded_len);
    assert(operational_runtime.last_relay_count == 0U);
    intercom_set_ptt(&state, true);

    assert(bluetooth_connect_peer(&operational_runtime, 4U));
    assert(bluetooth_is_peer_connected(&operational_runtime, 4U));
    assert(operational_runtime.connected_peer_count == 3U);
    assert(bluetooth_connect_peer(&operational_runtime, 4U));
    assert(operational_runtime.connected_peer_count == 3U);
    assert(bluetooth_disconnect_peer(&operational_runtime, 4U));
    assert(!bluetooth_is_peer_connected(&operational_runtime, 4U));
    assert(!bluetooth_disconnect_peer(&operational_runtime, 99U));
    assert(operational_runtime.connected_peer_count == 2U);
    assert(bluetooth_get_peer_state(&operational_runtime, 2U, &queried_peer_state));
    assert(queried_peer_state == BLUETOOTH_PEER_STATE_CONNECTED);
    assert(!bluetooth_get_peer_state(&operational_runtime, 99U, &queried_peer_state));

    bluetooth_le_audio_stack_t le_audio_stack = {0};
    bluetooth_le_audio_stack_init(&le_audio_stack);
    assert(strcmp(bluetooth_le_audio_stack_state_name(&le_audio_stack), "discoverable") == 0);
    assert(bluetooth_le_audio_stack_report_headset(&le_audio_stack, 5U, "headset-5", true));
    assert(bluetooth_le_audio_stack_pair(&le_audio_stack, 5U));
    assert(strcmp(bluetooth_le_audio_stack_state_name(&le_audio_stack), "connected") == 0);
    assert(bluetooth_le_audio_stack_queue_packet(&le_audio_stack, 1U, 5U, payload, sizeof(payload)));
    assert(bluetooth_le_audio_stack_pending_count(&le_audio_stack) == 1U);
    bluetooth_le_audio_packet_t le_audio_packet = {0};
    assert(bluetooth_le_audio_stack_dequeue_packet(&le_audio_stack, &le_audio_packet));
    assert(le_audio_packet.target_peer == 5U);
    assert(le_audio_packet.payload_len == sizeof(payload));
    assert(le_audio_packet.source_peer == 1U);
    assert(bluetooth_le_audio_stack_pending_count(&le_audio_stack) == 0U);
    for (size_t index = 0; index < BLUETOOTH_TRANSPORT_QUEUE_DEPTH; ++index) {
        assert(bluetooth_le_audio_stack_queue_packet(&le_audio_stack, 1U, 5U, payload,
                                                    sizeof(payload)));
    }
    assert(!bluetooth_le_audio_stack_queue_packet(&le_audio_stack, 1U, 5U, payload, sizeof(payload)));
    assert(le_audio_stack.transport.packets_dropped == 1U);
    while (bluetooth_le_audio_stack_dequeue_packet(&le_audio_stack, &le_audio_packet)) {
    }
    assert(bluetooth_le_audio_stack_pending_count(&le_audio_stack) == 0U);
    assert(bluetooth_le_audio_stack_disconnect(&le_audio_stack, 5U));
    assert(strcmp(bluetooth_le_audio_stack_state_name(&le_audio_stack), "discoverable") == 0);
    assert(bluetooth_le_audio_stack_set_enabled(&le_audio_stack, false));
    assert(strcmp(bluetooth_le_audio_stack_state_name(&le_audio_stack), "disabled") == 0);
    assert(bluetooth_le_audio_stack_set_enabled(&le_audio_stack, true));
    assert(strcmp(bluetooth_le_audio_stack_state_name(&le_audio_stack), "discoverable") == 0);

    bluetooth_le_audio_stack_t call_control_stack = {0};
    bluetooth_le_audio_stack_init(&call_control_stack);
    assert(bluetooth_le_audio_stack_enable_call_control(&call_control_stack, 5U, true));
    assert(bluetooth_le_audio_stack_handle_call_control_notification(
        &call_control_stack, 5U,
        (const uint8_t[]){BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_ACCEPT}, 1U));
    assert(bluetooth_le_audio_stack_call_control_ptt_pressed(&call_control_stack));
    assert(call_control_stack.call_control.active_call);
    assert(call_control_stack.call_control.peer_id == 5U);
    assert(bluetooth_le_audio_stack_handle_call_control_opcode(&call_control_stack, 5U,
                                                                BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_TERMINATE));
    assert(!bluetooth_le_audio_stack_call_control_ptt_pressed(&call_control_stack));
    assert(!call_control_stack.call_control.active_call);
    assert(bluetooth_le_audio_stack_handle_call_state_change(&call_control_stack, 5U,
                                                              BLUETOOTH_LE_AUDIO_CALL_STATE_ACTIVE));
    assert(bluetooth_le_audio_stack_call_control_ptt_pressed(&call_control_stack));
    assert(call_control_stack.call_control.last_state == BLUETOOTH_LE_AUDIO_CALL_STATE_ACTIVE);
    assert(strcmp(bluetooth_le_audio_call_control_opcode_name(
                     BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_ACCEPT),
                 "accept") == 0);
    assert(strcmp(bluetooth_le_audio_call_state_name(BLUETOOTH_LE_AUDIO_CALL_STATE_INCOMING),
                  "incoming") == 0);
    assert(!bluetooth_le_audio_stack_handle_call_control_opcode(&call_control_stack, 5U, 0x7FU));
 
    bluetooth_le_audio_stack_t reconnect_stack = {0};
    bluetooth_le_audio_stack_init(&reconnect_stack);
    assert(bluetooth_le_audio_stack_restore_pairing(&reconnect_stack, 7U));
    assert(bluetooth_le_audio_stack_report_headset(&reconnect_stack, 7U, "headset-7", false));
    uint8_t candidate_peer_id = 0U;
    assert(!bluetooth_le_audio_stack_select_pairing_candidate(&reconnect_stack, &candidate_peer_id));
    assert(bluetooth_le_audio_stack_report_headset(&reconnect_stack, 7U, "headset-7", true));
    assert(bluetooth_le_audio_stack_poll(&reconnect_stack));
    assert(reconnect_stack.connected);
    assert(bluetooth_transport_is_connected(&reconnect_stack.transport, 7U));
    assert(bluetooth_le_audio_stack_queue_packet(&reconnect_stack, 1U, 7U, payload, sizeof(payload)));
    assert(bluetooth_le_audio_stack_pending_count(&reconnect_stack) == 1U);
    assert(bluetooth_le_audio_stack_disconnect(&reconnect_stack, 7U));
    assert(reconnect_stack.transport.packets_dropped >= 1U);
    assert(bluetooth_le_audio_stack_pending_count(&reconnect_stack) == 0U);
    assert(bluetooth_le_audio_stack_report_headset(&reconnect_stack, 7U, "headset-7", true));
    assert(bluetooth_le_audio_stack_poll(&reconnect_stack));
    assert(!bluetooth_transport_is_connected(&reconnect_stack.transport, 7U));
    assert(bluetooth_le_audio_stack_connect(&reconnect_stack, 7U));
    assert(bluetooth_transport_is_connected(&reconnect_stack.transport, 7U));

    bluetooth_transport_t explicit_disconnect_transport = {0};
    bluetooth_transport_init(&explicit_disconnect_transport);
    assert(bluetooth_transport_connect(&explicit_disconnect_transport, 9U));
    assert(bluetooth_transport_disconnect(&explicit_disconnect_transport, 9U));
    assert(explicit_disconnect_transport.discovered_peer_count == 1U);
    assert(explicit_disconnect_transport.discovered_peers[0].disconnect_requested);
    explicit_disconnect_transport.discovered_peers[0].reconnect_blocked = false;
    (void)bluetooth_transport_poll(&explicit_disconnect_transport);
    assert(explicit_disconnect_transport.connected_peer_count == 0U);
    assert(!bluetooth_transport_is_connected(&explicit_disconnect_transport, 9U));

    intercom_state_t limit_state;
    bluetooth_runtime_t limit_runtime = {0};
    intercom_init(&limit_state);
    bluetooth_init(&limit_runtime, &limit_state);
    for (uint8_t peer_id = 2U; peer_id < (uint8_t)(INTERCOM_MAX_PEERS + 2U); ++peer_id) {
        assert(bluetooth_connect_peer(&limit_runtime, peer_id));
    }
    assert(limit_runtime.connected_peer_count == INTERCOM_MAX_PEERS);
    assert(!bluetooth_connect_peer(&limit_runtime, 10U));

    bluetooth_runtime_t protocol_runtime = {0};
    intercom_state_t protocol_state;
    intercom_init(&protocol_state);
    intercom_enable(&protocol_state, true);
    assert(intercom_add_peer(&protocol_state, TEST_PROTOCOL_RUNTIME_PEER_ID));
    bluetooth_init(&protocol_runtime, &protocol_state);
    assert(bluetooth_connect_peer(&protocol_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID));
    complete_handshake(&protocol_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    const bluetooth_peer_link_t *protocol_link =
        find_peer_link(&protocol_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(protocol_link != NULL);
    uint8_t inbound_audio[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t inbound_audio_len = 0U;
    assert(intercom_audio_encode_frame(&captured_frame, inbound_audio, sizeof(inbound_audio),
                                       &inbound_audio_len));
    size_t inbound_packet_len = 0U;
    assert(encode_protocol_message(protocol_packet, sizeof(protocol_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_AUDIO, protocol_link->session_id,
                                   2U, TEST_PROTOCOL_RUNTIME_PEER_ID, protocol_runtime.local_peer_id,
                                   inbound_audio, (uint16_t)inbound_audio_len,
                                   &inbound_packet_len));
    assert(bluetooth_handle_transport_payload(&protocol_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                              protocol_packet, inbound_packet_len));
    const size_t protocol_received = protocol_runtime.packets_received;
    const size_t dropped_before_duplicate = protocol_runtime.protocol_messages_dropped;
    assert(!bluetooth_handle_transport_payload(&protocol_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                               protocol_packet, inbound_packet_len));
    assert(protocol_runtime.packets_received == protocol_received);
    assert(protocol_runtime.protocol_messages_dropped == dropped_before_duplicate + 1U);
    size_t out_of_order_len = 0U;
    assert(encode_protocol_message(protocol_packet, sizeof(protocol_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_AUDIO, protocol_link->session_id,
                                   1U, TEST_PROTOCOL_RUNTIME_PEER_ID, protocol_runtime.local_peer_id,
                                   inbound_audio, (uint16_t)inbound_audio_len,
                                   &out_of_order_len));
    assert(!bluetooth_handle_transport_payload(&protocol_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                               protocol_packet, out_of_order_len));
    size_t wrong_target_len = 0U;
    assert(encode_protocol_message(protocol_packet, sizeof(protocol_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_AUDIO, protocol_link->session_id,
                                   3U, TEST_PROTOCOL_RUNTIME_PEER_ID, 99U, inbound_audio,
                                   (uint16_t)inbound_audio_len, &wrong_target_len));
    assert(!bluetooth_handle_transport_payload(&protocol_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                               protocol_packet, wrong_target_len));
    const char *protocol_error_name = bluetooth_error_name(protocol_runtime.last_error_code);
    assert(protocol_error_name != NULL);
    assert(strcmp(protocol_error_name, "protocol") == 0);

    bluetooth_runtime_t handshake_runtime = {0};
    intercom_state_t handshake_state;
    intercom_init(&handshake_state);
    intercom_enable(&handshake_state, true);
    assert(intercom_add_peer(&handshake_state, TEST_PROTOCOL_RUNTIME_PEER_ID));
    bluetooth_init(&handshake_runtime, &handshake_state);
    assert(bluetooth_connect_peer(&handshake_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID));
    complete_handshake(&handshake_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(handshake_runtime.intercom != NULL);
    assert(handshake_runtime.intercom->peer_count == 1U);
    assert(handshake_runtime.intercom->peers[0] == TEST_PROTOCOL_RUNTIME_PEER_ID);

    bluetooth_peer_link_t *handshake_link = find_peer_link_writable(&handshake_runtime,
                                                                     TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(handshake_link != NULL);
    assert(handshake_link->session_active);
    assert(handshake_link->hello_sent);
    handshake_link->session_active = false;
    handshake_link->hello_sent = false;
    handshake_link->link_state = BLUETOOTH_LINK_STATE_CONNECTING;
    handshake_link->last_handshake_ms = 0U;

    const size_t handshake_messages_before = handshake_runtime.protocol_messages_sent;
    bluetooth_poll(&handshake_runtime);
    assert(handshake_runtime.protocol_messages_sent > handshake_messages_before);
    assert(handshake_link->hello_sent);
    assert(handshake_runtime.intercom->peer_count == 1U);
    assert(bluetooth_disconnect_peer(&handshake_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID));
    assert(handshake_runtime.intercom->peer_count == 0U);

    bluetooth_runtime_t timeout_runtime = {0};
    intercom_state_t timeout_state;
    intercom_init(&timeout_state);
    intercom_enable(&timeout_state, true);
    bluetooth_init(&timeout_runtime, &timeout_state);
    assert(bluetooth_restore_pairing(&timeout_runtime, TEST_TIMEOUT_RUNTIME_PEER_ID));
    assert(bluetooth_connect_peer(&timeout_runtime, TEST_TIMEOUT_RUNTIME_PEER_ID));
    complete_handshake(&timeout_runtime, TEST_TIMEOUT_RUNTIME_PEER_ID);
    assert(bluetooth_runtime_is_operational(&timeout_runtime));
    assert(timeout_runtime.intercom != NULL);
    assert(timeout_runtime.intercom->peer_count == 1U);
    for (size_t index = 0; index < TIMEOUT_TEST_POLL_ITERATIONS; ++index) {
        bluetooth_poll(&timeout_runtime);
    }
    assert(!bluetooth_runtime_is_operational(&timeout_runtime));
    assert(timeout_runtime.intercom->peer_count == 0U);
    assert(bluetooth_le_audio_stack_report_headset(&timeout_runtime.le_audio_stack,
                                                  TEST_TIMEOUT_RUNTIME_PEER_ID, "headset-7", true));
    complete_handshake(&timeout_runtime, TEST_TIMEOUT_RUNTIME_PEER_ID);
    assert(bluetooth_runtime_is_operational(&timeout_runtime));
    assert(timeout_runtime.intercom->peer_count == 1U);

    /* Test: HELLO received via bluetooth_handle_transport_payload starts a session
     * and causes a HELLO_ACK to be queued on the outbound transport. */
    bluetooth_runtime_t hello_rx_runtime = {0};
    intercom_state_t hello_rx_state;
    intercom_init(&hello_rx_state);
    intercom_enable(&hello_rx_state, true);
    bluetooth_init(&hello_rx_runtime, &hello_rx_state);
    assert(bluetooth_connect_peer(&hello_rx_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID));
    bluetooth_peer_link_t *hello_rx_link =
        find_peer_link_writable(&hello_rx_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(hello_rx_link != NULL);
    /* Reset to "connected but handshake not started" to exercise the HELLO receive path. */
    hello_rx_link->session_active = false;
    hello_rx_link->session_id = 0U;
    hello_rx_link->hello_sent = false;
    hello_rx_link->hello_received = false;
    hello_rx_runtime.session_ready_peer_count = 0U;
    uint8_t hello_rx_packet[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t hello_rx_len = 0U;
    const uint32_t test_hello_session_id = 0x11223344U;
    assert(encode_protocol_message(hello_rx_packet, sizeof(hello_rx_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_HELLO, test_hello_session_id,
                                   1U, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                   hello_rx_runtime.local_peer_id,
                                   (const uint8_t *)"headset-6", 9U, &hello_rx_len));
    const size_t hello_tx_before = hello_rx_runtime.protocol_messages_sent;
    const size_t hello_queued_before = hello_rx_runtime.transport_packets_queued;
    assert(bluetooth_handle_transport_payload(&hello_rx_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                              hello_rx_packet, hello_rx_len));
    assert(hello_rx_link->session_active);
    assert(hello_rx_link->session_id == test_hello_session_id);
    assert(hello_rx_link->hello_received);
    assert(hello_rx_runtime.session_ready_peer_count == 1U);
    assert(hello_rx_runtime.protocol_messages_sent > hello_tx_before);
    assert(hello_rx_runtime.transport_packets_queued > hello_queued_before);

    /* Test: HELLO_ACK received via bluetooth_handle_transport_payload completes
     * the session handshake and increments successful_connections. */
    bluetooth_runtime_t hello_ack_runtime = {0};
    intercom_state_t hello_ack_state;
    intercom_init(&hello_ack_state);
    intercom_enable(&hello_ack_state, true);
    bluetooth_init(&hello_ack_runtime, &hello_ack_state);
    assert(bluetooth_connect_peer(&hello_ack_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID));
    bluetooth_poll(&hello_ack_runtime);
    bluetooth_peer_link_t *hello_ack_link =
        find_peer_link_writable(&hello_ack_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(hello_ack_link != NULL);
    /* Capture the session_id that was assigned during connect. */
    const uint32_t ack_session_id = hello_ack_link->session_id;
    assert(ack_session_id != 0U);
    /* Simulate "HELLO sent, waiting for ACK" by rolling back session_active. */
    hello_ack_link->session_active = false;
    hello_ack_link->hello_received = false;
    hello_ack_link->hello_sent = true;
    hello_ack_runtime.session_ready_peer_count = 0U;
    hello_ack_runtime.successful_connections = 0U;
    uint8_t hello_ack_packet[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t hello_ack_len = 0U;
    assert(encode_protocol_message(hello_ack_packet, sizeof(hello_ack_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_HELLO_ACK, ack_session_id,
                                   1U, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                   hello_ack_runtime.local_peer_id,
                                   NULL, 0U, &hello_ack_len));
    assert(bluetooth_handle_transport_payload(&hello_ack_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                              hello_ack_packet, hello_ack_len));
    assert(hello_ack_link->session_active);
    assert(hello_ack_runtime.session_ready_peer_count == 1U);
    assert(hello_ack_runtime.successful_connections == 1U);

    /* Test: GOODBYE received via bluetooth_handle_transport_payload tears down the
     * session, removes the peer from the intercom relay list, and marks the link
     * as no longer active. */
    bluetooth_runtime_t goodbye_runtime = {0};
    intercom_state_t goodbye_state;
    intercom_init(&goodbye_state);
    intercom_enable(&goodbye_state, true);
    assert(intercom_add_peer(&goodbye_state, TEST_PROTOCOL_RUNTIME_PEER_ID));
    bluetooth_init(&goodbye_runtime, &goodbye_state);
    assert(bluetooth_connect_peer(&goodbye_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID));
    complete_handshake(&goodbye_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(bluetooth_runtime_is_operational(&goodbye_runtime));
    assert(goodbye_runtime.intercom->peer_count == 1U);
    bluetooth_peer_link_t *goodbye_link =
        find_peer_link_writable(&goodbye_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(goodbye_link != NULL);
    const uint32_t goodbye_session_id = goodbye_link->session_id;
    uint8_t goodbye_packet[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t goodbye_len = 0U;
    assert(encode_protocol_message(goodbye_packet, sizeof(goodbye_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_GOODBYE, goodbye_session_id,
                                   2U, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                   goodbye_runtime.local_peer_id,
                                   NULL, 0U, &goodbye_len));
    assert(bluetooth_handle_transport_payload(&goodbye_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                              goodbye_packet, goodbye_len));
    assert(!goodbye_link->session_active);
    assert(!bluetooth_runtime_is_operational(&goodbye_runtime));
    assert(goodbye_runtime.intercom->peer_count == 0U);

    /* Test: ERROR message received via bluetooth_handle_transport_payload degrades the
     * link state and increments the dropped message counter. */
    bluetooth_runtime_t error_rx_runtime = {0};
    intercom_state_t error_rx_state;
    intercom_init(&error_rx_state);
    intercom_enable(&error_rx_state, true);
    assert(intercom_add_peer(&error_rx_state, TEST_PROTOCOL_RUNTIME_PEER_ID));
    bluetooth_init(&error_rx_runtime, &error_rx_state);
    assert(bluetooth_connect_peer(&error_rx_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID));
    complete_handshake(&error_rx_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    bluetooth_peer_link_t *error_rx_link =
        find_peer_link_writable(&error_rx_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID);
    assert(error_rx_link != NULL);
    const uint32_t error_rx_session_id = error_rx_link->session_id;
    uint8_t error_rx_payload[32] = {0};
    size_t error_rx_payload_len = 0U;
    assert(intercom_protocol_build_error_payload(error_rx_payload, sizeof(error_rx_payload),
                                                 INTERCOM_PROTOCOL_ERROR_SESSION,
                                                 "test error", &error_rx_payload_len));
    uint8_t error_rx_packet[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN] = {0};
    size_t error_rx_len = 0U;
    assert(encode_protocol_message(error_rx_packet, sizeof(error_rx_packet),
                                   INTERCOM_PROTOCOL_MESSAGE_ERROR, error_rx_session_id,
                                   2U, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                   error_rx_runtime.local_peer_id,
                                   error_rx_payload, (uint16_t)error_rx_payload_len,
                                   &error_rx_len));
    const size_t dropped_before_error_rx = error_rx_runtime.protocol_messages_dropped;
    assert(!bluetooth_handle_transport_payload(&error_rx_runtime, TEST_PROTOCOL_RUNTIME_PEER_ID,
                                               error_rx_packet, error_rx_len));
    assert(error_rx_link->link_state == BLUETOOTH_LINK_STATE_DEGRADED);
    assert(error_rx_runtime.protocol_messages_dropped > dropped_before_error_rx);

    /* Test: bluetooth_le_audio_stack_local_peer_id returns the configured ID and
     * normalises a zero peer ID to 1 to keep the ID in the valid range. */
    bluetooth_le_audio_stack_t local_id_stack = {0};
    bluetooth_le_audio_stack_init(&local_id_stack);
    bluetooth_le_audio_stack_set_local_peer_id(&local_id_stack, 42U);
    assert(bluetooth_le_audio_stack_local_peer_id(&local_id_stack) == 42U);
    bluetooth_le_audio_stack_set_local_peer_id(&local_id_stack, 0U);
    assert(bluetooth_le_audio_stack_local_peer_id(&local_id_stack) == 1U);

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
