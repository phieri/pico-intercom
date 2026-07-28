#ifndef INTERCOM_PROTOCOL_H
#define INTERCOM_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INTERCOM_PROTOCOL_VERSION 1U
#define INTERCOM_PROTOCOL_HEADER_BYTES 18U
#define INTERCOM_PROTOCOL_MAX_IDENTITY_LEN 48U

typedef enum {
    INTERCOM_PROTOCOL_MESSAGE_INVALID = 0,
    INTERCOM_PROTOCOL_MESSAGE_HELLO = 1,
    INTERCOM_PROTOCOL_MESSAGE_HELLO_ACK = 2,
    INTERCOM_PROTOCOL_MESSAGE_KEEPALIVE = 3,
    INTERCOM_PROTOCOL_MESSAGE_AUDIO = 4,
    INTERCOM_PROTOCOL_MESSAGE_ERROR = 5,
    INTERCOM_PROTOCOL_MESSAGE_GOODBYE = 6
} intercom_protocol_message_type_t;

typedef enum {
    INTERCOM_PROTOCOL_ERROR_NONE = 0,
    INTERCOM_PROTOCOL_ERROR_DECODE = 1,
    INTERCOM_PROTOCOL_ERROR_TARGET = 2,
    INTERCOM_PROTOCOL_ERROR_SESSION = 3,
    INTERCOM_PROTOCOL_ERROR_SEQUENCE = 4,
    INTERCOM_PROTOCOL_ERROR_AUDIO = 5,
    INTERCOM_PROTOCOL_ERROR_SATURATED = 6
} intercom_protocol_error_code_t;

typedef struct {
    uint8_t version;
    intercom_protocol_message_type_t message_type;
    uint32_t session_id;
    uint16_t sequence;
    uint16_t ack_sequence;
    uint8_t source_peer;
    uint8_t target_peer;
    uint16_t payload_len;
    uint16_t flags;
    const uint8_t *payload;
} intercom_protocol_message_t;

bool intercom_protocol_encode(uint8_t *buffer, size_t buffer_len,
                              const intercom_protocol_message_t *message,
                              size_t *encoded_len);
bool intercom_protocol_decode(const uint8_t *buffer, size_t buffer_len,
                              intercom_protocol_message_t *message);
bool intercom_protocol_build_error_payload(uint8_t *buffer, size_t buffer_len,
                                           intercom_protocol_error_code_t error_code,
                                           const char *reason, size_t *payload_len);
const char *intercom_protocol_message_type_name(intercom_protocol_message_type_t message_type);
const char *intercom_protocol_error_name(intercom_protocol_error_code_t error_code);

#ifdef __cplusplus
}
#endif

#endif
