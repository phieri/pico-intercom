#include "intercom_protocol.h"

#include <string.h>

enum {
    INTERCOM_PROTOCOL_MAGIC = 0x4349U
};

static uint16_t intercom_protocol_read_u16_le(const uint8_t *buffer) {
    return (uint16_t)((uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8U));
}

static uint32_t intercom_protocol_read_u32_le(const uint8_t *buffer) {
    return ((uint32_t)buffer[0]) | ((uint32_t)buffer[1] << 8U) |
           ((uint32_t)buffer[2] << 16U) | ((uint32_t)buffer[3] << 24U);
}

static void intercom_protocol_write_u16_le(uint8_t *buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void intercom_protocol_write_u32_le(uint8_t *buffer, uint32_t value) {
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16U) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

bool intercom_protocol_encode(uint8_t *buffer, size_t buffer_len,
                              const intercom_protocol_message_t *message,
                              size_t *encoded_len) {
    if (buffer == NULL || message == NULL || encoded_len == NULL) {
        return false;
    }

    if (message->message_type == INTERCOM_PROTOCOL_MESSAGE_INVALID || message->version == 0U) {
        return false;
    }

    if (message->payload_len > 0U && message->payload == NULL) {
        return false;
    }

    const size_t required_len = INTERCOM_PROTOCOL_HEADER_BYTES + (size_t)message->payload_len;
    if (buffer_len < required_len) {
        return false;
    }

    intercom_protocol_write_u16_le(buffer, INTERCOM_PROTOCOL_MAGIC);
    buffer[2] = message->version;
    buffer[3] = (uint8_t)message->message_type;
    intercom_protocol_write_u32_le(buffer + 4U, message->session_id);
    intercom_protocol_write_u16_le(buffer + 8U, message->sequence);
    intercom_protocol_write_u16_le(buffer + 10U, message->ack_sequence);
    buffer[12] = message->source_peer;
    buffer[13] = message->target_peer;
    intercom_protocol_write_u16_le(buffer + 14U, message->payload_len);
    intercom_protocol_write_u16_le(buffer + 16U, message->flags);

    if (message->payload_len > 0U) {
        memcpy(buffer + INTERCOM_PROTOCOL_HEADER_BYTES, message->payload,
               (size_t)message->payload_len);
    }

    *encoded_len = required_len;
    return true;
}

bool intercom_protocol_decode(const uint8_t *buffer, size_t buffer_len,
                              intercom_protocol_message_t *message) {
    if (buffer == NULL || message == NULL || buffer_len < INTERCOM_PROTOCOL_HEADER_BYTES) {
        return false;
    }

    if (intercom_protocol_read_u16_le(buffer) != INTERCOM_PROTOCOL_MAGIC) {
        return false;
    }

    memset(message, 0, sizeof(*message));
    message->version = buffer[2];
    message->message_type = (intercom_protocol_message_type_t)buffer[3];
    message->session_id = intercom_protocol_read_u32_le(buffer + 4U);
    message->sequence = intercom_protocol_read_u16_le(buffer + 8U);
    message->ack_sequence = intercom_protocol_read_u16_le(buffer + 10U);
    message->source_peer = buffer[12];
    message->target_peer = buffer[13];
    message->payload_len = intercom_protocol_read_u16_le(buffer + 14U);
    message->flags = intercom_protocol_read_u16_le(buffer + 16U);

    if (message->version != INTERCOM_PROTOCOL_VERSION ||
        message->message_type == INTERCOM_PROTOCOL_MESSAGE_INVALID) {
        return false;
    }

    if (buffer_len != INTERCOM_PROTOCOL_HEADER_BYTES + (size_t)message->payload_len) {
        return false;
    }

    message->payload = buffer + INTERCOM_PROTOCOL_HEADER_BYTES;
    return true;
}

bool intercom_protocol_build_error_payload(uint8_t *buffer, size_t buffer_len,
                                           intercom_protocol_error_code_t error_code,
                                           const char *reason, size_t *payload_len) {
    if (buffer == NULL || payload_len == NULL || buffer_len == 0U) {
        return false;
    }

    buffer[0] = (uint8_t)error_code;
    size_t written = 1U;
    if (reason != NULL && reason[0] != '\0' && buffer_len > 1U) {
        const size_t reason_len = strlen(reason);
        const size_t copy_len = reason_len < (buffer_len - 1U) ? reason_len : (buffer_len - 1U);
        memcpy(buffer + 1U, reason, copy_len);
        written += copy_len;
    }

    *payload_len = written;
    return true;
}

const char *intercom_protocol_message_type_name(intercom_protocol_message_type_t message_type) {
    switch (message_type) {
    case INTERCOM_PROTOCOL_MESSAGE_HELLO:
        return "hello";
    case INTERCOM_PROTOCOL_MESSAGE_HELLO_ACK:
        return "hello_ack";
    case INTERCOM_PROTOCOL_MESSAGE_KEEPALIVE:
        return "keepalive";
    case INTERCOM_PROTOCOL_MESSAGE_AUDIO:
        return "audio";
    case INTERCOM_PROTOCOL_MESSAGE_ERROR:
        return "error";
    case INTERCOM_PROTOCOL_MESSAGE_GOODBYE:
        return "goodbye";
    case INTERCOM_PROTOCOL_MESSAGE_INVALID:
    default:
        return "invalid";
    }
}

const char *intercom_protocol_error_name(intercom_protocol_error_code_t error_code) {
    switch (error_code) {
    case INTERCOM_PROTOCOL_ERROR_DECODE:
        return "decode";
    case INTERCOM_PROTOCOL_ERROR_TARGET:
        return "target";
    case INTERCOM_PROTOCOL_ERROR_SESSION:
        return "session";
    case INTERCOM_PROTOCOL_ERROR_SEQUENCE:
        return "sequence";
    case INTERCOM_PROTOCOL_ERROR_AUDIO:
        return "audio";
    case INTERCOM_PROTOCOL_ERROR_SATURATED:
        return "saturated";
    case INTERCOM_PROTOCOL_ERROR_NONE:
    default:
        return "none";
    }
}
