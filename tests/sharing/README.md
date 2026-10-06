# Direct model sharing

## Viewer behavior

The viewer's Share menu starts with **Share model file (OBJ + assets ZIP)** or
**Share model file (PLY)**. PLY has a visible share button too. Local sharing opens
the Android share sheet with the actual model, without an upload or account.
Screenshot/video remain separately named actions for OBJ viewers. The browser
opens the Android share sheet directly for OBJ and PLY models. Sketchfab OAuth,
uploads, and the AnyConv web converter have been retired.

The packaging dialog is modal and dismissed on completion or failure. A missing
asset, unsupported path/option, unavailable receiver, or cache-write error shows
an error dialog. A destroyed Activity never launches the chooser. Main's existing
pause lifecycle is bypassed only when a share target/chooser actually launches.

## Browser handoff API

Capture the selection **on the UI thread**, before any asynchronous work:

```java
import com.lvonasek.arcore3dscanner.sharing.ModelSharing;

final File selected = new File(getPath(), selectedKey);
ModelSharing.share(activity, selected);
```

`ModelSharing.share(Activity, File)` returns `true` if preparation started, `false`
if another operation is active or the Activity is finishing/destroyed. It owns the
progress/error dialogs and chooser; callers do not need to call `showProgress()`
or restore browser progress afterward. A busy request shows a message and is not
queued. The global single-operation gate covers both viewer and browser model sharing.

Additional APIs:

* `share(Activity, File, Runnable onLaunched)` — UI-thread hook after successful
  `startActivity`, before `onPause`; Main uses it to set its existing `mIgnoreSaving`.
  It is not called on packaging/launch failure or when the host is destroyed.
* `ModelPackage.prepare(File selected, File appCacheDir)` — blocking, Android-free
  packager for tests or other controlled workers; returns `file` and `mimeType`.
  Direct callers must provide their own concurrency/UI handling.
* `createChooser(Context, Result)` / `createSendIntent(Uri, String mimeType, String name)`
  — Android intent boundaries; only `content:` URIs are accepted by the latter.

Pass an exact OBJ/PLY file, or an app model folder whose name ends in `.obj`/`.ply`
and which contains exactly one corresponding model at its top level. Multiple
models are rejected: select the exact file instead. Never pass the library or
Downloads as a fallback. The browser uses this helper for OBJ and PLY shares.
It captures the selected
path before background work; no worker reads the browser's current folder.

**Dataset folders require export to OBJ or PLY first.** This API explicitly rejects
`.dataset` selections and explains that raw dataset sharing is not supported.
Do not label a dataset ZIP as an exported model.
The browser separately offers export-to-model and an explicitly labelled raw
dataset ZIP option. That legacy archive path uses the failure-safe compressor
and grants URI read permission; it is not passed off as an OBJ/PLY model.

## Packaging and URI contract

* OBJ ZIPs contain the selected OBJ, its `mtllib` files, and the textures/spectral
  assets referenced by those MTLs. Original bytes and relative ZIP paths are
  retained, including nested directories. Standard texture options and quoted
  filenames are supported. Unknown texture options/external OBJ dependency forms
  are rejected instead of silently producing an incomplete package.
* No recursive folder ZIP, arbitrary sibling scans, GPS data, or thumbnails are
  included merely because they share a directory.
* Canonical path containment rejects outside-root dependencies, symlinks and
  nonportable absolute/Windows paths. In-root `../` references remain portable.
  Missing required dependencies and source size/mtime changes fail visibly.
* PLY is copied byte-for-byte as `.ply` with `application/octet-stream` for broad
  receiver compatibility; OBJ bundles use `application/zip`.
* Bounds: one operation, no queue; 4096 input files, 2 GiB total input, 64 KiB
  metadata lines, and a checked 10-minute deadline. Packaging is streamed.
* Outputs use `cacheDir/model-shares/<UUID>/<model>.zip` (or `.ply`). Successful
  outputs are never rewritten or deleted by later shares. Android may reclaim
  cache storage; long-term retention is the receiving app's responsibility.
  Only this operation's incomplete output is removed on failure. Source scans
  are never written or deleted.
* The FileProvider adds only the `model-shares/` cache path. Both ACTION_SEND and
  its chooser carry `FLAG_GRANT_READ_URI_PERMISSION` and URI ClipData; no write
  permission is granted.

## Checks

Plain-Java package/content/scoping regressions (JDK 17):

```sh
python3 tests/sharing/run.py
```

Android 33 boundary tests use real Intent, ClipData, and AndroidX FileProvider
through Robolectric, plus shadows for Activity launching/UI lifecycle. They check
the actual readable provider URI, stream extra, MIME type, read grants on both
intents, single-operation gating, progress recovery, error surfacing, and destroyed
hosts. An injected worker-side `IllegalStateException` also verifies local sharing:
no launch callback, visible/enabled viewer, dismissed progress,
shown error, and an immediately available retry. The test manifest is isolated
from scanner/native startup and uses the
production provider-path XML and sharing resources/classes.

```sh
ANDROID_HOME=/path/to/android-sdk \
./scanner/gradlew \
  -p tests/sharing/android \
  --project-cache-dir /tmp/opencode/scanner-sharing-gradle-cache \
  testDebugUnitTest lintDebug --console=plain
```

Outputs go to `/tmp/opencode/scanner-sharing-android-build` (override with
`SHARING_TEST_BUILD_DIR`). Lint checks API compatibility against minSdk 24.
The plain-Java runner uses a temporary output directory and accepts `JAVA_HOME`,
falling back to system Java 17 and its compiler module when `javac` is absent. These checks do not
substitute for receiving/saving the model in another app on a physical device.

Android tests/lint require an installed SDK (platform 33). A full scanner build
also requires platform 35, the configured NDK, and flavor-specific native inputs.
