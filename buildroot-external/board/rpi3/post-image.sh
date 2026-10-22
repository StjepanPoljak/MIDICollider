#!/bin/sh
# Assembles sdcard.img: the boot partition (firmware, kernel, initramfs) and
# the data partition, seeded from the repository's data/ directory.
set -e

BOARD_DIR="$(dirname "$0")"

rm -rf "${BINARIES_DIR}/data"
cp -r "${BOARD_DIR}/../../../data" "${BINARIES_DIR}/data"

support/scripts/genimage.sh -c "${BOARD_DIR}/genimage.cfg"
