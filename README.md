# pico-intercom

A minimal C project skeleton for a Raspberry Pi Pico 2 W audio intercom firmware.

## What is included

- A small intercom routing library that models PTT-driven rebroadcasting between peers.
- A Bluetooth-facing shim that can later be wired to a real Pico W/Bluetooth stack.
- A Pico SDK based CMake build path that matches the firmware workflow used in `phieri/viking-bio-pwa`.
- A host-side test target that exercises the rebroadcast logic without requiring the Pico toolchain.

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
