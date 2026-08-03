#include "intercom_bluetooth.h"
#include "pairings.h"

#ifdef PICO_INTERCOM_TARGET
#include "pico/cyw43_arch.h"
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
#if defined(CYW43_WL_GPIO_LED_PIN)
#define PICO_INTERCOM_USE_CYW43_STATUS_LED 1
#else
#define PICO_INTERCOM_USE_CYW43_STATUS_LED 0
#define PICO_INTERCOM_STATUS_LED_PIN 15U
#endif
#else
#define PICO_INTERCOM_USE_CYW43_STATUS_LED 0
#endif

#ifndef PICO_INTERCOM_PAIR_BUTTON_POLL_MS
#define PICO_INTERCOM_PAIR_BUTTON_POLL_MS 50U
#endif

#ifndef PICO_INTERCOM_PAIR_BUTTON_HOLD_MS
#define PICO_INTERCOM_PAIR_BUTTON_HOLD_MS 400U
#endif

#ifndef PICO_INTERCOM_PAIR_BUTTON_PEER_ID
#define PICO_INTERCOM_PAIR_BUTTON_PEER_ID 2U
#endif

#ifndef PICO_INTERCOM_PAIRING_DISPLAY_MS
#define PICO_INTERCOM_PAIRING_DISPLAY_MS 400U
#endif

#ifndef PICO_INTERCOM_AUDIO_POLL_MS
#define PICO_INTERCOM_AUDIO_POLL_MS 100U
#endif

#ifndef PICO_INTERCOM_TRANSPORT_STARTUP_TIMEOUT_MS
#define PICO_INTERCOM_TRANSPORT_STARTUP_TIMEOUT_MS 10000U
#endif

#ifndef PICO_INTERCOM_LOCAL_PEER_ID
#define PICO_INTERCOM_LOCAL_PEER_ID 1U
#endif

/**
 * Generate a human-readable pairing label for the provided peer ID.
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

static void status_led_write(const bluetooth_runtime_t *bluetooth, bool enabled) {
#if PICO_INTERCOM_USE_CYW43_STATUS_LED
    if (bluetooth != NULL && bluetooth->platform_initialized && !bluetooth->platform_error) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, enabled ? 1 : 0);
    }
#else
    (void)bluetooth;
    gpio_put(PICO_INTERCOM_STATUS_LED_PIN, enabled ? 1 : 0);
#endif
}

static void init_status_led(void) {
#if !PICO_INTERCOM_USE_CYW43_STATUS_LED
    gpio_init(PICO_INTERCOM_STATUS_LED_PIN);
    gpio_set_dir(PICO_INTERCOM_STATUS_LED_PIN, GPIO_OUT);
    gpio_put(PICO_INTERCOM_STATUS_LED_PIN, 0);
#endif
}

static void update_status_led(const bluetooth_runtime_t *bluetooth, bool pairing_in_progress,
                              bool pairing_error, bool transport_warning, uint32_t now_ms) {
    if (bluetooth == NULL || !bluetooth_runtime_has_transport(bluetooth)) {
        status_led_write(bluetooth, false);
        return;
    }

    const bool runtime_error =
        pairing_error || transport_warning || bluetooth->storage_error || bluetooth->platform_error;
    if (runtime_error) {
        status_led_write(bluetooth, ((now_ms / 250U) & 1U) != 0U);
        return;
    }

    if (pairing_in_progress) {
        status_led_write(bluetooth, ((now_ms / 100U) & 1U) != 0U);
        return;
    }

    if (bluetooth->connected_peer_count > 0U) {
        status_led_write(bluetooth, true);
        return;
    }

    status_led_write(bluetooth, ((now_ms / 1000U) & 1U) != 0U);
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
    bool pairing_button_hold_triggered = false;
    uint32_t pairing_started_ms = 0U;
    uint32_t pairing_button_press_started_ms = 0U;
    uint32_t last_audio_tick_ms = 0U;
    size_t last_reported_ready_peers = 0U;
    uint32_t last_reported_error = 0U;
    uint32_t last_status_report_ms = 0U;
    bool have_reported_status = false;
    bool transport_warning = false;

    intercom_init(&intercom);
    intercom_enable(&intercom, true);
    intercom_set_ptt(&intercom, false);

    bluetooth_init(&bluetooth, &intercom);

    pairing_store_init(&pairing_store, "pairings.txt");
    if (pairing_store_load(&pairing_store, persisted_pairings, &persisted_count)) {
        for (size_t index = 0; index < persisted_count; ++index) {
            const uint8_t peer_id = persisted_pairings[index].peer_id;
            if (bluetooth_restore_pairing(&bluetooth, peer_id)) {
                printf("Restored remembered Bluetooth LE Audio peer %u.\n",
                       (unsigned)peer_id);
            } else {
                printf("Failed to restore remembered Bluetooth LE Audio peer %u.\n",
                       (unsigned)peer_id);
            }
        }
    }

    printf("Pico headset transport ready.\n");
    printf("Local Bluetooth peer id: %u.\n", (unsigned)bluetooth.local_peer_id);
    if (bluetooth_runtime_has_transport(&bluetooth)) {
        printf("Bluetooth LE Audio headset transport ready; CYW43 controller initialized.\n");
    } else {
        printf("Bluetooth LE Audio headset transport unavailable; check CYW43 controller initialization.\n");
    }
    if (persisted_count == 0U) {
        printf("No persisted Bluetooth LE Audio headset pairings found; press the onboard button to pair a compatible headset.\n");
    } else {
        printf("Loaded %zu remembered Bluetooth LE Audio headset pairing(s).\n", persisted_count);
    }
    printf("This firmware targets a single paired Bluetooth LE Audio headset over a LE audio-oriented path and does not support Pico-to-Pico audio relays.\n");

#ifdef PICO_INTERCOM_TARGET
    while (true) {
        const bool pairing_button_pressed = !gpio_get(PICO_INTERCOM_PAIR_BUTTON_PIN);
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        bluetooth_poll(&bluetooth);
        if (!bluetooth_runtime_has_transport(&bluetooth) &&
            now_ms >= PICO_INTERCOM_TRANSPORT_STARTUP_TIMEOUT_MS) {
            if (!transport_warning) {
                printf("Bluetooth LE Audio headset transport is still unavailable after %u ms; verify the Pico SDK BTstack checkout, CYW43 firmware support, and any custom status LED pin override.\n",
                       (unsigned)PICO_INTERCOM_TRANSPORT_STARTUP_TIMEOUT_MS);
            }
            transport_warning = true;
        } else if (bluetooth_runtime_has_transport(&bluetooth)) {
            transport_warning = false;
        }
        if (bluetooth.pairing_completed && bluetooth.completed_pairing_peer_id != 0U) {
            const uint8_t paired_peer_id = bluetooth.completed_pairing_peer_id;
            pairing_t completed_pairing = make_pairing(paired_peer_id);
            printf("Persisting Bluetooth LE Audio peer pairing for peer %u.\n",
                   (unsigned)paired_peer_id);
            if (pairing_store_save(&pairing_store, &completed_pairing)) {
                printf("Bluetooth LE Audio peer pairing complete for peer %u; session is operational.\n",
                       (unsigned)paired_peer_id);
            } else {
                bluetooth.storage_error = true;
                pairing_error = true;
                printf("Bluetooth LE Audio peer session reached peer %u but pairing persistence failed.\n",
                       (unsigned)paired_peer_id);
            }
            bluetooth.pairing_completed = false;
            bluetooth.completed_pairing_peer_id = 0U;
        }
        if (pairing_button_pressed && !pairing_button_was_pressed) {
            pairing_button_press_started_ms = now_ms;
            pairing_button_hold_triggered = false;
        }

        if (pairing_button_pressed && !pairing_button_hold_triggered &&
            (now_ms - pairing_button_press_started_ms) >= PICO_INTERCOM_PAIR_BUTTON_HOLD_MS) {
            pairing_button_hold_triggered = true;
            pairing_in_progress = true;
            pairing_error = false;
            pairing_started_ms = now_ms;
            if (bluetooth_handle_pairing_button(&bluetooth, PICO_INTERCOM_PAIR_BUTTON_PEER_ID,
                                               true)) {
                printf("Bluetooth LE Audio peer pairing started for peer %u; waiting for session readiness.\n",
                       (unsigned)bluetooth.pairing_peer_id);
            } else {
                pairing_error = true;
                printf("Bluetooth LE Audio headset pairing could not start; no suitable compatible headset is ready.\n");
            }
        }

        if (!pairing_button_pressed && pairing_button_was_pressed && !pairing_button_hold_triggered) {
            if (intercom_toggle_ptt(&intercom)) {
                printf("PTT %s.\n", intercom.ptt_pressed ? "enabled" : "disabled");
            }
            pairing_button_press_started_ms = 0U;
        }

        const uint32_t elapsed_pairing_ms = now_ms - pairing_started_ms;
        if (pairing_in_progress && elapsed_pairing_ms >= PICO_INTERCOM_PAIRING_DISPLAY_MS) {
            pairing_in_progress = false;
        }

        if (bluetooth_runtime_is_operational(&bluetooth) && intercom.enabled &&
            intercom.ptt_pressed && (now_ms - last_audio_tick_ms) >= PICO_INTERCOM_AUDIO_POLL_MS) {
            (void)bluetooth_process_local_audio(&bluetooth, bluetooth.local_peer_id);
            last_audio_tick_ms = now_ms;
        }

        if (!have_reported_status || bluetooth.session_ready_peer_count != last_reported_ready_peers ||
            bluetooth.last_error_code != last_reported_error) {
            if ((now_ms - last_status_report_ms) >= 1000U ||
                bluetooth.session_ready_peer_count == 0U ||
                last_reported_ready_peers == 0U) {
                printf("Bluetooth LE Audio status: transport=%s, sessions=%zu, connected_peers=%zu, last_error=%s.\n",
                       bluetooth_runtime_has_transport(&bluetooth) ? "ready" : "down",
                       bluetooth.session_ready_peer_count, bluetooth.connected_peer_count,
                       bluetooth_error_name(bluetooth.last_error_code));
                last_status_report_ms = now_ms;
                last_reported_ready_peers = bluetooth.session_ready_peer_count;
                last_reported_error = bluetooth.last_error_code;
                have_reported_status = true;
            }
        }

        update_status_led(&bluetooth, pairing_in_progress, pairing_error, transport_warning, now_ms);
        pairing_button_was_pressed = pairing_button_pressed;
        sleep_ms(PICO_INTERCOM_PAIR_BUTTON_POLL_MS);
    }
#else
    return 0;
#endif
}
