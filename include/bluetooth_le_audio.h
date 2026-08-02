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
