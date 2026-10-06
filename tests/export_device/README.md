# Read-only-input on-device export check

`run.py` builds the real modern dataset-texturing backend as a standalone ARM64
program with the production Dataset/Image code and existing NDK-built JPEG/PNG
archives. It reads a saved dataset and its generated `model.obj` on the phone;
captured images stay on the phone. It does not install/restart the scanner.

The backend writes into a unique `/data/local/tmp/scanner-export-*` directory.
The probe checks that output face count equals input face count and reports
textured-face coverage, atlas/tile dimensions, photo scaling, scratch size,
native export duration and process peak RSS. These are not whole-app timings or
an independent proof of every triangle's geometric identity; the backend's host
tests cover detailed geometry and visibility. Input OBJ and commit-state hashes
must match before/after, as must compiled source and codec-archive hashes.

```sh
python3 tests/export_device/run.py \
  --sdk /tmp/opencode/pixelshare-sdk --serial "$PIXEL_SERIAL" \
  --codec-dir /tmp/opencode/scanner-modern-integration/app/intermediates/cxx/Debug/2w2f4e1q/obj/local/arm64-v8a \
  --dataset '/storage/emulated/0/Documents/3D Live Scanner/example.dataset' \
  --output /tmp/opencode/new-export-verification
```

All remote test files are removed afterwards. `--keep-output` retains only a
successful export's scratch directory for further inspection; its binary is
still removed. The program is time-limited on the device and the host. Use a
stable saved dataset, not an actively changing capture. Never treat failed or
timed-out runs as successful exports.

The source of the reported October 1 failure was inspected without transferring
images: 263 frames, a 724,468,693-byte OBJ, 7,558,107 positions and normals,
2,519,369 triangles, and 1,462 material runs. Those dimensions exceed the old
32 MiB loader and 500,000-face limits; the old material preflight also constrains
material count. Output success must be established with the new backend, not by
raising those old limits and assuming it works.
