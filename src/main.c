#include "bluetooth.h"
#include "pairings.h"

#ifdef PICO_INTERCOM_TARGET
#include "pico/stdlib.h"
#else
#include <stdio.h>
#endif

int main(void) {
#ifdef PICO_INTERCOM_TARGET
    stdio_init_all();
#endif

    intercom_state_t intercom;
    bluetooth_runtime_t bluetooth = {0};
    pairing_store_t pairing_store = {0};
    pairing_t persisted_pairing = {.peer_id = 2U, .name = "headset-2"};
    static const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};

    intercom_init(&intercom);
    intercom_enable(&intercom, true);
    intercom_add_peer(&intercom, 2U);
    intercom_add_peer(&intercom, 3U);
    intercom_set_ptt(&intercom, true);

    bluetooth_init(&bluetooth, &intercom);
    bluetooth_handle_audio(&bluetooth, 1U, payload, sizeof(payload));

    pairing_store_init(&pairing_store, "pairings.txt");
    pairing_store_save(&pairing_store, &persisted_pairing);

    printf("Pico intercom ready.\n");
    printf("Packets received: %zu\n", bluetooth.packets_received);
    printf("Last source peer: %u\n", (unsigned)bluetooth.last_source_peer);
    printf("Relayed packets: %zu\n", bluetooth.last_relay_count);
    return 0;
}
