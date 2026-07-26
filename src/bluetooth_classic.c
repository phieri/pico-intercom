#include "bluetooth_classic.h"

#include <string.h>

static bool bluetooth_classic_stack_is_ready(const bluetooth_classic_stack_t *stack) {
    return stack != NULL && stack->initialized;
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
}

bool bluetooth_classic_stack_set_enabled(bluetooth_classic_stack_t *stack, bool enabled) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    stack->enabled = enabled;
    return bluetooth_transport_set_enabled(&stack->transport, enabled);
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
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    if (!stack->enabled) {
        return false;
    }

    if (bluetooth_transport_connect(&stack->transport, peer_id)) {
        stack->connected = true;
        return true;
    }

    return false;
}

bool bluetooth_classic_stack_disconnect(bluetooth_classic_stack_t *stack, uint8_t peer_id) {
    if (!bluetooth_classic_stack_is_ready(stack)) {
        return false;
    }

    if (!bluetooth_transport_disconnect(&stack->transport, peer_id)) {
        return false;
    }

    stack->connected = false;
    return true;
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

    if (stack->discoverable) {
        return "discoverable";
    }

    return "idle";
}
