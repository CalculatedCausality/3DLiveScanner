# Synthetic recorded-dataset interchange fixture

```sh
python3 tests/reconstruction/generate_dataset.py /path/to/new-fixture-directory
```

Requires Pillow. Existing output directories are refused. The generator creates
three 64×64 JPEG 4:4:4 frames observing a checkerboard plane at world Z=2 metres
from translated cameras, with 1,024 points per frame. No real scene data is used.

It writes the existing little-endian `.pcl` and `.bin` formats, column-oriented
GLM `.mat` text, `.tms`, distortion, yaw and state files. The state commit marker
is written last. Colour cameras use +Z forward/+Y down; the corresponding GL
camera is colour pose times Rx(pi). `fixture.json` records the ground truth.

The `.bin` preview is an analytic format/ownership fixture. It must not be reported
as mesh output produced by the new or legacy engine. Use raw points/poses for
reconstruction tests, and use the preview to validate compatibility with existing
dataset readers and history ownership.

Run the recorded-format integration together with the texturing fixtures:

```sh
python3 tests/reconstruction/texturing_run.py --dataset /path/to/fixture-directory
```

The initial generated fixture is stored outside the repository at
`/tmp/opencode/scanner-synthetic-dataset-16kb-20260930`. Actual native reader,
backend replay, texturing and Android interoperability results must be recorded
separately; generation alone does not establish them.

The host integration passed with real Dataset/Image/core code and modern colour
conversion: all three frames validated, the owned volume produced 3,354 triangles,
and maximum vertex-to-plane Z error was zero in this fixture. Host GL/log headers
and the texturing runner's documented legacy-JPEG shift-instrumentation exclusion
still apply. No Android runtime claim follows from this host result.
