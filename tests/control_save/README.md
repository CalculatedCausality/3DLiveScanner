# Control and editor-save boundary regressions

Run `python3 tests/control_save/run.py` with a C++11 compiler and ASan/UBSan.

The runner extracts the current production control, recovery, textured-save,
name-swap and row-flip definitions. It compiles those definitions with labelled
SDK/image-write boundaries and checks nonblocking controls while the binder is
held, recovery/cancellation-state outcomes, and 11 success/failure/exception save cases.
Names, image orientation, buffer addresses and locks must be restored.

It does not run the full `OnDrawFrame()` method, device camera SDK, GL driver or
actual image encoders. Frame admission uses one desired-running snapshot in the
production GL loop so a concurrent start cannot switch from skipped capture to
dereferencing a missing image midway through that frame. Validate lifecycle and
pause/resume behavior on-device as well as running these host checks.
Production history replay checks the requested-running flag between SDK frames,
retains committed data on cancellation, and retries the volume on Resume; the
control fixture checks state classification. A second fixture executes the actual
replay definition with synthetic valid dataset inputs and an instrumented SDK
boundary: it covers early cancellation, stopping between updates, destruction of
the partial context, full replay after Resume and distinct history/SDK failures.
Neither fixture establishes device SDK cancellation latency.

Run `python3 tests/control_save/lifecycle_run.py` for Activity pause/resume and
background surface-recreation checks. It executes the production lifecycle hooks
and the camera-admission method with fake camera/GL boundaries. Background
draws must not reach camera processing, surface recreation must not resume the
camera, and pausing must not acquire the scan worker's binder. Foreground resume
permits camera draws but does not automatically restart scan integration. Offline
GL work is not disabled by the camera-admission gate.
This is not a complete GL frame or Android camera-policy simulation.
