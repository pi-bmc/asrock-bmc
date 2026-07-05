#!/usr/bin/env bash
#
# Yocto OCI cache helper for .github/workflows/build.yaml.
#
# Saves/restores the Yocto sstate-cache and downloads directories to/from GHCR
# as non-expiring OCI artifacts using oras. The "save" path is also invoked
# periodically *during* the build (see the checkpoint loop in build.yaml) so
# that build progress is preserved even when a run is cancelled before the
# final save step runs to completion.
#
# Usage:
#   yocto-cache.sh restore [sstate] [downloads]
#   yocto-cache.sh save    [sstate] [downloads]
#
# Required environment:
#   CACHE_IMAGE  - GHCR repo, e.g. ghcr.io/<owner>/<repo>/yocto-cache
#   SSTATE_DIR   - absolute path to the sstate-cache directory
#   DL_DIR       - absolute path to the downloads directory
#   RUNNER_TEMP  - scratch dir (provided by GitHub Actions)

set -uo pipefail

: "${CACHE_IMAGE:?CACHE_IMAGE must be set}"
: "${SSTATE_DIR:?SSTATE_DIR must be set}"
: "${DL_DIR:?DL_DIR must be set}"
TMP="${RUNNER_TEMP:-/tmp}"
MEDIA_TYPE="application/vnd.yocto.cache.layer.v1.tar+zstd"

tag_dir() {
  case "$1" in
    sstate|sstate-*)    printf '%s' "${SSTATE_DIR}" ;;
    downloads|downloads-*) printf '%s' "${DL_DIR}" ;;
    *) return 1 ;;
  esac
}

tag_archive() {
  case "$1" in
    sstate|sstate-*)    printf '%s' "sstate-cache.tar.zst" ;;
    downloads|downloads-*) printf '%s' "downloads.tar.zst" ;;
    *) return 1 ;;
  esac
}

restore_tag() {
  local tag="$1" dest archive
  dest="$(tag_dir "${tag}")" || { echo "unknown cache tag: ${tag}"; return 1; }
  archive="$(tag_archive "${tag}")"
  echo "::group::restore ${tag}"
  if oras pull "${CACHE_IMAGE}:${tag}" -o "${TMP}"; then
    if [ -f "${TMP}/${archive}" ]; then
      mkdir -p "${dest}"
      tar -I 'zstd -d -T0' -xf "${TMP}/${archive}" -C "$(dirname "${dest}")"
      rm -f "${TMP}/${archive}"
      echo "restored ${tag} ($(du -sh "${dest}" 2>/dev/null | cut -f1))"
    fi
  else
    echo "no '${tag}' cache in registry yet (first run?)"
  fi
  echo "::endgroup::"
}

save_tag() {
  local tag="$1" src archive staged final rc=0
  src="$(tag_dir "${tag}")" || { echo "unknown cache tag: ${tag}"; return 1; }
  archive="$(tag_archive "${tag}")"
  if [ ! -d "${src}" ]; then echo "skip ${tag}: ${src} missing"; return 0; fi
  echo "::group::save ${tag} ($(du -sh "${src}" 2>/dev/null | cut -f1))"

  staged="${TMP}/${archive}.staging"
  final="${TMP}/${archive}"
  # bitbake writes sstate files atomically, but new files may appear while we
  # archive a live cache; tolerate tar's "file changed/removed" status (rc=1).
  tar --warning=no-file-changed --ignore-failed-read -I 'zstd -3 -T0' \
      -cf "${staged}" -C "$(dirname "${src}")" "$(basename "${src}")" || rc=$?
  if [ "${rc}" -gt 1 ]; then
    echo "tar of ${tag} failed (rc=${rc}); skipping push this round"
    rm -f "${staged}"
    echo "::endgroup::"
    return 0
  fi
  # Atomic rename so oras always pushes a complete archive named ${archive}
  # (the basename becomes the layer title used by restore).
  mv -f "${staged}" "${final}"
  if oras push "${CACHE_IMAGE}:${tag}" \
        --annotation "org.opencontainers.image.source=https://github.com/${GITHUB_REPOSITORY:-}" \
        --annotation "org.opencontainers.image.description=Yocto ${tag} cache" \
        "${final}:${MEDIA_TYPE}"; then
    echo "pushed ${tag}"
  else
    echo "push of ${tag} failed; will retry on next checkpoint/final save"
  fi
  rm -f "${final}"
  echo "::endgroup::"
}

main() {
  local action="${1:-}"; shift || true
  local -a tags=("$@")
  [ "${#tags[@]}" -gt 0 ] || tags=(sstate downloads)
  case "${action}" in
    restore) for t in "${tags[@]}"; do restore_tag "${t}"; done ;;
    save)    for t in "${tags[@]}"; do save_tag "${t}"; done ;;
    *) echo "usage: $0 {restore|save} [sstate] [downloads]" >&2; exit 2 ;;
  esac
}

main "$@"
