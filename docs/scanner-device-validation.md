# Scanner device validation

Host regression checks cannot establish AR tracking quality, graphics performance,
or Android layout correctness. Use this checklist alongside the automated checks
when comparing scanner builds on a supported device.

Current target: the Pixel original-engine `quality` candidate. See
[current status and pacing diagnostics](quality-modernization.md) for its baseline,
settings and remaining acceptance work. Historical experimental-engine results
are not quality acceptance for this candidate.

## Record the comparison conditions

- Device, Android version, AR provider and provider version.
- Build revision, scan resolution, depth mode and enabled postprocessing options.
- Lighting, scanning distance and the same subject/route for both builds.
- Whether screen/video recording is enabled, and whether the device is thermally
  throttled. Compare warmed-up runs as well as the initial run.

Test Google ARCore and Huawei AREngine separately where compatible hardware is
available. A successful test on one provider does not validate the other.

## Geometry and recovery

Record the exact message and whether preview, capture, or both stop. A transient
bad frame should not require discarding the scan; subsequent usable frames should
be accepted. Check quality independently from preview smoothness.
Wireless ADB identified the device as a **Google Pixel 9 Pro XL**, Android 17,
with 4 KiB kernel pages. Prioritize original-engine quality and tracking recovery;
a true 16 KiB-kernel runtime check remains separate.

1. Scan a stationary, textured, matte object slowly from several viewpoints.
   Check that ordinary surfaces accumulate normally and export successfully.
2. Briefly cover the camera, point at a blank wall, and move rapidly. Check that
   tracking/depth interruptions do not create long spikes, sheets through the
   scene, non-finite vertices, or crashes.
3. Return to the original object. Previously accepted geometry should remain;
   capture should resume when tracking and depth are usable again.
4. Include reflective, dark and thin surfaces. Record missing geometry separately
   from fabricated geometry; filtering should not be evaluated only on easy
   subjects.
5. Exercise repeated pause/resume, background/foreground transitions and a new
   scan after an interrupted scan. Look for stale poses or points crossing scans.
6. Save, reopen and export the result. Inspect both the preview and the exported
   mesh; a good preview alone does not establish valid reconstruction output.

For each failure, record the last action, approximate distance, scan settings,
provider, a screenshot or short recording, and relevant logcat output.

Historical modern-engine recovery trials and timing evidence are retained in
[capture I/O](../tests/capture_io/README.md). They predate the quality rollback.

## Performance

- Replay the same route with a small and a large accumulated scan. Compare frame
  pacing, input latency, reconstruction latency and memory growth.
- Measure sustained operation, not just startup. Watch for frame-time growth as
  the mesh grows, repeated allocations and repeated texture/buffer uploads.
- Compare recording disabled and enabled, and timestamp overlays off and on.
- Use an Android system trace/Perfetto capture to distinguish UI-thread stalls,
  reconstruction work, GL-thread work and GPU pressure. Android UI frame metrics
  alone may not describe a separately rendered GL preview.
- Report median and tail frame times for comparable runs, duration and memory
  observations. Do not infer an FPS improvement from reduced call counts alone.

## On-device data and exported meshes

- Import and reopen existing OBJ/PLY/dataset archives. Compare successful output
  with the original files and check that a failed import does not overwrite an
  existing scan or present a partial archive as a completed scan.
- Interrupt an import/export, then retry. Test a full/low-space destination and
  unreadable or truncated input, and check the reported failure and cleanup.
- Browse folders containing identically named scans, scroll rapidly, change the
  grid density, and leave/reopen the browser. Verify thumbnail identity and
  responsiveness as well as bounded cache/worker memory.
- For mesh changes, record vertices, triangles, bounds, material assignments and
  normal orientation. Test separate surfaces that nearly touch, thin features,
  texture seams and disconnected components. A reduction in file size or triangle
  count is useful only if the geometry and appearance remain correct.
- Check a known-length object and a closed scanning route for scale, pose drift
  and duplicated/misaligned surfaces. Offline mesh cleanup does not by itself
  demonstrate improved camera-pose alignment.

## Android interface

- Confirm the camera preview starts, remains visible and responds to gestures.
- Exercise start, pause, finish, discard, save, reopen, rename, export and share.
- Share an OBJ with materials/textures and a PLY from the viewer and library
  through Android's share sheet. Open the received files in another app and
  verify the model data, relative texture paths and URI read permission. Cancel
  the chooser and repeat sharing another model; the first shared file should
  remain unchanged. Test a device with no compatible receiving app too.
- Check permission denial, tracking interruptions, empty model lists and long
  filenames. Status text should describe a useful next action.
- Check portrait/landscape where supported, narrow screens, display cutouts,
  gesture navigation and larger system font sizes. Controls must remain visible
  and tappable without covering essential scan guidance.
- Check TalkBack labels, focus order, selected/disabled states and touch targets.
- Check dialog dismissal and the Android Back action, including cancellation
  during a scan or a file operation.
- Repeat background/resume and GL-context recreation after interacting with the
  controls; modernized widgets must not change capture or resource lifetimes.
