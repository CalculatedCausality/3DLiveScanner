# Scanner runtime variants and 16 KiB builds

The scanner uses **AGP 8.8.2, Gradle 8.10.2, JDK 17 and NDK r28c
(28.2.13676358)**. Compile SDK is 35; target SDK remains 33, minimum SDK 24,
and the packaged ABI remains arm64-v8a. The Android application builds from
`scanner/`; its runtime flavors share this toolchain.

## Variants

| Flavor | AR provider | Reconstruction | Retired VR |
| --- | --- | --- | --- |
| `quality` (default) | ARCore **1.31.0**, Java SDK matched to archived native SDK | All nine native libraries extracted byte-for-byte from the user-validated historical APK; JNI compatibility bridge only is rebuilt | Java features unavailable; historical native libraries retained for identity preservation |
| `modern` | Official ARCore **1.56.0**, matching Java/native/C header | `reconstruction/core.cc` and `texturing.cc`, statically linked into `lib3dscanner.so` | Unavailable; button hidden, activities and GVR SDK omitted |
| `legacy` | ARCore **1.31.0** and Huawei AR Engine 3.5 | Historical Tango3DR shared library | Historical GVR/Cardboard/Daydream |

`modernDebug` / `modernRelease` are the 16 KiB migration variants. The legacy
flavor retains unmodifiable Tango/Huawei binaries and is **not a 16 KiB-supported
variant**. The original-binary `quality` flavor also makes **no 16 KiB kernel
compatibility claim**. Retained legacy providers and VR require separate compatible-device
validation. Modern and legacy keep the existing application ID and version metadata;
installing one replaces the other when signatures match.

The default `quality` flavor instead uses `com.lvonasek.arcore3dscanner.quality`,
label **3D Scanner Quality Test**, with its own private capture and library
directories. It leaves the restored original app installed. See
[original-engine modernization and pacing diagnostics](quality-modernization.md).
Real-device quality failed with the experimental modern engine; do not deploy
modern or rebuilt legacy over the restored baseline as a routine build step.

### Feature tradeoffs

Huawei AR Engine and Tango reconstruction were separate dependencies. On the
Pixel, camera tracking/depth already came from Google ARCore; removing Huawei
does not remove the provider used by that phone. Modern remains ARCore-only, so
Huawei devices without a supported Google ARCore route lose their vendor path.

| Capability | Modern build | Legacy build |
| --- | --- | --- |
| Google camera tracking/depth and augmented-face path | ARCore 1.56.0 | ARCore 1.31.0 |
| Huawei world tracking, supported ToF/depth, Huawei face/torch integration | Not packaged | Retained Huawei provider |
| Reconstruction/texturing | Owned bounded TSDF and CPU atlas implementation; performance/quality parity not established | Historical Tango3DR |
| Tango texturing mesh-simplification factor | Not implemented; factor 1 preserves geometry | Historical option retained |
| GVR/Cardboard/Daydream viewer | Not packaged | Retained retired VR integration |
| Existing mesh/dataset readers | Retained, with synthetic dataset/codec integration checks | Retained |

The owned texturer has explicit mesh/atlas budgets, and the reconstruction has
chunk/work limits. Large-scene behavior and real exported quality need comparison,
not an assumption of equivalence from matching API signatures. Google augmented
faces are still present; removing Huawei face support does not remove all face
scanning. Huawei torch support was not the Pixel's ARCore torch implementation.

The legacy flavor retains those old paths for comparison; it is not a 16 KiB
compatibility workaround. Arbitrary historical recordings and Huawei hardware
still need their own device validation.

### Isolation

- Quality extraction verifies the archived APK SHA-256 and the matching ARCore
  JNI SDK before copying all nine native libraries. Only `scanner_quality_bridge`
  is compiled (`SCANNER_QUALITY=1`); current experimental capture/reconstruction
  sources are not compiled into this flavor. Audit with `tools/check_quality_apk.py`.
- Each flavor resolves its own ARCore AAR. Only the C library needed by
  ndk-build is extracted to `app/build/arcore/<flavor>/jni`; Java and JNI SDK
  packaging comes from the same flavor's AAR. No extraction overwrites the
  shared `arcore/jni` tree or the other flavor's artifacts.
- Modern native builds define `SCANNER_MODERN=1`, omit `arengine.cc`, and do not
  import/link AREngine or the Tango prebuilt. `service`, `camera` and `platform`
  compile out all Huawei API references. A stale Huawei mode request falls back
  to ARCore.
- `SCANNER_MODERN` is the canonical numeric C/C++ feature flag: **1 for modern,
  0 for legacy**. Use `#if SCANNER_MODERN`, not `#ifdef SCANNER_MODERN`. It is
  passed to all parent scanner sources and the modern static reconstruction
  module; non-scanner provider consumers default to 0.
- The new static `scanner_reconstruction` module exports the existing Tango3DR
  C API header. Its File3d/Image references resolve from the parent scanner
  shared object. `common/tango` remains the scanner's integration layer, not a
  modern dependency on the retired Tango shared library.
- Static reconstruction declares dependencies on the existing TurboJPEG and
  libpng modules, inheriting their exported headers and libpng's `-lz` link
  requirement. The bundled libpng configuration enables simplified reads and
  stdio, and C++ exceptions remain enabled in reconstruction and the parent.
- Flavor-specific `ARProviderFeatures` implementations ensure modern Java
  contains no Huawei SDK dependency, including on devices without Play Store.
  Legacy probe cleanup semantics are preserved. GVR Java subclasses, SDK,
  activities and layout, Huawei metadata and Huawei deep link are legacy-only.
- The original ARCore C header remains available to legacy consumers. The
  official v1.56.0 header lives in `arcore/include-modern` with provenance/hash.

## Commands

Run in `scanner/` with a local SDK configured by `local.properties` or the Android
SDK environment variables, and `JAVA_HOME` pointing to JDK 17:

```sh
# Default, isolated original-engine comparison.
./gradlew :app:assembleQualityDebug --max-workers=2

# Scoped checks: no full native build or APK assembly.
./gradlew :app:checkModernDebugAarMetadata :app:processModernDebugResources \
  :app:compileModernDebugJavaWithJavac :app:compileLegacyDebugJavaWithJavac \
  :app:extractModernArcoreNatives :app:extractLegacyArcoreNatives --max-workers=2

# Native configuration/model generation only (no native compilation).
./gradlew :app:generateJsonModelModernDebug :app:generateJsonModelLegacyDebug \
  --max-workers=2

# Integration builds (run one at a time).
./gradlew :app:assembleModernDebug --max-workers=2
./gradlew :app:assembleModernRelease --max-workers=2
./gradlew :app:assembleLegacyDebug --max-workers=2
```

No-argument `./gradlew` defaults to `:app:assembleQualityDebug`. Android Studio's
default flavor is quality. `assembleDebug` builds all three flavors; prefer the
explicit quality task for the current Pixel comparison.

Quality debug output: `app/build/outputs/apk/quality/debug/app-quality-debug.apk`.
Modern debug output: `app/build/outputs/apk/modern/debug/app-modern-debug.apk`.
Release signing retains the existing project's configuration.

The migration's temporary `/tmp/opencode/pixelshare-sdk` and
`/tmp/opencode/android-sdk` paths are no longer available. Full builds require
an available SDK with the pinned NDK and build tools, plus a complete JDK 17:

```sh
sdkmanager --sdk_root="$ANDROID_HOME" 'ndk;28.2.13676358' \
  'platforms;android-35' 'build-tools;35.0.0'
```

The current system JDK has the runtime and compiler module, but no `bin/javac`
or JNI headers. Gradle 8.10.2 rejects it for Java compile tasks with
`required capabilities: [JAVA_COMPILER]`; compiler-module host checks alone
do not establish full Android build availability.

The WSL Windows mount briefly failed during migration, and its project-local
Gradle execution-history cache subsequently failed to read. In this environment,
add `--project-cache-dir /tmp/opencode/scanner16k-gradle-cache` to the commands
above to use a local-filesystem cache without deleting existing
project state.

## 16 KiB verification boundary

NDK r28c supplies a 16 KiB-capable libc++ runtime. Scanner linking additionally
sets both `max-page-size=16384` and `common-page-size=16384`, with RELRO/NOW
enabled. AGP's uncompressed JNI packaging provides 16 KiB APK ZIP alignment.

Final integration must audit **every packaged native library**, transitive
`DT_NEEDED` entries, ELF LOAD congruence/alignment, RELRO protection boundaries,
and APK ZIP alignment, then run the modern app on the 16 KiB device. The build
settings alone do not establish that final application behavior is correct.
For RELRO, check that page rounding never protects live writable bytes outside
RELRO; end-address divisibility alone is too strict for official ARCore/r28c
libraries that place safe padding before the next writable segment. No binary
header stripping, RELRO disabling, or page-size compatibility suppression is
used.

Provider probe regression tests (from repository root):

```sh
python3 common/ar/tests/test_compatibility_lifecycle.py
```

These test legacy session cleanup and modern execution without any Huawei
classes present. They use the JDK compiler module if the `javac` launcher is
absent; they do not validate provider behavior on a physical device.

### Migration verification (2026-09-30)

- Both debug flavors passed Gradle AAR metadata, manifests/resources, Java
  compilation and separate ARCore extraction using the historical local JDK/SDK/cache.
- Both native JSON models generated successfully, with each ARCore extraction
  running before its flavor's native configuration task.
- Provider sources passed NDK r28c ARM64 syntax checks in modern and legacy
  modes. Modern's check had no Huawei include directory.
- Host tests passed modern no-Huawei isolation and 190 legacy probe lifecycle
  assertions.
- Resolved runtime artifacts selected ARCore 1.56.0 for modern and 1.31.0 for
  legacy. Modern had no Huawei/GVR artifact; its AAR native binaries were only
  ARCore C/JNI. Its merged manifest and compiled app classes omitted VR.
- Modern ndk-build dry-run selected the static reconstruction module and
  16 KiB linker flags, with no Tango/Huawei/GVR native inputs.
- After standardizing on `SCANNER_MODERN`, the dry-run also verified that both
  reconstruction translation units inherit the numeric modern flag, exceptions
  and codec include paths. The completed core/texturing sources passed NDK r28c
  ARM64 syntax checks with those dependencies.

This verification did not run native compilation, APK assembly, release
shrinking/signing, final packaged-library auditing, or device checks. Those are
the integration stage, including validating the replacement reconstruction and
texturing implementations.
