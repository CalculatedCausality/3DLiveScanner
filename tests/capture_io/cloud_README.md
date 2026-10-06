# Live cloud ownership regression

Run `python3 tests/capture_io/cloud_run.py` from the repository root. Requires a
host C++11 compiler with AddressSanitizer and UndefinedBehaviorSanitizer.

The runner extracts the current production `Retango::PCL`, `TangoScan::Update`,
`TangoScan::DiscardAdded`, `ProcessReconstruction`, and `Dataset::WritePointCloud`
definitions, plus the public update declaration. It uses the real Tango SDK
header and geometry validators. SDK operations, camera/Retango preparation,
recovery, and non-cloud persistence are labeled instrumented boundaries.

Checks cover:

- One cloud construction/allocation and one destruction per admitted worker
  frame, including SDK/extraction/persistence failures; failed allocation also
  cleans the partially initialized cloud once.
- The SDK and real cloud writer use the same independently owned point storage.
  Changing the source Retango vector at the SDK boundary cannot change the file.
- Real temporary `.pcl` bytes equal the original count and float4 payload,
  including confidence, ordering, precision and signed zero.
- Cloud and YUV cleanup before binder release, cloud cleanup before failure
  recovery/commit, commit before merge, and retained committed counters on failure.
- SDK INVALID vs recovery-requiring failures, empty/invalid cloud and geometry,
  missing/empty SDK indices, partial extraction cleanup, every persistence gate,
  and open/header/payload/close failures in the real cloud writer.
- Invalid frame/recovery/pause paths allocate no cloud; direct borrowed-cloud
  validation never destroys the caller's storage.

This is a host ownership/serialization regression, not device SDK execution or
a throughput measurement. Generated sources, binaries and files are temporary.
