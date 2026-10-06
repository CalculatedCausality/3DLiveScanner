# Geometry and interruption checks

Run `bash tests/geometry/run.sh` from the repository root with a host C++11
compiler and AddressSanitizer/UndefinedBehaviorSanitizer available.

The suite checks finite/invertible poses, depth-plane layout and confidence
pairing, projection/mesh boundaries, real Retango bad-frame to good-frame
acceptance, and ARCore session/configuration state helpers. Vendor declarations
come from the repository. The small GL header supplies host types only.

These are host checks. They do not run the device ARCore/Huawei SDK, validate
Tango's internal reconstruction volume, measure tracking accuracy, or prove
history replay behavior on the Pixel. Use the device-validation checklist for
those checks, including pause/background responsiveness during recovery.
