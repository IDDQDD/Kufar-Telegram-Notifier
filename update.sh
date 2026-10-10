#!/usr/bin/env bash
# Run after git pull --ff-only in the existing server checkout.
set -Eeuo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
if [[ "${1:-}" == --netherlands-proxies ]]; then
    [[ $# == 1 ]] || { printf '%s\n' 'Use --netherlands-proxies without other arguments.' >&2; exit 1; }
    cd -- "$project_dir"
    [[ -f .env ]] || { printf '%s\n' 'Prepare .env before switching proxies.' >&2; exit 1; }
    umask 077
    backup="$(mktemp .env.before-netherlands.XXXXXX)"
    cp -- .env "$backup"
    chmod 600 "$backup"
    replacement="$(mktemp .env.netherlands.XXXXXX)"
    trap 'rm -f -- "$replacement"' EXIT
    # Do not source .env: retain non-proxy settings verbatim, including secrets.
    awk '!/^[[:space:]]*(export[[:space:]]+)?KUFAR_PROXY(_POOL(_FILE)?)?[[:space:]]*=/' .env > "$replacement"
    printf '\n%s\n' 'KUFAR_PROXY=http://85.209.156.148:1080' >> "$replacement"
    mv -- "$replacement" .env
    trap - EXIT
    printf 'Netherlands proxy settings applied. Previous settings saved to %s\n' "$backup"
    shift
fi
exec bash "$project_dir/deploy.sh" "$@"
