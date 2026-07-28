# pico-intercom

pico-intercom is a Raspberry Pi Pico 2 W firmware project for a local Bluetooth audio intercom. The repository now contains a real target-side runtime for the Pico 2 W that initializes the CYW43 wireless core and a hardware-backed audio backend rather than relying on synthetic placeholders.

## What changed

The firmware now provides:

- A modular intercom routing core that relays audio packets to connected peers while PTT is active.
- A target-side Bluetooth runtime that initializes the Pico W radio stack through the Raspberry Pi Pico SDK and brings up the CYW43 wireless backend for runtime status and future transport work.
- A hardware-backed audio path that uses the Pico SDK ADC/PWM peripherals for capture/playback on the Pico target.
- Flash-backed pairing persistence for Pico targets with verified writes instead of best-effort storage.
- Host-side tests that exercise the routing core, Bluetooth runtime, pairing persistence, and transport logic without requiring hardware.

## Hardware requirements

- Raspberry Pi Pico 2 W board
- USB cable for flashing and serial console
- Onboard button (GPIO 14 by default) to trigger pairing
- Onboard LED (GPIO 25 by default) to indicate runtime state
- Optional analog audio hardware:
  - ADC input on GPIO 26 (ADC0) for a microphone or analog signal source
  - PWM output on GPIO 16 for a small speaker or earphone driver

The audio backend uses a simple, practical path that works with an analog microphone or other ADC source and a PWM-driven output stage. The build does not require a codec or I2S peripheral.

## Firmware behavior

On boot, the firmware:

1. Initializes the intercom routing state and enables relay handling.
2. Initializes the Pico W radio backend through the CYW43 driver and brings the wireless backend online.
3. Restores any persisted pairings from flash-backed storage.
4. Waits for the onboard button to trigger a pairing attempt.
5. Uses the onboard LED to indicate pairing progress, pairing errors, or a healthy connected state.

When a pairing is initiated, the runtime records the target peer in the runtime state, marks the radio backend as active, and persists the pairing metadata to flash.

Audio packets are queued through the existing transport layer and relayed to peers while the intercom is enabled and PTT is active. On the Pico target the audio subsystem uses hardware capture/playback callbacks rather than a purely synthetic frame generator.

## Build and test

### Host build and tests

```sh
cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Pico firmware build

The repository includes a local Pico SDK checkout, so the firmware build can be configured directly:

```sh
cmake -S . -B build-firmware -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=$PWD/pico-sdk
cmake --build build-firmware
```

The build produces a `.uf2` image suitable for flashing to the Pico 2 W.

## Flashing

1. Put the Pico 2 W into BOOTSEL mode.
2. Copy the generated `.uf2` image to the mounted Pico drive.
3. Reboot the board and observe the USB serial console for startup and pairing messages.

## Known limitations

- The current firmware uses a practical Pico SDK CYW43 runtime initialization path and a hardware ADC/PWM audio backend. It is intended as a buildable, flashable runtime and demonstration firmware rather than a full-featured Bluetooth Classic or advanced codec implementation.
- Actual peer-to-peer interoperability depends on a second Pico or a compatible device with a matching transport layer; the current build is focused on bringing up the radio and audio hardware cleanly.
- The audio path uses a simple ADC/PWM bridge and is intended for development and demonstration purposes; for production quality audio you would typically use a dedicated microphone codec and DAC/I2S path.
