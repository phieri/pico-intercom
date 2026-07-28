# pico-intercom

`pico-intercom` is a Raspberry Pi Pico 2 W firmware project for a local **Bluetooth Low Energy** intercom.

The host build still uses a software transport for fast tests, but the Pico 2 W firmware now uses the CYW43 radio with **BTstack BLE** for real on-device discovery, pairing, connection, session negotiation, keepalive handling, and packet relay.

## Chosen wireless backend

- **Transport:** Bluetooth Low Energy (BLE) over the Pico SDK BTstack integration
- **Board:** Raspberry Pi Pico 2 W
- **SDK support used:** `pico_btstack_ble`, `pico_btstack_cyw43`, `pico_cyw43_arch_none`
- **GATT profile:** custom intercom service with
  - notify characteristic for peer-to-peer intercom frames
  - write-without-response characteristic for low-latency uplink frames

Wi-Fi is not used by this project.

## Current hardware behavior

On hardware, the firmware:

1. boots and reports startup state over USB serial
2. initializes CYW43 and BTstack
3. derives a stable local peer ID from the Pico unique board ID
4. advertises a BLE intercom service and scans for other Pico intercom peers
5. restores remembered peer IDs from flash-backed pairing storage
6. attempts reconnect when a remembered peer is discovered again
7. performs an application-level session handshake before treating a link as usable
8. lets the onboard pairing button request pairing with a discovered peer
9. only reports the intercom path as operational once a session is established
10. relays intercom audio frames over the BLE link while keepalives are healthy

## Intercom protocol

Runtime traffic is wrapped in a small application protocol carried inside the custom
BLE GATT characteristics.

- `HELLO`: announces peer identity and starts or resets a session
- `HELLO_ACK`: confirms that the remote peer accepted the session
- `KEEPALIVE`: keeps an established session alive and helps detect stale links
- `AUDIO`: carries encoded PCM frames
- `ERROR`: reports decode, routing, session, and saturation failures
- `GOODBYE`: requests a clean session shutdown

Each message carries a source peer ID, target peer ID, session ID, sequence number,
and acknowledgment sequence. The runtime drops duplicate or stale audio frames,
tracks missing sequence gaps, and does not mark the link operational until the
session handshake completes.

## Practical limitations

- The current firmware is built around a **single active BLE peer link at a time** on hardware.
- Audio is still software-generated PCM in this repository; no microphone codec or speaker DAC path is wired yet.
- BLE bandwidth is lower than Wi-Fi, so this is a practical firmware path for real radio validation and transport integration, not a production audio stack.
- Pairing is based on BLE discovery plus bonding/connection state through BTstack; there is no UI beyond USB serial logs and the onboard button/LED.
- CI validates the host-side logic and the firmware build, but it cannot prove live radio interoperability inside this environment.

## Hardware requirements

- Raspberry Pi Pico 2 W
- USB cable for flashing and serial logs
- a second Pico 2 W running the same firmware for peer testing

## Build and test

### Host build and tests

Use the fast host path first:

```sh
cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Firmware build

The Pico SDK checkout must include its BTstack submodule support. Verify that
`$PICO_SDK_PATH/lib/btstack` exists before configuring the firmware build. If
it is missing, populate the SDK recursively or initialize the BTstack submodule
inside the SDK checkout first before continuing.

```sh
cmake -S . -B build-firmware -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=$PWD/pico-sdk
cmake --build build-firmware
```

The build produces a UF2 image for the Pico 2 W.

## Flashing

1. Hold **BOOTSEL** while connecting the Pico 2 W over USB.
2. Copy `build-firmware/pico_intercom.uf2` to the mounted mass-storage device.
3. Reboot the board.
4. Open the USB serial console.

## Running on hardware

1. Flash the same firmware to two Pico 2 W boards.
2. Open USB serial on both boards.
3. Wait for each board to report its local Bluetooth peer ID and that the BLE transport is ready.
4. Press the onboard pairing button on one board once the other board is visible.
5. Watch the serial log for discovery, pairing request, session handshake, and operational-state messages.
6. Confirm that the log reports at least one active session before expecting audio traffic.
7. Once connected, the firmware will start relaying generated intercom frames over BLE while PTT is active.

If the radio drops, the session times out, or the peer resets, the runtime reports the
failure explicitly and falls back to scanning so remembered peers can reconnect.

## Pairing persistence

- Remembered peers are stored in flash on Pico targets.
- On boot, the firmware reloads remembered peer IDs and reconnects when those peers advertise again.
- Pairing is only persisted once the peer session is actually operational.
- If flash persistence fails, the runtime reports it over USB serial and keeps the transport marked degraded.

## Validation

- Fast regression path:
  ```sh
  cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```
- Firmware build path:
  ```sh
  cmake -S . -B build-firmware -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=$PWD/pico-sdk
  cmake --build build-firmware
  ```

The host test suite covers protocol framing, pairing persistence, session establishment,
audio relaying, duplicate-packet rejection, and timeout handling. Hardware validation
still requires two physical Pico 2 W boards.

## Source layout

- `src/bluetooth.c` - high-level runtime state, command handling, audio relay integration
- `src/bluetooth_transport.c` - transport queues and peer bookkeeping
- `src/bluetooth_classic.c` - Pico target BLE runtime backed by BTstack
- `src/pairings.c` - host file storage and Pico flash-backed pairing persistence
- `src/main.c` - Pico firmware entrypoint and button/LED loop
- `tests/test_intercom.c` - host-side regression coverage
