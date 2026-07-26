# pico-intercom

A small C project for a Raspberry Pi Pico 2 W that models a local Bluetooth audio intercom.

## What is included

- An intercom routing core that rebroadcasts audio from one connected headset to every other connected headset while PTT is active.
- A simple Bluetooth runtime that records incoming audio packets, tracks connected headset peers, stores pairings, and relays audio while PTT is active.
- A pairing persistence layer that saves connected headset pairings so they can be reloaded across boots; on Pico targets this path is intended to use LittleFS-backed storage.
- A Pico SDK-based CMake build path that matches the firmware workflow used in `phieri/viking-bio-pwa`.
- A host-side test target that exercises the rebroadcast logic, Bluetooth shim, and pairing persistence without requiring the Pico toolchain.

## How it works

The current firmware model keeps a small peer list and rebroadcasts any audio payload to all peers except the source when the intercom is enabled and the PTT state is active. This gives a simple, self-contained model of a local audio intercom where connected headsets relay audio to one another.

## Pairing headphones in practice

A practical pairing flow for this model is:

1. Put the headphones you want to use into pairing mode and keep them near the Pico.
2. Power on the Pico intercom firmware and make the runtime available over a simple serial or command interface.
3. Enable Bluetooth and register the headset as a peer using the runtime's `pair`/`connect` commands (for example `pair 2` or `connect 2`). Choose a unique numeric peer ID for each headset; `2` is just an example. The runtime already exposes `bluetooth_handle_command()` and `bluetooth_execute_command()` helpers in `include/bluetooth.h` and `src/bluetooth.c` to parse and run those command aliases.
4. Keep the intercom in PTT mode while speaking; audio is rebroadcast to the other connected headsets automatically.
5. Use `disconnect`/`unpair` when you want to remove a headset from the session.

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

The build produces a `.uf2` image for flashing to a Raspberry Pi Pico 2 W board. The firmware target uses the Pico SDK's standard libraries and the intercom runtime's simple peer-pairing logic directly, without any extra Bluetooth shim layer.
