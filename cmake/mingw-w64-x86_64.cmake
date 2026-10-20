# Cross-compile toolchain: Linux host -> Windows x86_64 via MinGW-w64
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Override with -DMINGW_SYSROOT=... (or the MINGW_SYSROOT env var) for a
# toolchain installed somewhere other than /usr.
if(NOT MINGW_SYSROOT AND DEFINED ENV{MINGW_SYSROOT})
  set(MINGW_SYSROOT $ENV{MINGW_SYSROOT})
endif()
if(NOT MINGW_SYSROOT)
  set(MINGW_SYSROOT /usr/${TOOLCHAIN_PREFIX})
endif()
set(CMAKE_FIND_ROOT_PATH ${MINGW_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static-link the gcc/winpthread runtimes so the .exe is self-contained.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")
