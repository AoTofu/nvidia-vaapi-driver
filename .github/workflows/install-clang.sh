#!/bin/bash

set -euo pipefail

if (( $# != 1 )) || [[ ! "$1" =~ ^[0-9]+$ ]]; then
	echo Script requires one argument - the clang version to be installed
	exit 1
fi

compiler="clang-$1"
if ! command -v "$compiler" >/dev/null 2>&1; then
	# Use the runner distribution's authenticated repositories only.
	sudo apt-get update
	sudo apt-get install -y --no-install-recommends "$compiler"
fi
"$compiler" --version
