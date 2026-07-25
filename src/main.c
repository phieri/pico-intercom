#include "bluetooth.h"

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
    static const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};

    intercom_init(&intercom);
    intercom_enable(&intercom, true);
    intercom_add_peer(&intercom, 2U);
    intercom_add_peer(&intercom, 3U);
    intercom_set_ptt(&intercom, true);

    bluetooth_init(&bluetooth, &intercom);
    bluetooth_handle_audio(&bluetooth, 1U, payload, sizeof(payload));

    printf("Pico intercom skeleton ready.\n");
    printf("Relayed packets: %zu\n", bluetooth.last_relay_count);
    return 0;
}
