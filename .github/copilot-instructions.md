# Copilot instructions for pico-intercom

## Repository overview

This repository contains a small C codebase for a Raspberry Pi Pico 2 W that models a local Bluetooth audio intercom. The code is split between:

- `src/`: core implementation for intercom routing, Bluetooth shim behavior, and pairing persistence.
- `include/`: public headers for the intercom, Bluetooth, and pairing modules.
- `tests/`: host-side tests that exercise the core logic without requiring the Pico SDK.
- `.github/workflows/build-firmware.yml`: the canonical firmware build workflow used in CI.

## Preferred workflow for fast iteration

When making changes, prefer the host-side build/test path first because it is much faster and does not require the Pico SDK or ARM toolchain:

```sh
cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

This builds the host demo target (`pico_intercom_app`) and the test binary (`intercom_tests`).

## Build and test commands

- Host build/tests (default for local iteration):
  ```sh
  cmake -S . -B build -DPICO_INTERCOM_FORCE_HOST_BUILD=ON -DPICO_INTERCOM_BUILD_HOST_TESTS=ON
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```
- Pico firmware build (requires the Pico SDK and ARM toolchain):
  ```sh
  cmake -S . -B build-firmware -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=/path/to/pico-sdk
  cmake --build build-firmware
  ```
  The CI workflow uses the same general approach and installs the required cross-compilation toolchain packages.

## Project conventions

- The codebase is C (C11) with a small amount of C++-style build glue in CMake; keep changes compatible with the existing CMake and compiler settings.
- The main CMake entry point is `CMakeLists.txt`; it switches between a host build and a Pico firmware build based on `PICO_INTERCOM_FORCE_HOST_BUILD`.
- New behavior changes should usually be reflected in `tests/test_intercom.c` so the host-side test suite continues to cover the core logic.
- Keep build artifacts out of source control; use separate build directories such as `build` and `build-firmware` for local work.

## Important gotchas and known issues

- A fresh checkout does not automatically contain a populated Pico SDK directory. If you try to configure a firmware build before the SDK is present, CMake will fail with an error like:
  ```text
  Directory '/.../pico-sdk' not found
  ```
  Workaround:
  - Populate `pico-sdk/` with the Raspberry Pi Pico SDK (version 2.3.0, matching the CI workflow), or
  - Point CMake at a valid SDK location with `-DPICO_SDK_PATH=/path/to/pico-sdk`, or
  - Set `-DPICO_SDK_FETCH_FROM_GIT=ON` so the SDK can be fetched by CMake.
- The CI workflow installs the firmware toolchain packages (`cmake`, `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`, and build essentials). If a local firmware build fails because of missing ARM cross-compiler tools, install the same packages before retrying.
- The repository’s README already documents the host-side build/test path; use that as the first validation step before attempting firmware builds.

## Files to inspect first when debugging

- `CMakeLists.txt` for build targets and options.
- `README.md` for the documented local workflow and project description.
- `.github/workflows/build-firmware.yml` for the canonical CI build steps.
- `tests/test_intercom.c` for examples of expected behavior and regression coverage.
