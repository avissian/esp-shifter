#!/bin/bash
#
# Build and run the host-side protocol and firmware regression tests.
#
# The tests are plain C++11 with no ESP SDK or third-party dependencies, so
# they compile and run on any host toolchain. The test binary is placed in
# build/tests/ (build/ is git-ignored).
#
# The compiler can be overridden via $CXX (default: c++), e.g.:
#   CXX=g++ scripts/test.sh

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${repo_root}/build/tests"
out_bin="${out_dir}/test_obc_protocol"
cxx="${CXX:-c++}"

mkdir -p "${out_dir}"

echo "Building unit tests with ${cxx} ..."
"${cxx}" -std=c++11 -Wall -Wextra -Werror -pedantic \
  -o "${out_bin}" \
  "${repo_root}/tests/test_obc_protocol.cpp"

echo "Running unit tests (${out_bin}) ..."
"${out_bin}"

firmware_bin="${out_dir}/test_firmware"
echo "Building firmware control-flow tests with ${cxx} ..."
"${cxx}" -std=c++11 -Wall -Wextra -Werror -pedantic \
  -I "${repo_root}/tests/fakes" \
  -o "${firmware_bin}" \
  "${repo_root}/tests/test_firmware.cpp"

echo "Running firmware control-flow tests (${firmware_bin}) ..."
"${firmware_bin}"
