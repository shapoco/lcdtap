#!/bin/bash

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASE_NAME="lcdtap_v$(date +%Y%m%d%H%M)"
RELEASE_DIR="${SCRIPT_DIR}/release/${RELEASE_NAME}"

# Build all example programs
for example_dir in "${SCRIPT_DIR}/example"/*/; do
    echo "=== Building ${example_dir} ==="
    (cd "${example_dir}" && bash build.sh)
done

# Create release directory
mkdir -p "${RELEASE_DIR}"

# Copy top-level README
cp "${SCRIPT_DIR}/README.md" "${RELEASE_DIR}/"
cp -r "${SCRIPT_DIR}/image" "${RELEASE_DIR}/"

# Copy per-example artifacts
for example_dir in "${SCRIPT_DIR}/example"/*/; do
    example_name="$(basename "${example_dir}")"
    dest="${RELEASE_DIR}/${example_name}"
    mkdir -p "${dest}"

    cp "${example_dir}/README.md" "${dest}/"
    [ -d "${example_dir}/image" ] && cp -r "${example_dir}/image" "${dest}/"
    cp "${example_dir}"/build/*.uf2 "${dest}/"
done

# Create zip archive
(cd "${SCRIPT_DIR}/release" && zip -r "${RELEASE_NAME}.zip" "${RELEASE_NAME}")

echo "=== Release created: release/${RELEASE_NAME}.zip ==="
