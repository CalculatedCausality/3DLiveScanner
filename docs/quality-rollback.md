# Quality rollback to the original reconstruction engine

## Reason

The user reports that scans still become oversized, accumulate floating fragments,
and become mangled when revisiting existing areas. The earlier local appearance
improvement did not establish acceptable overall scan quality. Subsequent capture
and reference changes did not meet the real-device acceptance criterion.

Further geometry/performance experimentation is paused. Do not redeploy the
experimental working tree merely because its unit tests or numerical replay
checks pass. Those checks did not demonstrate the required real capture quality.

## Selected historical baseline

`artifacts/3DLiveScanner-modernized-debug-2026-09-29.apk`

SHA-256: `0b1e81ecfcaabcf04494de38ff98754b1ca0e4abf7d99cdfdebb9476b1f51d0e`.

This is the earliest archived modernization build. It packages the original
`libtango_3d_reconstruction.so`, rather than the replacement reconstruction core.
It predates the capture-speed and mesh-reduction experiments. It is an archived
development APK, not an assertion that an unmodified store-signed release has
been recovered.

Verified before rollback:

- Application ID `com.lvonasek.arcore3dscanner`, version `220022` /
  `2022-build_0022`, ARM64, minimum API 24, target API 33.
- Signature matches the development key used by the installed builds.
- Pixel 9 Pro XL reports a 4 KiB kernel page size. This historical vendor build
  is not claimed to support a 16 KiB kernel.

## Restoration policy

- In-place `adb install -r -t`; no uninstall and no data clearing.
- Stop only after the user confirms that the active scan is saved.
- Back up app-private files and preferences before installation. Back up the
  historical public working dataset if it exists. Keep backups private and
  outside the repository.
- Preserve the selected saved-library location and unrelated preferences.
- Restore 2 cm resolution, **4 m camera measurement range**, space clearing on,
  and the original geometry-offset setting. Disable experimental TPU correction
  and coarse preview. Retain capture-first saving for data preservation.
- Verify installed APK identity and saved-library/capture-state preservation.

The 4 m limit is distance from the camera, not a limit on total scan extent.
The experimental profile had clearing disabled and a 100 m measurement limit;
that was not an original-settings baseline comparison.

Rollback work and backups:
`/tmp/opencode/scanner-original-engine-rollback-20261001`.
Candidate audit: `/tmp/opencode/scanner-build-verification/rollback-candidate-20261001.json`.

## Acceptance

The rollback completed successfully. The private backup contains **1,246,390,784
bytes** and 1,228 archive entries. All 11 pre-existing saved-library entries
remained present both after installation and after the user test. Installed APK
identity and private capture-state preservation were verified.

The user tested a fresh scan and answered **“Quality is back.”** The post-test
app process was PID 9857, with an empty scoped crash buffer. This establishes
the restored build/configuration as the real-device quality baseline. It does
not isolate the contribution of every engine, capture-policy and settings change
that was rolled back together.

The previous experimental code and diagnostic evidence are retained; no Git
reset or destructive cleanup was performed. **The working tree is not the source
of the currently installed historical APK. Do not automatically rebuild and
redeploy its experimental reconstruction path.** Future changes must start from
the restored original-engine behavior and preserve out-and-back scan quality.

The separate `quality` flavor now modernizes Java/storage around those exact
native binaries. It has a different application ID and independent storage.
The user reports the same quality but a stuttery experience, so smoothness is
still unaccepted. See [candidate evidence and profiling](quality-modernization.md).
