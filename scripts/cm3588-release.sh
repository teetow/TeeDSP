#!/usr/bin/env bash
# Run on CM3588 from the deployment checkout. The caller supplies a tested,
# committed source tree and its matching generated browser bundle.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
state_file=linux/data/deploy-state

check_tag() {
    [[ "$1" =~ ^[a-z0-9][a-z0-9._-]*$ ]] || { echo "Invalid image tag: $1" >&2; exit 2; }
}

wait_for_release() {
    local tag=$1 image
    for ((attempt=0; attempt<45; ++attempt)); do
        image=$(docker inspect --format '{{.Config.Image}}' teedsp 2>/dev/null || true)
        if [[ "$image" == "teedsp:$tag" ]] && curl -fsS --max-time 3 \
            http://127.0.0.1:8790/api/health >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
    done
    echo "TeeDSP did not become healthy with image teedsp:$tag" >&2
    return 1
}

start_release() {
    local tag=$1
    check_tag "$tag"
    TEEDSP_IMAGE_TAG="$tag" docker compose up -d --no-build --force-recreate teedsp || return 1
    wait_for_release "$tag"
}

write_state() {
    local active=$1 previous=$2
    mkdir -p linux/data
    printf '%s\n%s\n' "$active" "$previous" > "${state_file}.tmp"
    mv "${state_file}.tmp" "$state_file"
}

case "${1:-}" in
    activate)
        [[ $# == 2 ]] || { echo "Usage: $0 activate RELEASE_TAG" >&2; exit 2; }
        new_tag=$2
        check_tag "$new_tag"
        [[ -f linux/web/qt/teedsp-editor.wasm ]] || { echo "Browser bundle missing" >&2; exit 1; }
        revision=$(git rev-parse HEAD)
        [[ -z "$(git status --porcelain)" ]] || { echo "Deployment checkout is dirty" >&2; exit 1; }
        bundle_hash=$(sha256sum linux/web/qt/index.html linux/web/qt/qtloader.js \
            linux/web/qt/teedsp-editor.js linux/web/qt/teedsp-editor.wasm | sha256sum | cut -c1-12)
        [[ "$new_tag" == "${revision:0:12}-$bundle_hash" ]] || {
            echo "Release tag does not match the source commit and browser bundle" >&2
            exit 1
        }

        # Keep the live image for rollback. The first release may be an old
        # Compose-generated tag, so give that image a durable TeeDSP tag.
        old_image=$(docker inspect --format '{{.Image}}' teedsp)
        old_ref=$(docker inspect --format '{{.Config.Image}}' teedsp)
        if [[ "$old_ref" == "teedsp:$new_tag" ]]; then
            wait_for_release "$new_tag"
            echo "Already active: teedsp:$new_tag"
            exit 0
        fi
        if [[ "$old_ref" == teedsp:* ]] && docker image inspect "$old_ref" >/dev/null 2>&1; then
            old_tag=${old_ref#teedsp:}
        else
            old_tag="rollback-$(date -u +%Y%m%d%H%M%S)"
            docker tag "$old_image" "teedsp:$old_tag"
        fi

        echo "Building teedsp:$new_tag from $revision on CM3588"
        TEEDSP_IMAGE_TAG="$new_tag" TEEDSP_REVISION="$revision" docker compose build teedsp
        if ! start_release "$new_tag"; then
            echo "Activation failed; restoring teedsp:$old_tag" >&2
            start_release "$old_tag"
            exit 1
        fi
        write_state "$new_tag" "$old_tag"
        echo "Active: teedsp:$new_tag; rollback: teedsp:$old_tag"
        ;;
    rollback)
        [[ $# == 1 ]] || { echo "Usage: $0 rollback" >&2; exit 2; }
        [[ -f "$state_file" ]] || { echo "No previous release recorded" >&2; exit 1; }
        active=$(sed -n '1p' "$state_file")
        previous=$(sed -n '2p' "$state_file")
        check_tag "$active"
        check_tag "$previous"
        [[ "$(docker inspect --format '{{.Config.Image}}' teedsp)" == "teedsp:$active" ]] || {
            echo "Active image differs from recorded release; refusing rollback" >&2
            exit 1
        }
        docker image inspect "teedsp:$previous" >/dev/null
        start_release "$previous"
        write_state "$previous" "$active"
        echo "Rolled back to teedsp:$previous"
        ;;
    *)
        echo "Usage: $0 activate RELEASE_TAG | rollback" >&2
        exit 2
        ;;
esac
