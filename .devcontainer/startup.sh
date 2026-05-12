#!/usr/bin/env bash

set -euo pipefail

export LANG=C.UTF-8
export LC_ALL=C.UTF-8

if [ -f /opt/esp/idf/export.sh ]; then
  # shellcheck disable=SC1091
  . /opt/esp/idf/export.sh >/dev/null
fi

git config --global --add safe.directory /workspace >/dev/null 2>&1 || true

echo "ESP-IDF devcontainer ready. Use 'make bridge-container' after starting the host socat bridge."
