#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${TMPDIR:-/tmp}/quiesco-host-tests"
power_binary="${TMPDIR:-/tmp}/quiesco-power-domain-tests"
storage_binary="${TMPDIR:-/tmp}/quiesco-storage-ble-tests"
flashdb_build_dir="${TMPDIR:-/tmp}/quiesco-flashdb-host-objects"

# Mbed TLS for HMAC-SHA-256 (src/platform/HmacSha256.cpp). The unit links the
# 2.x copy precompiled into the mbed core; the host uses 3.x, whose HMAC API is
# the same (4.x removed it). MBEDTLS_PREFIX overrides the Homebrew lookup.
mbedtls_prefix="${MBEDTLS_PREFIX:-$(brew --prefix mbedtls@3 2>/dev/null || true)}"
if [ ! -f "$mbedtls_prefix/include/mbedtls/md.h" ]; then
  echo "Mbed TLS 3.x not found: brew install mbedtls@3, or set MBEDTLS_PREFIX." >&2
  exit 1
fi

mkdir -p "$flashdb_build_dir"
flashdb_objects=()
for source in fdb.c fdb_kvdb.c fdb_tsdb.c fdb_utils.c fal.c fal_flash.c fal_partition.c; do
  object="$flashdb_build_dir/${source%.c}.o"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -I"$root/src" \
    -I"$root/src/third_party/flashdb" \
    -c "$root/src/third_party/flashdb/$source" -o "$object"
  flashdb_objects+=("$object")
done

"${CXX:-c++}" \
  -std=c++14 \
  -Wall -Wextra -Werror -pedantic \
  -I"$root/src" \
  "$root/tests/host/test_main.cpp" \
  "$root/src/services/Scheduler.cpp" \
  "$root/src/ui/ComfortEvaluation.cpp" \
  "$root/src/ui/UiModel.cpp" \
  "$root/src/dsp/AcousticMetrics.cpp" \
  "$root/src/services/SampleRecord.cpp" \
  -o "$binary"

"$binary"

"${CXX:-c++}" \
  -std=c++14 \
  -Wall -Wextra -Werror -pedantic \
  -DARDUINO_SEEED_XIAO_NRF52840_PLUS \
  -I"$root/tests/host/fakes" \
  -I"$root/src" \
  "$root/tests/host/test_power_domain.cpp" \
  "$root/src/board/PowerDomain.cpp" \
  -o "$power_binary"

"$power_binary"

# Flash-facing stores and the BLE wire codec run against the in-memory
# W25Q64Flash sim (no Arduino fakes needed: every linked TU is pure C++).
"${CXX:-c++}" \
  -std=c++14 \
  -Wall -Wextra -Werror -pedantic \
  -I"$root/tests/host/fakes" \
  -I"$root/src" \
  -I"$root/src/third_party/flashdb" \
  -isystem "$mbedtls_prefix/include" \
  -DQUIESCO_PROTOCOL_DOC="\"$root/src/protocol/PROTOCOL.md\"" \
  "$root/tests/host/test_storage_ble.cpp" \
  "$root/tests/host/fakes/W25Q64FlashSim.cpp" \
  "$root/src/drivers/FlashDbPort.cpp" \
  "$root/src/protocol/BleCodec.cpp" \
  "$root/src/platform/HmacSha256.cpp" \
  "$root/src/services/CalibrationPolicy.cpp" \
  "$root/src/services/ConfigRecord.cpp" \
  "$root/src/services/ConfigStore.cpp" \
  "$root/src/services/SampleLog.cpp" \
  "$root/src/services/SampleLogFlashDb.cpp" \
  "$root/src/services/SampleRecord.cpp" \
  "$root/src/services/UnitIdentity.cpp" \
  "$root/src/services/BondTable.cpp" \
  "${flashdb_objects[@]}" \
  -L"$mbedtls_prefix/lib" -lmbedcrypto \
  -o "$storage_binary"

"$storage_binary"

runtime_binary="${TMPDIR:-/tmp}/quiesco-runtime-tests"
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror -pedantic \
  -DARDUINO_SEEED_XIAO_NRF52840_PLUS \
  -I"$root/tests/host/fakes" -I"$root/src" -I"$root/src/third_party/flashdb" \
  -isystem "$mbedtls_prefix/include" \
  "$root/tests/host/test_runtime.cpp" \
  "$root/tests/host/fakes/W25Q64FlashSim.cpp" \
  "$root/src/App.cpp" "$root/src/board/PowerDomain.cpp" \
  "$root/src/drivers/BleConfig.cpp" "$root/src/drivers/Scd41Sensor.cpp" \
  "$root/src/drivers/FlashDbPort.cpp" "$root/src/protocol/BleCodec.cpp" \
  "$root/src/platform/HmacSha256.cpp" "$root/src/platform/MonotonicClock.cpp" \
  "$root/src/diagnostics/DebugLog.cpp" \
  "$root/src/services/CalibrationPolicy.cpp" "$root/src/services/ConfigRecord.cpp" \
  "$root/src/services/ConfigStore.cpp" "$root/src/services/SampleLog.cpp" \
  "$root/src/services/SampleLogFlashDb.cpp" "$root/src/services/SampleRecord.cpp" \
  "$root/src/services/UnitIdentity.cpp" "$root/src/services/BondTable.cpp" \
  "$root/src/services/Scheduler.cpp" "$root/src/ui/UiModel.cpp" \
  "$root/src/ui/ComfortEvaluation.cpp" "${flashdb_objects[@]}" \
  -L"$mbedtls_prefix/lib" -lmbedcrypto -o "$runtime_binary"
"$runtime_binary"

"${CXX:-c++}" \
  -std=c++14 \
  -Wall -Wextra -Werror -pedantic \
  -DARDUINO_SEEED_XIAO_NRF52840_PLUS \
  -I"$root/tests/host/fakes" \
  -I"$root/src" \
  -fsyntax-only \
  "$root/src/App.cpp" \
  "$root/src/board/PowerDomain.cpp" \
  "$root/src/diagnostics/DebugLog.cpp" \
  "$root/src/drivers/BleConfig.cpp" \
  "$root/src/platform/MonotonicClock.cpp" \
  "$root/src/services/SampleLog.cpp" \
  "$root/src/ui/Renderer.cpp"
