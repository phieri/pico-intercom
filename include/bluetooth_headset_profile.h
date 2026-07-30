#ifndef BLUETOOTH_HEADSET_PROFILE_H
#define BLUETOOTH_HEADSET_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bluetooth_classic.h"

#ifdef __cplusplus
extern "C" {
#endif

void bluetooth_headset_profile_init(bluetooth_classic_stack_t *stack);
bool bluetooth_headset_profile_set_enabled(bluetooth_classic_stack_t *stack, bool enabled);
bool bluetooth_headset_profile_pair(bluetooth_classic_stack_t *stack, uint8_t peer_id);
bool bluetooth_headset_profile_connect(bluetooth_classic_stack_t *stack, uint8_t peer_id);
bool bluetooth_headset_profile_disconnect(bluetooth_classic_stack_t *stack, uint8_t peer_id);
bool bluetooth_headset_profile_restore_pairing(bluetooth_classic_stack_t *stack, uint8_t peer_id);
bool bluetooth_headset_profile_poll(bluetooth_classic_stack_t *stack);
bool bluetooth_headset_profile_report_headset(bluetooth_classic_stack_t *stack, uint8_t peer_id,
                                              const char *name, bool audio_ready);
bool bluetooth_headset_profile_queue_packet(bluetooth_classic_stack_t *stack, uint8_t source_peer,
                                            uint8_t target_peer, const uint8_t *payload,
                                            size_t payload_len);
size_t bluetooth_headset_profile_pending_count(const bluetooth_classic_stack_t *stack);

#ifdef __cplusplus
}
#endif

#endif
