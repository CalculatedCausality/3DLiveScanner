#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
output="$(mktemp -d /tmp/opencode/alignment.XXXXXX)"
trap 'rm -rf "$output"' EXIT
"${CXX:-g++}" -std=c++11 -O1 -g -Wall -Wextra \
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -include "$root/tests/performance/native/include/host.h" \
  -I"$root/tests/geometry/include" -I"$root/common" -I"$root/third_party/glm" \
  "$root/tests/alignment/optimizer_test.cc" "$root/tests/alignment/image_stubs.cc" \
  "$root/common/postproc/optimizer.cc" "$root/common/data/file3d.cc" "$root/common/data/mesh.cc" \
  -o "$output/optimizer_test"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$output/optimizer_test" "$output"
