# pico-intercom

A small C project for a Raspberry Pi Pico 2 W that models a local Bluetooth audio intercom.

## What is included

- An intercom routing core that rebroadcasts audio from one connected headset to every other connected headset while PTT is active.
- A Bluetooth-facing shim that records incoming audio packets, tracks connected headset peers, and exposes relay stats that can later be wired to a real Pico W/Bluetooth stack.
- A pairing persistence layer that saves connected headset pairings so they can be reloaded across boots; on Pico targets this path is intended to use LittleFS-backed storage.
- A Pico SDK-based CMake build path that matches the firmware workflow used in `phieri/viking-bio-pwa`.
- A host-side test target that exercises the rebroadcast logic, Bluetooth shim, and pairing persistence without requiring the Pico toolchain.

## How it works

The current firmware model keeps a small peer list and rebroadcasts any audio payload to all peers except the source when the intercom is enabled and the PTT state is active. This gives a simple stand-in for a local audio intercom where connected headsets relay audio to one another.

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

The build produces a `.uf2` image for flashing to a Raspberry Pi Pico 2 W board.
