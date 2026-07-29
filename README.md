# pico-intercom

`pico-intercom` is a Raspberry Pi Pico 2 W firmware project for a local
**Bluetooth Classic headset intercom**.

The Pico acts as a Bluetooth Classic transport/controller node. It does not
capture analog microphone audio or drive analog speakers directly. Paired
headsets handle their own local audio I/O, while the Pico relays encoded
intercom frames, pairing state, and session control.

## Chosen wireless backend

- **Transport:** Bluetooth Classic headset transport over the Pico SDK BTstack integration
- **Board:** Raspberry Pi Pico 2 W
- **SDK support used:** `pico_btstack_classic`, `pico_btstack_cyw43`, `pico_cyw43_arch_none`
- **Audio model:** encoded intercom frames are exchanged with paired headsets and relayed across application sessions

Wi-Fi is not used by this project.

## Current hardware behavior

On hardware, the firmware:

1. boots and reports startup state over USB serial
2. initializes CYW43 and the Bluetooth Classic controller path
3. derives a stable local peer ID from the Pico unique board ID
4. restores remembered headset pairings from persistent storage
5. reconnects automatically when a remembered headset becomes available again
6. supports pairing, connect, disconnect, and reconnect flows for headset peers
7. performs an application-level session handshake before treating a link as usable
8. keeps the active intercom peer list in sync as sessions connect and disconnect
9. lets the onboard pairing button request pairing with a selected headset peer
10. only reports the intercom path as operational once a session is established
11. relays encoded intercom audio frames over the active Bluetooth Classic headset session while keepalives are healthy

## Audio architecture

- The Pico **does not have an analog audio path** in this project.
- The Pico **does not capture microphones or drive speakers directly**.
- Headsets remain responsible for their own local microphone and speaker handling.
- The firmware encodes and decodes intercom audio frames, queues them for
  transport, and routes them between established headset sessions.
- Host builds use software-generated frames so protocol and routing logic can be
  tested quickly without requiring Pico hardware.

## Intercom protocol

Runtime traffic is wrapped in a small application protocol carried inside the
Bluetooth Classic headset transport.

- `HELLO`: announces peer identity and starts or resets a session
- `HELLO_ACK`: confirms that the remote peer accepted the session
- `KEEPALIVE`: keeps an established session alive and helps detect stale links
- `AUDIO`: carries encoded audio frames
- `ERROR`: reports decode, routing, session, and saturation failures
- `GOODBYE`: requests a clean session shutdown

Each message carries a source peer ID, target peer ID, session ID, sequence
number, and acknowledgment sequence. The runtime drops duplicate or stale audio
frames, tracks missing sequence gaps, and does not mark the link operational
until the session handshake completes.

## Pairing and reconnect behavior

- Remembered headsets are stored in flash on Pico targets.
- On boot, the firmware reloads remembered headset IDs and reconnects
  automatically when those headsets become available again.
- Pairing is only persisted once the headset session is actually operational.
- Pairing, connection, transport, and session failures are reported over USB
  serial and reflected in runtime state.
- Dead sessions are torn down and stale peers are removed from the active relay
  list so reconnects can establish a fresh session cleanly.

## Practical limitations

- The repository models the Pico as a **Bluetooth Classic controller/relay** and
  validates the transport/session logic primarily through the fast host build.
- Hardware validation still requires paired Bluetooth Classic headsets and live
  on-device testing outside this environment.
- The host runtime uses software-generated sample frames to keep tests fast and
  deterministic.

## Hardware requirements

- Raspberry Pi Pico 2 W
- USB cable for flashing and serial logs
- one or more Bluetooth Classic headsets compatible with the intended deployment

## Build and test

### Host build and tests

Use the fast host path first:

```sh
cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Firmware build

The Pico SDK checkout must include BTstack support. Configure the firmware build
with a valid SDK path before building:

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

1. Flash the firmware to the Pico 2 W.
2. Open the USB serial console.
3. Wait for the Pico to report its local Bluetooth peer ID and controller status.
4. Restore remembered headsets automatically on boot, or press the onboard
   pairing button to start pairing a headset peer.
5. Wait for session handshake completion before treating the link as operational.
6. Confirm that the serial log reports at least one ready session before
   expecting audio relay traffic.

If the radio drops, the session times out, or the headset resets, the runtime
reports the failure explicitly, clears the stale session, and allows remembered
headsets to reconnect with a fresh session.

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

The host test suite covers protocol framing, classic transport queueing,
pairing persistence, reconnect behavior, session establishment, audio relaying,
duplicate-packet rejection, and timeout handling.

## Source layout

- `src/bluetooth.c` - high-level runtime state, command handling, audio relay integration
- `src/bluetooth_transport.c` - Classic headset transport queues, peer bookkeeping, and reconnect logic
- `src/bluetooth_classic.c` - Classic headset stack wrapper used by the Pico runtime and host tests
- `src/pairings.c` - host file storage and Pico flash-backed pairing persistence
- `src/main.c` - Pico firmware entrypoint and button/LED loop
- `tests/test_intercom.c` - host-side regression coverage
