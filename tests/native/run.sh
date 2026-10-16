#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${ARDUINOJSON_DIR:?Set ARDUINOJSON_DIR to an ArduinoJson 7 checkout}"
binary=$(mktemp)
trap 'rm -f "$binary"' EXIT
"${CXX:-g++}" -std=c++17 -g -fsanitize=address -fno-omit-frame-pointer \
  -DARDUINOJSON_DEFAULT_NESTING_LIMIT=64 -Wno-deprecated-declarations -Wno-address-of-packed-member \
  -Itests/native/stubs -I"$ARDUINOJSON_DIR/src" -Isrc \
  src/vm.cpp tests/native/vm_regression.cpp -o "$binary"
"$binary" behaviors/*.json examples/store-only/*.json
