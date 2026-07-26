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

#ifndef PICO_INTERCOM_STATUS_LED_PIN
#if defined(PICO_DEFAULT_LED_PIN)
#define PICO_INTERCOM_STATUS_LED_PIN PICO_DEFAULT_LED_PIN
#else
#define PICO_INTERCOM_STATUS_LED_PIN 25U
#endif
#endif

#ifndef PICO_INTERCOM_PAIR_BUTTON_POLL_MS
#define PICO_INTERCOM_PAIR_BUTTON_POLL_MS 50U
#endif

#ifndef PICO_INTERCOM_PAIR_BUTTON_PEER_ID
#define PICO_INTERCOM_PAIR_BUTTON_PEER_ID 2U
#endif

#ifndef PICO_INTERCOM_PAIRING_DISPLAY_MS
#define PICO_INTERCOM_PAIRING_DISPLAY_MS 400U
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

static void init_status_led(void) {
    gpio_init(PICO_INTERCOM_STATUS_LED_PIN);
    gpio_set_dir(PICO_INTERCOM_STATUS_LED_PIN, GPIO_OUT);
    gpio_put(PICO_INTERCOM_STATUS_LED_PIN, 0);
}

static void update_status_led(const bluetooth_runtime_t *bluetooth, bool pairing_in_progress,
                              bool pairing_error, uint32_t now_ms) {
    if (bluetooth == NULL || !bluetooth_runtime_is_operational(bluetooth)) {
        gpio_put(PICO_INTERCOM_STATUS_LED_PIN, 0);
        return;
    }

    if (pairing_error) {
        gpio_put(PICO_INTERCOM_STATUS_LED_PIN, (now_ms / 250U) & 1U);
        return;
    }

    if (pairing_in_progress) {
        gpio_put(PICO_INTERCOM_STATUS_LED_PIN, (now_ms / 100U) & 1U);
        return;
    }

    if (bluetooth->connected_peer_count > 0U) {
        gpio_put(PICO_INTERCOM_STATUS_LED_PIN, 1);
        return;
    }

    gpio_put(PICO_INTERCOM_STATUS_LED_PIN, (now_ms / 1000U) & 1U);
}
#endif

int main(void) {
#ifdef PICO_INTERCOM_TARGET
    stdio_init_all();
    init_pairing_button();
    init_status_led();
#endif

    intercom_state_t intercom = {0};
    bluetooth_runtime_t bluetooth = {0};
    pairing_store_t pairing_store = {0};
    pairing_t persisted_pairings[PAIRING_MAX_COUNT] = {{0}};
    size_t persisted_count = 0U;
    bool pairing_button_was_pressed = false;
    bool pairing_in_progress = false;
    bool pairing_error = false;
    uint32_t pairing_started_ms = 0U;

    intercom_init(&intercom);
    intercom_enable(&intercom, true);
    intercom_set_ptt(&intercom, true);

    bluetooth_init(&bluetooth, &intercom);

    pairing_store_init(&pairing_store, "pairings.txt");
    if (pairing_store_load(&pairing_store, persisted_pairings, &persisted_count)) {
        for (size_t index = 0; index < persisted_count; ++index) {
            if (bluetooth_connect_peer(&bluetooth, persisted_pairings[index].peer_id)) {
                printf("Restored pairing for peer %u.\n",
                       (unsigned)persisted_pairings[index].peer_id);
            }
        }
    }

    printf("Pico intercom ready.\n");
    if (persisted_count == 0U) {
        printf("No persisted pairings found; press the onboard button to start pairing.\n");
    } else {
        printf("Loaded %zu persisted pairing(s).\n", persisted_count);
    }

#ifdef PICO_INTERCOM_TARGET
    while (true) {
        const bool pairing_button_pressed = !gpio_get(PICO_INTERCOM_PAIR_BUTTON_PIN);
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (pairing_button_pressed && !pairing_button_was_pressed) {
            pairing_in_progress = true;
            pairing_error = false;
            pairing_started_ms = now_ms;
            if (bluetooth_handle_pairing_button(&bluetooth, PICO_INTERCOM_PAIR_BUTTON_PEER_ID,
                                                 true)) {
                pairing_t button_pairing = make_pairing(PICO_INTERCOM_PAIR_BUTTON_PEER_ID);
                if (pairing_store_save(&pairing_store, &button_pairing)) {
                    printf("Pairing initiated for peer %u.\n",
                           (unsigned)PICO_INTERCOM_PAIR_BUTTON_PEER_ID);
                } else {
                    pairing_error = true;
                    printf("Failed to persist pairing for peer %u.\n",
                           (unsigned)PICO_INTERCOM_PAIR_BUTTON_PEER_ID);
                }
            } else {
                pairing_error = true;
                printf("Pairing request rejected for peer %u.\n",
                       (unsigned)PICO_INTERCOM_PAIR_BUTTON_PEER_ID);
            }
        }

        const uint32_t elapsed_pairing_ms = now_ms - pairing_started_ms;
        if (pairing_in_progress && elapsed_pairing_ms >= PICO_INTERCOM_PAIRING_DISPLAY_MS) {
            pairing_in_progress = false;
        }

        update_status_led(&bluetooth, pairing_in_progress, pairing_error, now_ms);
        pairing_button_was_pressed = pairing_button_pressed;
        sleep_ms(PICO_INTERCOM_PAIR_BUTTON_POLL_MS);
    }
#else
    return 0;
#endif
}
