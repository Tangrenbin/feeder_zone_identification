#!/usr/bin/env bash

python3 ./extract_period_cmp_records.py ./DATA0000.TXT -o ./bin/period_cmp_records.txt 

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="${SCRIPT_DIR}"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${TEST_ROOT}/bin"
SOURCE_FILE="${TEST_ROOT}/../app_area_frequency_calc.c"
OUTPUT_BIN="${BUILD_DIR}/app_area_frequency_calc_single"
RUN_BIN="/tmp/app_area_frequency_calc_single"
EXTRACT_SCRIPT="${TEST_ROOT}/extract_period_cmp_records.py"
INPUT_LOG="${TEST_ROOT}/DATA0000.TXT"
EXTRACT_OUTPUT="${BUILD_DIR}/period_cmp_records.txt"

mkdir -p "${BUILD_DIR}"

gcc -Wall -Werror -std=c99 -include stdio.h \
    -I "${REPO_ROOT}/SDK/src/include" \
    -I "${REPO_ROOT}/" \
    "${SOURCE_FILE}" \
    -o "${OUTPUT_BIN}" \
    -DTEST_CODE \
    -DFEEDER_ZONE_IDENTIFICATION


# Run from /tmp to avoid ELF execution issues on /mnt/c in WSL.
cp "${OUTPUT_BIN}" "${RUN_BIN}"
chmod +x "${RUN_BIN}"

echo "compiled: ${OUTPUT_BIN}"
echo "running: ${RUN_BIN}"
python3 "${EXTRACT_SCRIPT}" "${INPUT_LOG}" -o "${EXTRACT_OUTPUT}"
echo "extracted: ${EXTRACT_OUTPUT}"
"${RUN_BIN}" "${EXTRACT_OUTPUT}"
