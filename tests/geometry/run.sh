#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
"${CXX:-c++}" -std=c++11 -Wall -Wextra -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/common" -I"$root/third_party/glm" \
  -I"$root/third_party/tango_3d_reconstruction/include" \
  "$root/tests/geometry/validation_test.cc" -o "$output/validation_test"
"$output/validation_test"
"${CXX:-c++}" -std=c++11 -Wall -Wextra \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I"$root/tests/geometry/include" -I"$root/common" \
  -I"$root/third_party/glm" -I"$root/third_party/delaunay" \
  -I"$root/third_party/tango_3d_reconstruction/include" \
  "$root/tests/geometry/retango_recovery_test.cc" "$root/common/tango/retango.cc" \
  -o "$output/retango_recovery_test"
"$output/retango_recovery_test"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/common" -I"$root/third_party/glm" -I"$root/arcore/include" \
  "$root/tests/geometry/arcore_recovery_test.cc" -o "$output/arcore_recovery_test"
"$output/arcore_recovery_test"
