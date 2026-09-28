#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd "$(dirname "$0")/.." && pwd)
qt_sdk_dir=${TEEDSP_QT_SDK:-$HOME/.cache/teedsp-wasm/Qt/6.10.1}
emsdk_dir=${TEEDSP_EMSDK:-$HOME/.cache/teedsp-wasm/emsdk}
build_dir=${TEEDSP_WEB_BUILD:-$HOME/.cache/teedsp-wasm/build}
source "$emsdk_dir/emsdk_env.sh" >/dev/null 2>&1
cmake -S "$project_dir/webqt" -B "$build_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$qt_sdk_dir/wasm_singlethread/lib/cmake/Qt6/qt.toolchain.cmake" \
  -DQT_HOST_PATH="$qt_sdk_dir/gcc_64" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" -j4
mkdir -p "$project_dir/linux/web/qt"
cp "$build_dir/teedsp-editor.js" "$build_dir/teedsp-editor.wasm" "$build_dir/qtloader.js" "$project_dir/linux/web/qt/"
cp "$project_dir/webqt/index.html" "$project_dir/linux/web/qt/index.html"
