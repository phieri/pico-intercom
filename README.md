# pico-intercom

A small C project for a Raspberry Pi Pico 2 W that models a local Bluetooth audio intercom.

## What is included

- An intercom routing core that rebroadcasts audio from one connected headset to every other connected headset while PTT is active.
- A simple Bluetooth runtime that records incoming audio packets, tracks connected headset peers, stores pairings, and relays audio while PTT is active.
- A Pico-target pairing persistence layer that writes peer IDs and labels to a reserved flash region so pairings survive reboot and can be restored on startup.
- A firmware entry point that uses the onboard button and status LED to trigger pairing, show connection state, and recover saved pairings automatically.
- A Pico SDK-based CMake build path that matches the firmware workflow used in `phieri/viking-bio-pwa`.
- A host-side test target that exercises the rebroadcast logic, Bluetooth shim, and pairing persistence without requiring the Pico toolchain.

## How it works

The current firmware model keeps a small peer list and rebroadcasts any audio payload to all peers except the source when the intercom is enabled and the PTT state is active. This gives a simple, self-contained model of a local audio intercom where connected headsets relay audio to one another.

## Pairing headphones in practice

A practical pairing flow for this model is:

1. Power on the Pico intercom firmware and let it boot into its idle state.
2. Put the headphones you want to use into pairing mode and keep them near the Pico.
3. Press the Pico 2 W onboard button to initiate pairing with the nearby headset. The status LED blinks while the request is active and stays solid once at least one peer is connected.
4. If the pairing succeeds, the firmware persists the new pairing to flash and restores it automatically on the next boot.
5. Keep the intercom in PTT mode while speaking; audio is rebroadcast to the other connected headsets automatically.
6. Use the runtime's disconnect or unpair flow when you want to remove a headset from the session.

The host-side tests and demo binary exercise the same pairing and relay flow, so you can validate the behavior locally before flashing hardware.

## Build the firmware

The repository includes a GitHub Actions workflow at `.github/workflows/build-firmware.yml` that mirrors the firmware build pipeline from `phieri/viking-bio-pwa`.

For a local host-side build and test run:

```sh
cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

For a Pico firmware build, install the Raspberry Pi Pico SDK and configure with:

```sh
cmake -S . -B build -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build
```

The build produces a `.uf2` image for flashing to a Raspberry Pi Pico 2 W board. The firmware target uses the Pico SDK's standard libraries and the intercom runtime's simple peer-pairing logic directly, with flash-backed persistence and a simple board interaction loop. The current implementation focuses on realistic firmware-state management and pairing recovery rather than a full wireless stack; the Bluetooth layer remains a practical embedded-runtime shim for the intercom logic.
