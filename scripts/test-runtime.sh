#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
xcodebuild -project cpp-arc-runtime.xcodeproj -scheme cpp-arc-runtime \
    -configuration Debug -destination 'platform=macOS' \
    -derivedDataPath build/CppARC build
build/CppARC/Build/Products/Debug/cpp-arc-tests
