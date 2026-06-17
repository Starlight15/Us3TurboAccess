# Us3TurboAccess

GPU-aware turbo-access client SDK and reference gateway for object storage.

The client SDK uses NVIDIA GPUDirect Storage (cuObject) as its data path and brpc
as the control / chunk RPC transport. The gateway in this repo is a reference
implementation used for end-to-end testing.

## Layout

```
client/      C++ SDK (libus3_turbo_access_client)
gateway/     reference gateway server (us3_turbo_access_gateway)
examples/    end-to-end examples (us3_turbo_access_gds_example, ...)
scripts/     dev helpers (restart_and_test.sh)
include/     project-wide shared headers (us3_turbo_access::common)
```

Public client API lives in `client/include/us3_turbo_access/client/`. Everything
under `client/src/` is internal.

## Build

### 1. Build dependencies (first time only)

Build static dependencies from the bundled source tarballs:

```sh
./third_party/build_deps.sh
```

This builds protobuf, brpc, spdlog, and abseil into `third_party/install/`. Run once per checkout, or after `git clean -fdx`.

### 2. Build the project

```sh
./do_make.sh
```

Or manually:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

Artifacts are written to `build/`.

## Run the end-to-end test

```sh
scripts/restart_and_test.sh
```

The script kills any running gateway on port 18082, foreground-launches a fresh
gateway, runs the GDS example (PUT / HEAD / GET / `same=true`), and stops the
gateway on exit.
