# Pixel depth inference: production integration findings

The initial code-path audit below informed the now-installed, explicitly enabled
[experimental live test mode](experimental-tpu-test.md). Production approval is
still separate. The implementation uses an owned native input packet and captured
measured-point links, then runs inference in the existing reconstruction worker.
Requirements below distinguish remaining sensor validation from those already
addressed by the test build.

## Correct input boundary

`common/arcore/arcore.cc::ARCore::UpdateFeaturePoints()` obtains validated native
depth/confidence planes before converting samples to points. The useful snapshot
boundary is after depth/confidence and optional secondary-depth acquisition,
before sampling, interpolation and filtering. `ReadPlane()` and
`common/arcore/geometry_validation.h::Plane` already validate runtime dimensions,
row/pixel stride and accessible byte length.

- D16 values are millimetres along the camera principal axis; zero means missing.
- Confidence is a separate uint8 plane, not the point-cloud `w` field.
- Pack using both strides; a plane is not necessarily contiguous `uint16_t[H*W]`.
- ARCore image pointers are borrowed and released on function exit. Asynchronous
  processing needs an owned copy, not retained `ArImage` plane pointers.
- The prototype's `[1,120,160,3]` means native **width 160, height 120**. Confirm
  the phone's native shape; do not infer it from portrait RGB or reshape a
  transposed plane just because byte counts match. Cache shape-specific graphs
  using the same fully convolutional weights rather than resizing measurements.

## Existing policy must remain identifiable

The existing path can already subsample widths above 240, crop hardware-depth
planes, fill low-confidence holes from secondary depth, and replace some raw
values. Confidence thresholds are `>32` for hardware depth and `>128` otherwise.
Accepted depth becomes point confidence `w=1`; original confidence is lost.
It also applies depth/range/offset checks and different duplicate-timestamp rules
for hardware versus software/reprojected depth.

Consequently, a network preserving raw zeros alone does not prove unchanged
downstream holes or admission. Compute original admission/fill decisions from
the original planes; correct only eligible measured samples, leave synthetic
fills unchanged, and reject corrections that alter range/validity admission.
Removing existing sampling or changing fill policy would be a separate change.

## Thread and frame ownership

This acquisition currently runs through `App::OnDrawFrame()` on the GL thread
under `render_mutex_`. **Do not compile or execute NNAPI synchronously there.**

An owned packet must carry native dimensions, original depth and confidence,
optional secondary depth, camera-frame and depth timestamps, session/scan
generation, view/projection, native-to-screen mapping, resolution and offset.
Compile/warm/cache on a worker; use a bounded nonblocking handoff. Never apply a
late result with a newer pose/RGB image or integrate both fallback and corrected
versions of the same frame. Camera and depth timestamps are both needed because
raw-depth reprojections can reuse a depth timestamp.

The existing nonblocking frame admission and reconstruction worker live in
`common/thread/reconstr.cc`; world-to-colour-camera conversion and prior estimates
are handled by `common/tango/retango.cc`. A pure packet-consuming point generator
is preferable to a worker reading mutable live ARCore/camera members.

Pause, clear, tracking loss, session replacement and geometry changes must
invalidate outstanding results without joining a slow driver under the render
lock. Unsupported devices/shapes, compilation/execution failures, busy queues
and late results must use the unchanged original path.

## Compatibility and actual-data validation

The scanner remains minSDK 24. Explicit NNAPI device selection requires API 29;
an optional production adapter needs runtime API checks and `dlopen`/`dlsym` or
an optionally loaded implementation. Adding a mandatory `DT_NEEDED
libneuralnetworks.so` would break older supported devices. No such dependency has
been added by the research tools.

There is currently **no raw depth-plus-confidence snapshot writer**. Saved
`.pcl/.mat` recordings contain processed points, not original sensor planes,
strides, holes or confidence. They cannot establish the native input distribution
or replace raw-data validation. A future explicitly enabled diagnostic capture
must snapshot the input boundary and its transforms/timestamps, use bounded
worker-side disk writing, and include complete device/source provenance.

The synthetic noise used by the current training experiments has not been fitted
to Pixel sensor measurements. Real raw-plane validation and a controlled geometry
reference are required before claiming actual scan-accuracy improvement.
