#!/bin/sh
# apply-patches.sh -- apply the capture-kit instrumentation series to a
# fresh clone of the gemian native tree.
#
# Usage:
#   git clone --branch native https://github.com/gemian/gemini-linux-kernel-3.18 <src>
#   capture-kit/apply-patches.sh <src>
#   capture-kit/build.sh --src <src> --out <out>
set -eu

SRC=${1:?usage: apply-patches.sh <gemian-3.18-src-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)

cd "$SRC"
# refuse silently-wrong trees
grep -q "^SUBLEVEL = 41" Makefile || { echo "FATAL: not a 3.18.41 tree (wrong branch? use 'native')"; exit 1; }
[ -e arch/arm64/configs/gemini_modular_defconfig ] || { echo "FATAL: gemini_modular_defconfig missing (wrong tree)"; exit 1; }

git am "$HERE"/patches/*.patch
echo "capture-kit patches applied:"
git log --oneline -3
