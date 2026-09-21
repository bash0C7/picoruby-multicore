#!/bin/sh
# Builds test/c/engine_test.c against the host port and the fake kernels, and runs it under
# ThreadSanitizer and AddressSanitizer. Usage: test/c/run.sh   (CC overrides the compiler)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=${TMPDIR:-/tmp}/multicore_engine_test.$$
mkdir -p "$out"
for san in thread address; do
  ${CC:-cc} -g -O1 -Wall -Wextra -fsanitize=$san -I"$root/include" \
    "$here/engine_test.c" "$root/ports/host/multicore.c" "$root/test/support/fake_kernels.c" \
    -lpthread -o "$out/engine_test_$san"
  echo "== $san"
  "$out/engine_test_$san"
done
rm -rf "$out"
