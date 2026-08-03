#ifndef BLUETOOTH_LE_AUDIO_H
#define BLUETOOTH_LE_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bluetooth_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bluetooth_transport_t bluetooth_le_audio_transport_t;
typedef bluetooth_transport_packet_t bluetooth_le_audio_packet_t;

typedef enum {
    BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_ACCEPT = 0x00U,
    BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_TERMINATE = 0x01U,
    BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_LOCAL_HOLD = 0x02U,
    BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_LOCAL_RETRIEVE = 0x03U,
    BLUETOOTH_LE_AUDIO_CALL_CONTROL_OPCODE_ORIGINATE = 0x04U,
} bluetooth_le_audio_call_control_opcode_t;

typedef enum {
    BLUETOOTH_LE_AUDIO_CALL_STATE_IDLE = 0x00U,
    BLUETOOTH_LE_AUDIO_CALL_STATE_INCOMING = 0x01U,
    BLUETOOTH_LE_AUDIO_CALL_STATE_ACTIVE = 0x02U,
    BLUETOOTH_LE_AUDIO_CALL_STATE_HELD = 0x03U,
    BLUETOOTH_LE_AUDIO_CALL_STATE_ENDED = 0x04U,
    BLUETOOTH_LE_AUDIO_CALL_STATE_ORIGINATING = 0x05U,
    BLUETOOTH_LE_AUDIO_CALL_STATE_ALERTING = 0x06U,
    BLUETOOTH_LE_AUDIO_CALL_STATE_QUEUED = 0x07U,
} bluetooth_le_audio_call_state_t;

typedef struct {
    bool supported;
    bool enabled;
    bool notifications_enabled;
    bool ptt_pressed;
    bool active_call;
    bool incoming_call;
    bool held_call;
    uint8_t peer_id;
    uint8_t last_opcode;
    uint8_t last_state;
    uint32_t last_event_ms;
} bluetooth_le_audio_call_control_state_t;

typedef struct {
    bool initialized;
    bool enabled;
    bool discoverable;
    bool pairing_enabled;
    bool connected;
    uint8_t paired_peer_id;
    bluetooth_le_audio_transport_t transport;
    bluetooth_le_audio_packet_t outbound_queue[BLUETOOTH_TRANSPORT_QUEUE_DEPTH];
    size_t outbound_packet_count;
    bluetooth_le_audio_call_control_state_t call_control;
} bluetooth_le_audio_stack_t;

void bluetooth_le_audio_stack_init(bluetooth_le_audio_stack_t *stack);
bool bluetooth_le_audio_stack_set_enabled(bluetooth_le_audio_stack_t *stack, bool enabled);
void bluetooth_le_audio_stack_set_local_peer_id(bluetooth_le_audio_stack_t *stack, uint8_t peer_id);
uint8_t bluetooth_le_audio_stack_local_peer_id(const bluetooth_le_audio_stack_t *stack);
bool bluetooth_le_audio_stack_pair(bluetooth_le_audio_stack_t *stack, uint8_t peer_id);
bool bluetooth_le_audio_stack_connect(bluetooth_le_audio_stack_t *stack, uint8_t peer_id);
bool bluetooth_le_audio_stack_disconnect(bluetooth_le_audio_stack_t *stack, uint8_t peer_id);
bool bluetooth_le_audio_stack_restore_pairing(bluetooth_le_audio_stack_t *stack, uint8_t peer_id);
bool bluetooth_le_audio_stack_poll(bluetooth_le_audio_stack_t *stack);
bool bluetooth_le_audio_stack_report_headset(bluetooth_le_audio_stack_t *stack, uint8_t peer_id,
                                             const char *name, bool audio_ready);
bool bluetooth_le_audio_stack_select_pairing_candidate(const bluetooth_le_audio_stack_t *stack,
                                                       uint8_t *peer_id);
bool bluetooth_le_audio_stack_enable_call_control(bluetooth_le_audio_stack_t *stack,
                                                 uint8_t peer_id, bool enabled);
bool bluetooth_le_audio_stack_handle_call_control_notification(bluetooth_le_audio_stack_t *stack,
                                                              uint8_t peer_id,
                                                              const uint8_t *data,
                                                              size_t data_len);
bool bluetooth_le_audio_stack_handle_call_control_opcode(bluetooth_le_audio_stack_t *stack,
                                                         uint8_t peer_id, uint8_t opcode);
bool bluetooth_le_audio_stack_handle_call_state_change(bluetooth_le_audio_stack_t *stack,
                                                       uint8_t peer_id, uint8_t call_state);
bool bluetooth_le_audio_stack_call_control_ptt_pressed(const bluetooth_le_audio_stack_t *stack);
const char *bluetooth_le_audio_call_control_opcode_name(uint8_t opcode);
const char *bluetooth_le_audio_call_state_name(uint8_t state);
bool bluetooth_le_audio_stack_queue_packet(bluetooth_le_audio_stack_t *stack, uint8_t source_peer,
                                           uint8_t target_peer, const uint8_t *payload,
                                           size_t payload_len);
bool bluetooth_le_audio_stack_dequeue_packet(bluetooth_le_audio_stack_t *stack,
                                              bluetooth_le_audio_packet_t *packet);
size_t bluetooth_le_audio_stack_pending_count(const bluetooth_le_audio_stack_t *stack);
const char *bluetooth_le_audio_stack_state_name(const bluetooth_le_audio_stack_t *stack);

#ifdef __cplusplus
}
#endif

#endif
