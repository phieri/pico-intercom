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

#ifndef PICO_INTERCOM_PAIR_BUTTON_POLL_MS
#define PICO_INTERCOM_PAIR_BUTTON_POLL_MS 50U
#endif

#ifndef PICO_INTERCOM_PAIR_BUTTON_PEER_ID
#define PICO_INTERCOM_PAIR_BUTTON_PEER_ID 2U
#endif

/**
 * Generate a human-readable pairing label for the provided peer ID.
 *
 * The resulting name is persisted with the pairing metadata so paired devices
 * can be distinguished in the runtime state.
 */
static void pairing_name_from_peer_id(char *buffer, size_t buffer_len, uint8_t peer_id) {
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }

    snprintf(buffer, buffer_len, "headset-%u", (unsigned)peer_id);
}

static pairing_t make_pairing(uint8_t peer_id) {
    pairing_t pairing = {.peer_id = peer_id};
    pairing_name_from_peer_id(pairing.name, sizeof(pairing.name), peer_id);
    return pairing;
}

#ifdef PICO_INTERCOM_TARGET
static void init_pairing_button(void) {
    gpio_init(PICO_INTERCOM_PAIR_BUTTON_PIN);
    gpio_set_dir(PICO_INTERCOM_PAIR_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PICO_INTERCOM_PAIR_BUTTON_PIN);
}
#endif

int main(void) {
#ifdef PICO_INTERCOM_TARGET
    stdio_init_all();
    init_pairing_button();
#endif

    intercom_state_t intercom;
    bluetooth_runtime_t bluetooth = {0};
    pairing_store_t pairing_store = {0};
    pairing_t initial_pairing = make_pairing(PICO_INTERCOM_PAIR_BUTTON_PEER_ID);
#ifdef PICO_INTERCOM_TARGET
    pairing_t button_pairing = make_pairing(PICO_INTERCOM_PAIR_BUTTON_PEER_ID);
    bool pairing_button_was_pressed = false;
#endif
    static const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};

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
    pairing_store_save(&pairing_store, &initial_pairing);

    printf("Pico intercom ready.\n");
    printf("Packets received: %zu\n", bluetooth.packets_received);
    printf("Last source peer: %u\n", (unsigned)bluetooth.last_source_peer);
    printf("Relayed packets: %zu\n", bluetooth.last_relay_count);

#ifdef PICO_INTERCOM_TARGET
    while (true) {
        const bool pairing_button_pressed = !gpio_get(PICO_INTERCOM_PAIR_BUTTON_PIN);
        if (pairing_button_pressed && !pairing_button_was_pressed) {
            if (bluetooth_handle_pairing_button(&bluetooth, PICO_INTERCOM_PAIR_BUTTON_PEER_ID,
                                                 true)) {
                pairing_store_save(&pairing_store, &button_pairing);
                printf("Pairing initiated for peer %u.\n",
                       (unsigned)PICO_INTERCOM_PAIR_BUTTON_PEER_ID);
            }
        }
        pairing_button_was_pressed = pairing_button_pressed;
        sleep_ms(PICO_INTERCOM_PAIR_BUTTON_POLL_MS);
    }
#else
    return 0;
#endif
}
