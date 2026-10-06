# Storage regression tests and integration handoff

Run from the repository root:

```sh
python3 tests/storage/run.py
python3 tests/storage/run.py --host-only
python3 tests/storage/run.py --baseline
```

The normal run compiles the **real** `IO.java` and `Exporter.java` against Android 33,
then compiles and executes them on JDK 17 with only `Log` and the storage-facing
`AbstractActivity` methods stubbed. Production sources use Java 8 and pre-API-24
`java.io`/ZIP APIs; no `java.nio.file` dependency was introduced into the app.
Test-only NIO creates isolated fixtures. Build products and fixtures live under
`/tmp/opencode` and are removed at the end. No Gradle, device, installation, or
repository build outputs are required.

Java discovery uses `tests/java_tools.py` (`JAVA_HOME` or PATH, including the
compiler-module fallback). Set `ANDROID_JAR` or `ANDROID_SDK_ROOT`/`ANDROID_HOME`
for the Android 33 type-check. `--host-only` explicitly omits that check but runs
all host assertions. Keep the host runtime on JDK 17: filesystem collision tests
use its deprecated, host-only
`SecurityManager` observation hook to provoke real `File.renameTo()` failures.
The hook is never part of Android production code. Its deprecation warning and
logged injected I/O exceptions are expected.

`--baseline` compiles **git HEAD**, not the dirty working tree, and runs the same
11 legacy-public-API cases. It is expected to exit nonzero. The normal suite also
runs 18 storage regressions and 8 checked-directory publication cases, for **37
cases**. The baseline is not a reconstruction of uncommitted edits.

## Fixed causes / contracts

- `IO.copy(File, File)` previously truncated destinations (including self-copy),
  swallowed errors, and explicitly re-closed try-with-resources streams. It now
  throws `IllegalStateException` on failure. `copyChecked` exposes `IOException`;
  `copyToFile(InputStream, File)` owns/closes the input, stages in the destination
  directory, and publishes after both streams close. Partial reads and close
  failures preserve prior destination bytes. One reusable 64 KiB transfer buffer
  handles zero-length provider reads without truncation or a busy loop.
- `IO.zip` previously truncated the destination before reading all sources. It
  now validates duplicate basenames/self-inclusion and stages output until ZIP
  finalization and close succeed. Its flat entry names and level-9 DEFLATE remain.
  `zipDirectory` additionally preserves nested relative resource paths and checks
  containment and rejects symlink resources/directory cycles using the publication inventory.
- `IO.unzip` previously used a raw string prefix check (allowing sibling-prefix
  escapes), leaked streams on exceptions, left partial files, overwrote existing
  files, and treated missing central directories/non-ZIP input as successful EOF.
  It now owns/closes the provider stream, spools into its own temporary archive,
  validates a central directory plus every member's size/CRC, rejects canonical
  duplicate paths/conflicts/escapes, and stages all entries before publishing.
  The destination must be **absent or empty**. Existing nonempty directories are
  refused, not merged. Canonical roots handle Android path aliases without
  hardcoded `/data/data` rewriting. On failure, an absent destination stays absent
  and a pre-existing empty destination stays empty.
- `Exporter.export` previously deleted old resources, ignored failed moves, and
  returned a success-looking path. It now builds the established scan-directory
  layout directly: `library/name.obj/name.obj` (or `.ply`) plus resources/GPS,
  publishes the completed directory, and returns the **inner model file**. It
  retains source files and refuses an existing scan name. Declared MTL/textures
  are required; the generated MTL preview PNG remains optional. Filename and
  canonical resource containment checks reject escaping references. Material
  parsing uses closing buffered readers instead of `Scanner`; nullable thumbnail
  lookup remains nullable, while destructive resource enumeration fails loudly.
- `compressModel` previously deleted/reused one shared ZIP and returned its path
  even after a ZIP error. Each successful request now owns a separate
  `temp/.storage-UUID/upload.scan.zip`; failure throws `IllegalStateException`.
  Model folders include nested resources. The legacy library-root fallback
  continues to archive only immediate files, excluding other scan directories.
- `makeStructure` previously deleted a fixed `temp` directory and moved shared
  textures/GPS out from under subsequent models. It now copies resources into a
  unique stage, checks model/publication moves, and attempts rollback on publish
  failure. A failed rollback **retains and logs the stage holding the only model
  copy**. Existing loose shared resources remain; there is no cleanup sweep.
  Enumeration uses `File.listFiles()` rather than the activity's helper, which
  also schedules background deletions as a side effect.

## Boundaries

These changes provide failure-safe publication under ordinary I/O errors, not
power-loss-atomic directory publication: checked-directory copies fsync each file
and validate inventory/size/SHA-256, but there is no directory fsync or recovery
journal. Legacy restructuring still has a two-rename window; interruption may
leave a recoverable `.storage-*` directory. No startup cleanup is added. Collision
tests cover rollback and preservation when rollback itself cannot finish.

Import spooling/staging consumes additional disk space and does not impose new
archive size/entry quotas. Source models should be quiescent during export/share;
this does not provide a coherent snapshot of concurrently mutating native files.
Resource parsing retains the app's single-material-library/simple texture-name
convention; it is not a general Wavefront option/multiple-library parser.
The default run checks Android compilation; `--host-only` does not. Device
filesystem/SAF/UI behavior and full application integration require device checks.
