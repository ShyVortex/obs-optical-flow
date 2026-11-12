#!/usr/bin/env bash
# Print the plugin version from CMakeLists.txt — the single source of truth
# for artifact names, the release tag (v<version>) and the CHANGELOG entry.
set -euo pipefail
cd "$(dirname "$0")/../.."
v=$(sed -nE 's/^project\(obs-optical-flow VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)
if [[ -z "$v" ]]; then
	echo "could not read the version from CMakeLists.txt" >&2
	exit 1
fi
echo "$v"
