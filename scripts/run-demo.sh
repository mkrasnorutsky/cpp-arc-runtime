#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
xcodebuild -quiet -project cpp-arc-runtime.xcodeproj -scheme cpparc-demo \
    -configuration Debug -destination 'platform=macOS' \
    -derivedDataPath build/Demo build
exec build/Demo/Build/Products/Debug/cpparc-demo
