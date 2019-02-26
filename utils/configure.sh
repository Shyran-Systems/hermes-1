#!/usr/bin/env bash

set -x
set -e

START_DIR="$PWD"
function finish () {
  cd "$START_DIR"
}
trap finish EXIT

# Optional environment variables to pass in:
#   DISTRIBUTE=1             Enable release build
#   ENABLE_ASAN=1            Enable address sanitizer
#   HERMESVM_OPCODE_STATS=1  Enable opcode stats profiling in interpreter
#                            We profile frequency of each opcode and
#                            time spent in each opcode.
#   BUILD_DIR=<..>           Directory to build
#   BUILD_TYPE=<...>         CMake build type. Defaults to Debug.
#   BUILD_32BIT=1            Build 32-bit binaries on 64-bit systems
#   LIBFUZZER_PATH=<..>      Path to libfuzzer
#   ICU_ROOT=<..>            Path to ICU library

# HERMES_WS_DIR is the root directory for LLVM checkout and build dirs.
[ -z "$HERMES_WS_DIR" ] && echo "HERMES_WS_DIR must be set" >&2 && exit 1
[ "${HERMES_WS_DIR:0:1}" != "/" ] && echo "HERMES_WS_DIR must be an absolute path" >&2 && exit 1

# Detect the Hermes source dir
cd "$(dirname "${BASH_SOURCE[0]}")/.."
HERMES_DIR="$PWD"
[ ! -e "$HERMES_DIR/utils/configure.sh" ] && echo "Could not detect source dir" >&2 && exit 1

cd "$HERMES_WS_DIR"

# You can substitute the 'Ninja' build system with "Xcode",
# "Visual Studio 10 Win64" or other build system to create project files for
# these editors.
if [[ `uname` == *"_NT-"* ]]; then
  BUILD_SYSTEM="${1-Visual Studio 15 2017 Win64}"
else
  BUILD_SYSTEM=${1-Ninja}
fi

BUILD_DIR=${2-}

BUILD_DIR_SUFFIX=""
if [ -n "$ENABLE_ASAN" ]; then
    BUILD_DIR_SUFFIX="${BUILD_DIR_SUFFIX}_asan";
fi
if [ -n "$DISTRIBUTE" ]; then
    BUILD_DIR_SUFFIX="${BUILD_DIR_SUFFIX}_release";
fi
if [ -n "$BUILD_32BIT" ]; then
    BUILD_DIR_SUFFIX="${BUILD_DIR_SUFFIX}_32";
fi

# Guess ICU_ROOT if ICU_ROOT is not specified and is on Linux
if [ -z "$ICU_ROOT" ] && [[ `uname` = Linux ]]
then
  guess_path=/mnt/gvfs/third-party2/icu/4e8f3e00e1c7d7315fd006903a9ff7f073dfc02b/53.1/gcc-4.8.1-glibc-2.17/c3f970a/
  [ -d "$guess_path" ] && ICU_ROOT=$guess_path
fi

# If env DISTRIBUTE is set, we will use release build.
if [ -n "$DISTRIBUTE" ]; then
  BUILD_TYPE=${BUILD_TYPE:-MinSizeRel}
else
  BUILD_TYPE=${BUILD_TYPE:-Debug}
fi

LLVM_BUILD_DIR=${LLVM_BUILD_DIR:-llvm_build$BUILD_DIR_SUFFIX}
BUILD_DIR=${BUILD_DIR:-build$BUILD_DIR_SUFFIX}

echo "Building hermes using $BUILD_SYSTEM into $BUILD_DIR/".

# Create the build directory.
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "Hermes Path: $HERMES_DIR"

FLAGS="-DLLVM_BUILD_DIR=$PWD/../$LLVM_BUILD_DIR -DLLVM_SRC_DIR=$PWD/../llvm -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
if [ -n "$BUILD_32BIT" ]; then
  FLAGS="$FLAGS -DLLVM_BUILD_32_BITS=On"
fi

if [ -z "$DISTRIBUTE" ]
then
  FLAGS="$FLAGS -DLLVM_ENABLE_ASSERTIONS=On"
fi

if [ -n "$LIBFUZZER_PATH" ]
then
  FLAGS="$FLAGS -DLIBFUZZER_PATH=$LIBFUZZER_PATH"
  ENABLE_ASAN=1
fi

if [ -n "$ENABLE_ASAN" ]
then
  FLAGS="$FLAGS -DLLVM_USE_SANITIZER=Address"
fi

if [ -n "$HERMESVM_OPCODE_STATS" ]
then
  FLAGS="$FLAGS -DHERMESVM_PROFILER_OPCODE=ON"
fi

if [ -n "$HERMESVM_PROFILER_BB" ]
then
  FLAGS="$FLAGS -DHERMESVM_PROFILER_BB=ON"
fi

if [ -n "$FBSOURCE_DIR" ]
then
  FLAGS="$FLAGS -DFBSOURCE_DIR=$FBSOURCE_DIR"
fi

if [ -n "$ICU_ROOT" ]
then
  echo "Using ICU_ROOT: $ICU_ROOT"
  FLAGS="$FLAGS -DICU_ROOT=$ICU_ROOT"
elif [ -n "$SANDCASTLE" ] && [[ "$(uname)" != "Darwin" ]]
then
  # If we're on sandcastle and not on OSX, we need an ICU path.
  echo "No ICU path provided on sandcastle."
  exit 1
fi

echo "cmake flags: $FLAGS"

cmake "$HERMES_DIR" -G "$BUILD_SYSTEM" $FLAGS
