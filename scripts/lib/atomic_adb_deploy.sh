#!/usr/bin/env bash

# Deploy a complete set of files without ever streaming bytes into a live path.
# Each specification is: local-path|remote-name|mode.  Callers must put the
# main executable last so an interrupted commit leaves the old executable in
# place until every data file has already been renamed.
atomic_adb_deploy() (
  set -euo pipefail

  if (( $# < 3 )); then
    echo "atomic_adb_deploy: REMOTE_DIR LABEL FILE_SPEC..." >&2
    return 2
  fi

  local remote_dir="$1"
  local label="$2"
  shift 2

  if [[ ! "$remote_dir" =~ ^/root/[A-Za-z0-9._-]+$ ]]; then
    echo "Unsafe remote deployment directory: $remote_dir" >&2
    return 2
  fi
  if [[ ! "$label" =~ ^[A-Za-z0-9._-]+$ ]]; then
    echo "Unsafe deployment label: $label" >&2
    return 2
  fi
  if [[ "$(adb get-state 2>/dev/null || true)" != "device" ]]; then
    echo "No online ADB device" >&2
    return 1
  fi

  local stage="${remote_dir}/.deploy-${label}-$$"
  local local_tmp
  local_tmp=$(mktemp -d /tmp/rv1126-deploy-manifest.XXXXXX)
  trap 'rm -rf "$local_tmp"' EXIT
  local manifest="$local_tmp/SHA256SUMS"
  : >"$manifest"

  local spec local_path remote_name mode hash
  for spec in "$@"; do
    IFS='|' read -r local_path remote_name mode <<<"$spec"
    if [[ -z "$local_path" || ! -s "$local_path" ]]; then
      echo "Missing or empty deployment file: $local_path" >&2
      return 1
    fi
    if [[ ! "$remote_name" =~ ^[A-Za-z0-9._-]+$ ]]; then
      echo "Unsafe remote filename: $remote_name" >&2
      return 2
    fi
    if [[ "$mode" != 0644 && "$mode" != 0755 ]]; then
      echo "Unsupported mode for $remote_name: $mode" >&2
      return 2
    fi
    hash=$(sha256sum "$local_path")
    printf '%s  %s\n' "${hash%% *}" "$remote_name" >>"$manifest"
  done

  adb shell "mkdir -p '$remote_dir' && test ! -e '$stage' && mkdir '$stage'"
  echo "Uploading to staging directory: $stage"
  for spec in "$@"; do
    IFS='|' read -r local_path remote_name mode <<<"$spec"
    adb push "$local_path" "$stage/$remote_name"
  done
  adb push "$manifest" "$stage/SHA256SUMS"
  adb shell "cd '$stage' && sha256sum -c SHA256SUMS"

  local commit="set -e; target='$remote_dir'; stage='$stage';"
  for spec in "$@"; do
    IFS='|' read -r local_path remote_name mode <<<"$spec"
    commit+=" chmod $mode \"\$stage/$remote_name\";"
    commit+=" if [ -e \"\$target/$remote_name\" ]; then ln -f \"\$target/$remote_name\" \"\$target/$remote_name.previous\"; fi;"
    commit+=" mv -f \"\$stage/$remote_name\" \"\$target/$remote_name\";"
  done
  commit+=" if [ -e \"\$target/DEPLOYED_SHA256SUMS\" ]; then ln -f \"\$target/DEPLOYED_SHA256SUMS\" \"\$target/DEPLOYED_SHA256SUMS.previous\"; fi;"
  commit+=" mv -f \"\$stage/SHA256SUMS\" \"\$target/DEPLOYED_SHA256SUMS\"; rmdir \"\$stage\"; sync"

  # Every uploaded file has been verified before this short commit phase.
  # rename(2) keeps each destination either old or complete, never truncated.
  adb shell "$commit"
  adb shell "cd '$remote_dir' && sha256sum -c DEPLOYED_SHA256SUMS"
  echo "Atomic deployment complete: $remote_dir ($label)"
)
