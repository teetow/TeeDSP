#!/usr/bin/env bash
# Run from a clean WSL checkout. Build the browser locally, the audio image on
# ARM64, then smoke-test the switched service. Roll back on a failed smoke test.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
host=${TEEDSP_CM_HOST:-teetow@cm3588.lan}
remote_dir=${TEEDSP_CM_DIR:-/home/teetow/apps/teedsp}
url=${TEEDSP_URL:-http://teedsp.local}
[[ "$remote_dir" =~ ^/[a-zA-Z0-9_./-]+$ ]] || { echo "Unsafe remote path" >&2; exit 2; }

if [[ "${1:-deploy}" == rollback ]]; then
    [[ $# == 1 ]] || { echo "Usage: $0 [deploy|rollback]" >&2; exit 2; }
    ssh "$host" "cd '$remote_dir' && bash scripts/cm3588-release.sh rollback"
    exit
fi
[[ "${1:-deploy}" == deploy && $# -le 1 ]] || { echo "Usage: $0 [deploy|rollback]" >&2; exit 2; }
[[ -z "$(git status --porcelain)" ]] || { echo "Commit or discard local changes before deployment" >&2; exit 1; }

node_bin=${TEEDSP_NODE_BIN:-$(command -v node || true)}
if [[ -z "$node_bin" ]]; then
    for candidate in "$HOME"/.nvm/versions/node/*/bin/node; do
        [[ -x "$candidate" ]] && node_bin=$candidate
    done
fi
playwright_module=${PLAYWRIGHT_MODULE:-$HOME/apps/audacity-github-io/node_modules/playwright}
chromium_path=${CHROMIUM_PATH:-}
if [[ -z "$chromium_path" ]]; then
    for candidate in "$HOME"/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell; do
        [[ -x "$candidate" ]] && chromium_path=$candidate
    done
fi
[[ -x "$node_bin" && -d "$playwright_module" && -x "$chromium_path" ]] || {
    echo "Set TEEDSP_NODE_BIN, PLAYWRIGHT_MODULE and CHROMIUM_PATH before deploying" >&2
    exit 1
}

revision=$(git rev-parse HEAD)
bash webqt/build.sh
qt_sdk_dir=${TEEDSP_QT_SDK:-$HOME/.cache/teedsp-wasm/Qt/6.10.1}
cmake -S webqt/tests -B "$HOME/.cache/teedsp-wasm/tests" \
    -DCMAKE_PREFIX_PATH="$qt_sdk_dir/gcc_64"
cmake --build "$HOME/.cache/teedsp-wasm/tests" -j4
ctest --test-dir "$HOME/.cache/teedsp-wasm/tests" --output-on-failure

bundle_hash=$(sha256sum linux/web/qt/index.html linux/web/qt/qtloader.js \
    linux/web/qt/teedsp-editor.js linux/web/qt/teedsp-editor.wasm | sha256sum | cut -c1-12)
tag="${revision:0:12}-$bundle_hash"
echo "Prepared teedsp:$tag"

# NAS is this checkout's private origin. Push only after local build/tests pass;
# the CM3588 checkout can then fast-forward cleanly to the exact source commit.
git push origin HEAD:refs/heads/main
ssh "$host" "cd '$remote_dir' && test -z \"\$(git -c core.filemode=false status --porcelain)\" && git fetch origin main && git merge --ff-only origin/main && test \"\$(git rev-parse HEAD)\" = '$revision'"
rsync -rzt --checksum --chmod=F644,D755 linux/web/qt/ "$host:$remote_dir/linux/web/qt/"
ssh "$host" "cd '$remote_dir' && bash scripts/cm3588-release.sh activate '$tag'"

if ! PLAYWRIGHT_MODULE="$playwright_module" CHROMIUM_PATH="$chromium_path" \
    TEEDSP_URL="$url" "$node_bin" webqt/tests/browser.cjs; then
    echo "Browser smoke test failed; rolling back" >&2
    ssh "$host" "cd '$remote_dir' && bash scripts/cm3588-release.sh rollback"
    exit 1
fi
echo "Deployed teedsp:$tag ($revision). Roll back with: bash scripts/deploy-cm3588.sh rollback"
