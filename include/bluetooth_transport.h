#ifndef BLUETOOTH_TRANSPORT_H
#define BLUETOOTH_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intercom.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN 256U
#define BLUETOOTH_TRANSPORT_QUEUE_DEPTH (INTERCOM_MAX_PEERS * 2U)
#define BLUETOOTH_TRANSPORT_PEER_NAME_LEN 32U
#define BLUETOOTH_TRANSPORT_DISCOVERY_TIMEOUT_MS 5000U

typedef enum {
    BLUETOOTH_TRANSPORT_MEDIUM_CLASSIC_HEADSET = 0
} bluetooth_transport_medium_t;

typedef enum {
    BLUETOOTH_TRANSPORT_STATE_DISCONNECTED = 0,
    BLUETOOTH_TRANSPORT_STATE_CONNECTING,
    BLUETOOTH_TRANSPORT_STATE_CONNECTED,
    BLUETOOTH_TRANSPORT_STATE_DISCONNECTING
} bluetooth_transport_state_t;

typedef struct {
    uint8_t source_peer;
    uint8_t target_peer;
    uint8_t payload[BLUETOOTH_MAX_AUDIO_PAYLOAD_LEN];
    size_t payload_len;
} bluetooth_transport_packet_t;

typedef struct {
    bool valid;
    bool paired;
    bool pairing_pending;
    bool audio_ready;
    uint8_t peer_id;
    uint8_t address[6];
    uint8_t address_type;
    uint32_t last_seen_ms;
    uint32_t last_connected_ms;
    uint32_t last_disconnected_ms;
    uint16_t reconnect_attempts;
    char name[BLUETOOTH_TRANSPORT_PEER_NAME_LEN];
} bluetooth_transport_peer_info_t;

typedef struct {
    bool initialized;
    bool enabled;
    bool error;
    bluetooth_transport_medium_t medium;
    uint8_t connected_peers[INTERCOM_MAX_PEERS];
    bluetooth_transport_state_t peer_states[INTERCOM_MAX_PEERS];
    size_t connected_peer_count;
    bluetooth_transport_packet_t queue[BLUETOOTH_TRANSPORT_QUEUE_DEPTH];
    size_t queued_packet_count;
    size_t packets_queued;
    size_t packets_delivered;
    size_t packets_dropped;
    uint8_t last_source_peer;
    uint8_t last_target_peer;
    uint32_t last_error_code;
    bool backend_ready;
    bool network_connected;
    uint8_t local_peer_id;
    uint8_t pending_pair_peer_id;
    uint8_t remembered_peers[INTERCOM_MAX_PEERS];
    size_t remembered_peer_count;
    bluetooth_transport_peer_info_t discovered_peers[INTERCOM_MAX_PEERS];
    size_t discovered_peer_count;
    uint32_t last_poll_ms;
} bluetooth_transport_t;

void bluetooth_transport_init(bluetooth_transport_t *transport);
bool bluetooth_transport_set_enabled(bluetooth_transport_t *transport, bool enabled);
void bluetooth_transport_set_local_peer_id(bluetooth_transport_t *transport, uint8_t peer_id);
uint8_t bluetooth_transport_local_peer_id(const bluetooth_transport_t *transport);
bool bluetooth_transport_connect(bluetooth_transport_t *transport, uint8_t peer_id);
bool bluetooth_transport_disconnect(bluetooth_transport_t *transport, uint8_t peer_id);
bool bluetooth_transport_restore_pairing(bluetooth_transport_t *transport, uint8_t peer_id);
bool bluetooth_transport_poll(bluetooth_transport_t *transport);
bool bluetooth_transport_report_peer(bluetooth_transport_t *transport, uint8_t peer_id,
                                     const char *name, bool audio_ready);
bool bluetooth_transport_select_pairing_candidate(const bluetooth_transport_t *transport,
                                                  uint8_t *peer_id);
bool bluetooth_transport_is_connected(const bluetooth_transport_t *transport, uint8_t peer_id);
bool bluetooth_transport_queue_packet(bluetooth_transport_t *transport, uint8_t source_peer,
                                      uint8_t target_peer, const uint8_t *payload,
                                      size_t payload_len);
bool bluetooth_transport_dequeue_packet(bluetooth_transport_t *transport,
                                         bluetooth_transport_packet_t *packet);
size_t bluetooth_transport_pending_count(const bluetooth_transport_t *transport);
const char *bluetooth_transport_state_name(bluetooth_transport_state_t state);

#ifdef __cplusplus
}
#endif

#endif
