#!/bin/sh
# Runs on the target tree before it's packed into the initramfs.
set -e

# Command line tools that came along with the libraries scsynth needs
rm -f "${TARGET_DIR}"/usr/bin/aserver "${TARGET_DIR}"/usr/bin/flac \
	"${TARGET_DIR}"/usr/bin/metaflac "${TARGET_DIR}"/usr/bin/fftw*-wisdom*
