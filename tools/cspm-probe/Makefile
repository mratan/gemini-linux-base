# Build against the tree gemini-kbuild.sh builds, with the same toolchain:
#   export PATH=$REPO/03-tools/gcc-13.3.0-nolibc/aarch64-linux/bin:$PATH
#   ARCH=arm64 CROSS_COMPILE=aarch64-linux- \
#     make -C $REPO/07-kernel/build-local/src M=$PWD modules
obj-m := gemini-cspm-probe.o
