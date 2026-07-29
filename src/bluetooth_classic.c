#include "bluetooth_classic.h"

#include <stdio.h>
#include <string.h>

static bool bluetooth_classic_stack_is_ready(const bluetooth_classic_stack_t *stack) {
    return stack != NULL && stack->initialized;
}

static void bluetooth_classic_set_transport_online(bluetooth_classic_stack_t *stack, bool enabled) {
    if (stack == NULL) {
        return;
    }

    stack->transport.backend_ready = enabled;
    stack->transport.network_connected = enabled;
}

void bluetooth_classic_stack_init(bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return;
    }

    memset(stack, 0, sizeof(*stack));
    bluetooth_transport_init(&stack->transport);
    stack->initialized = true;
    stack->enabled = true;
    stack->discoverable = true;
    stack->pairing_enabled = true;
    bluetooth_classic_set_transport_online(stack, true);
}

bool bluetooth_classic_stack_set_enabled(bluetooth_classic_stack_t *stack, bool enabled) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->enabled = enabled;
    stack->discoverable = enabled;
    stack->connected = enabled && stack->transport.connected_peer_count > 0U;
    if (!bluetooth_transport_set_enabled(&stack->transport, enabled)) {
        return false;
    }

    bluetooth_classic_set_transport_online(stack, enabled);
    return true;
}

void bluetooth_classic_stack_set_local_peer_id(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (stack == NULL) {
        return;
    }

    bluetooth_transport_set_local_peer_id(&stack->transport, peer_id);
}

uint8_t bluetooth_classic_stack_local_peer_id(const bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return 1U;
    }

    return bluetooth_transport_local_peer_id(&stack->transport);
}

bool bluetooth_classic_stack_pair(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->discoverable = true;
    stack->pairing_enabled = true;
    stack->paired_peer_id = peer_id;
    return bluetooth_classic_stack_connect(stack, peer_id);
}

bool bluetooth_classic_stack_connect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack) || !stack->enabled || peer_id == 0U) {
        return false;
    }

    if (!bluetooth_transport_connect(&stack->transport, peer_id)) {
        return false;
    }

    stack->paired_peer_id = peer_id;
    stack->connected = stack->transport.connected_peer_count > 0U;
    return bluetooth_transport_is_connected(&stack->transport, peer_id);
}

bool bluetooth_classic_stack_disconnect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    if (!bluetooth_transport_disconnect(&stack->transport, peer_id)) {
        return false;
    }

    if (stack->paired_peer_id == peer_id) {
        stack->paired_peer_id = 0U;
    }
    stack->connected = stack->transport.connected_peer_count > 0U;
    return true;
}

bool bluetooth_classic_stack_restore_pairing(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    return bluetooth_transport_restore_pairing(&stack->transport, peer_id);
}

bool bluetooth_classic_stack_poll(bluetooth_classic_stack_t *stack) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->transport.last_poll_ms += 100U;
    const bool polled = bluetooth_transport_poll(&stack->transport);
    stack->connected = stack->transport.connected_peer_count > 0U;
    return polled;
}

bool bluetooth_classic_stack_report_headset(bluetooth_classic_stack_t *stack, uint8_t peer_id,
                                            const char *name, bool audio_ready) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    const bool reported =
        bluetooth_transport_report_peer(&stack->transport, peer_id, name, audio_ready);
    if (reported) {
        printf("Bluetooth Classic headset %u %s audio transport is %s.\n",
               (unsigned)peer_id, name != NULL && name[0] != '\0' ? name : "peer",
               audio_ready ? "ready" : "negotiating");
    }
    return reported;
}

bool bluetooth_classic_stack_select_pairing_candidate(const bluetooth_classic_stack_t *stack,
                                                      uint8_t *peer_id) {
    if (stack == NULL) {
        return false;
    }

    return bluetooth_transport_select_pairing_candidate(&stack->transport, peer_id);
}

bool bluetooth_classic_stack_queue_packet(bluetooth_classic_stack_t *stack, uint8_t source_peer,
                                          uint8_t target_peer, const uint8_t *payload,
                                          size_t payload_len) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    return bluetooth_transport_queue_packet(&stack->transport, source_peer, target_peer, payload,
                                            payload_len);
}

bool bluetooth_classic_stack_dequeue_packet(bluetooth_classic_stack_t *stack,
                                            bluetooth_classic_packet_t *packet) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    return bluetooth_transport_dequeue_packet(&stack->transport, packet);
}

size_t bluetooth_classic_stack_pending_count(const bluetooth_classic_stack_t *stack) {
    if (stack == NULL) {
        return 0U;
    }

    return bluetooth_transport_pending_count(&stack->transport);
}

const char *bluetooth_classic_stack_state_name(const bluetooth_classic_stack_t *stack) {
    if (stack == NULL || !stack->initialized) {
        return "uninitialized";
    }

    if (!stack->enabled) {
        return "disabled";
    }

    if (stack->connected) {
        return "connected";
    }

    if (stack->transport.pending_pair_peer_id != 0U) {
        return "pairing";
    }

    if (stack->discoverable) {
        return "discoverable";
    }

    return "idle";
}
