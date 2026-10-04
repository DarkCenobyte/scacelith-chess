# Cross-compilation toolchain: Linux x86-64 host -> Linux aarch64 (Debian/Ubuntu multiarch: the
# arm64 development packages, e.g. libssl-dev:arm64 libx11-dev:arm64 libgl-dev:arm64, installed
# beside the host's). The tests run under qemu-user: qemu-aarch64 -L /usr/aarch64-linux-gnu.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(TOOLCHAIN_PREFIX aarch64-linux-gnu)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_LIBRARY_ARCHITECTURE ${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-L;/usr/${TOOLCHAIN_PREFIX})
# The host's x86-64 libraries are not candidates (FindOpenSSL would take them), nor its pkg-config files.
set(CMAKE_IGNORE_PATH /usr/lib/x86_64-linux-gnu)
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/${TOOLCHAIN_PREFIX}/pkgconfig:/usr/share/pkgconfig)
