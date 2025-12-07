cmake_minimum_required(VERSION 3.10)
project(AMLDigitalFPV C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Allow passing toolchain/sysroot from environment or toolchain file
if(NOT DEFINED CMAKE_SYSROOT AND DEFINED ENV{SYSROOT_PREFIX})
  set(CMAKE_SYSROOT "$ENV{SYSROOT_PREFIX}")
endif()
# Ensure pkg-config looks inside the sysroot
if(DEFINED CMAKE_SYSROOT)
  set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
endif()

# Match old behavior: ignore unresolved symbols at link time
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--unresolved-symbols=ignore-all")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--unresolved-symbols=ignore-all")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(SPDLOG REQUIRED spdlog)
# gstapp .pc may be missing; fall back to manual hints
pkg_check_modules(GSTAPP QUIET gstapp-1.0)

# libamcodec usually doesn't have pkg-config; include path from sysroot
if(DEFINED CMAKE_SYSROOT)
  include_directories(${CMAKE_SYSROOT}/usr/include)
  link_directories(${CMAKE_SYSROOT}/usr/lib)
endif()

include_directories(
  ${GSTREAMER_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS}
  ${FMT_INCLUDE_DIRS}
  ${SPDLOG_INCLUDE_DIRS}
  ${GSTAPP_INCLUDE_DIRS}
  ${CMAKE_SYSROOT}/usr/include/amcodec
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

link_directories(
  ${GSTREAMER_LIBRARY_DIRS}
  ${GLIB_LIBRARY_DIRS}
  ${FMT_LIBRARY_DIRS}
  ${SPDLOG_LIBRARY_DIRS}
  ${GSTAPP_LIBRARY_DIRS}
)

add_definitions(-DSPDLOG_FMT_EXTERNAL)

set(SRC_CPP
  src/main.cpp
  src/gstrtpreceiver.cpp
  src/spdlog_wrapper.cpp
  src/dvr_recorder.cpp
)
set(SRC_C
  src/util.c
  src/aml.c
)
add_executable(AMLDigitalFPV ${SRC_CPP} ${SRC_C})

target_link_libraries(AMLDigitalFPV
  ${GSTREAMER_LIBRARIES}
  ${GLIB_LIBRARIES}
  fmt
  spdlog
  amcodec
  ${GSTAPP_LIBRARIES}
  gstapp-1.0
)

install(TARGETS AMLDigitalFPV DESTINATION bin)
