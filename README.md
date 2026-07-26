# pico-intercom

pico-intercom is a Raspberry Pi Pico 2 W firmware project for a local Bluetooth audio intercom. The codebase has been refactored from a demo-style simulation into a more production-oriented embedded runtime with explicit connection state handling, persistent pairing storage, and a hardware-driven pairing workflow.

## What changed

The firmware now provides:

- A modular intercom routing core that relays audio packets to connected peers while PTT is active.
- A Bluetooth runtime with explicit peer states (`disconnected`, `connecting`, `connected`, `disconnecting`) and error tracking for failed connections and disconnects.
- Flash-backed pairing persistence for Pico targets, with write validation instead of best-effort storage.
- A practical pairing flow driven by the onboard button and status LED.
- Host-side tests that exercise the routing core, Bluetooth runtime, and pairing persistence without requiring hardware.

## Hardware requirements

- Raspberry Pi Pico 2 W board
- USB cable for flashing and serial console
- Optional: a Bluetooth headset or test device that can be paired to the Pico runtime conceptually

The firmware uses the Pico SDK's GPIO and flash support and is designed to run from the Pico 2 W's onboard flash.

## Firmware behavior

On boot, the firmware:

1. Initialises the intercom routing state and enables relay handling.
2. Restores any persisted pairings from flash-backed storage.
3. Waits for the onboard button to trigger a pairing attempt.
4. Uses the onboard LED to indicate pairing progress, pairing errors, or a healthy connected state.

When a pairing is initiated, the runtime:

- marks the target peer as `connecting`,
- attempts to add the peer to the runtime's connected-peer list,
- records the resulting connection state,
- and persists the pairing metadata to flash.

Audio packets are relayed to all connected peers except the source while the intercom is enabled and PTT is active.

## Pairing workflow

1. Power on the Pico intercom firmware.
2. Press the onboard button to start a pairing attempt for the default test peer ID.
3. Observe the status LED:
   - blinking quickly: pairing or connection error
   - blinking steadily: pairing in progress
   - solid: at least one peer is connected
4. The pairing metadata is persisted so it can be restored across reboots.

## Build and test

### Host build and tests

```sh
cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Pico firmware build

Install the Raspberry Pi Pico SDK and then configure the firmware build:

```sh
cmake -S . -B build-firmware -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build-firmware
```

The build produces a `.uf2` image suitable for flashing to the Pico 2 W.

## Flashing

1. Put the Pico 2 W into BOOTSEL mode.
2. Copy the generated `.uf2` image to the mounted Pico drive.
3. Reboot the board and observe the serial console for startup and pairing messages.

## Known limitations

- The repository currently provides a realistic embedded runtime and flash-backed persistence layer, but it does not implement a full low-level BLE transport stack in this codebase.
- Pairing is currently represented as a managed runtime connection flow for the target peer ID used by the onboard button workflow.
- Production deployments that require full Bluetooth interoperability should wire this runtime into a hardware-appropriate transport layer (for example, a Pico SDK-compatible BLE stack) in a follow-up integration step.
