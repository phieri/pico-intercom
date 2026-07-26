#include "bluetooth.h"
#include "pairings.h"

#ifdef PICO_INTERCOM_TARGET
#include "pico/stdlib.h"
#endif

#include <stdio.h>

#ifndef PICO_INTERCOM_PAIR_BUTTON_PIN
#if defined(PICO_DEFAULT_BUTTON_PIN)
#define PICO_INTERCOM_PAIR_BUTTON_PIN PICO_DEFAULT_BUTTON_PIN
#else
#define PICO_INTERCOM_PAIR_BUTTON_PIN 14U
#endif
#endif

int main(void) {
#ifdef PICO_INTERCOM_TARGET
    stdio_init_all();
    gpio_init(PICO_INTERCOM_PAIR_BUTTON_PIN);
    gpio_set_dir(PICO_INTERCOM_PAIR_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PICO_INTERCOM_PAIR_BUTTON_PIN);
#endif

    intercom_state_t intercom;
    bluetooth_runtime_t bluetooth = {0};
    pairing_store_t pairing_store = {0};
    pairing_t persisted_pairing = {.peer_id = 2U, .name = "headset-2"};
    static const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};
    static const uint8_t pairing_peer_id = 2U;
#ifdef PICO_INTERCOM_TARGET
    bool pairing_button_was_pressed = false;
#endif

    intercom_init(&intercom);
    intercom_enable(&intercom, true);
    intercom_add_peer(&intercom, 2U);
    intercom_add_peer(&intercom, 3U);
    intercom_set_ptt(&intercom, true);

    bluetooth_init(&bluetooth, &intercom);
    bluetooth_connect_peer(&bluetooth, 2U);
    bluetooth_connect_peer(&bluetooth, 3U);
    bluetooth_handle_audio(&bluetooth, 1U, payload, sizeof(payload));

    pairing_store_init(&pairing_store, "pairings.txt");
    pairing_store_save(&pairing_store, &persisted_pairing);

    printf("Pico intercom ready.\n");
    printf("Packets received: %zu\n", bluetooth.packets_received);
    printf("Last source peer: %u\n", (unsigned)bluetooth.last_source_peer);
    printf("Relayed packets: %zu\n", bluetooth.last_relay_count);

#ifdef PICO_INTERCOM_TARGET
    while (true) {
        const bool pairing_button_pressed = !gpio_get(PICO_INTERCOM_PAIR_BUTTON_PIN);
        if (pairing_button_pressed && !pairing_button_was_pressed) {
            if (bluetooth_handle_pairing_button(&bluetooth, pairing_peer_id, true)) {
                pairing_t button_pairing = {.peer_id = pairing_peer_id, .name = "headset-2"};
                pairing_store_save(&pairing_store, &button_pairing);
                printf("Pairing initiated for peer %u.\n", (unsigned)pairing_peer_id);
            }
        }
        pairing_button_was_pressed = pairing_button_pressed;
        sleep_ms(50);
    }
#else
    return 0;
#endif
}
