#!/usr/bin/env bash
# Run after git pull --ff-only in the existing server checkout.
set -Eeuo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
exec bash "$project_dir/deploy.sh" "$@"
