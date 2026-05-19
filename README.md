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

```sh
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-local --target us3_turbo_access_client us3_turbo_access_gds_example
```

## Run the end-to-end test

```sh
scripts/restart_and_test.sh
```

The script kills any running gateway on port 18082, foreground-launches a fresh
gateway, runs the GDS example (PUT / HEAD / GET / `same=true`), and stops the
gateway on exit.
