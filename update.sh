#!/usr/bin/env bash
# Run after git pull --ff-only in the existing server checkout.
set -Eeuo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
if [[ "${1:-}" == --local-proxy ]]; then
    [[ $# == 1 ]] || { printf '%s\n' 'Use --local-proxy without other arguments.' >&2; exit 1; }
    cd -- "$project_dir"
    [[ -f .env ]] || { printf '%s\n' 'Prepare .env before switching proxies.' >&2; exit 1; }
    umask 077
    backup="$(mktemp .env.before-local-proxy.XXXXXX)"
    cp -- .env "$backup"
    chmod 600 "$backup"
    replacement="$(mktemp .env.local-proxy.XXXXXX)"
    trap 'rm -f -- "$replacement"' EXIT
    # Do not source .env: retain non-proxy settings verbatim, including secrets.
    awk '!/^[[:space:]]*(export[[:space:]]+)?KUFAR_PROXY(_POOL(_FILE)?)?[[:space:]]*=/' .env > "$replacement"
    printf '\n%s\n' 'KUFAR_PROXY=socks5h://127.0.0.1:1080' 'KUFAR_PROXY_POOL=' >> "$replacement"
    mv -- "$replacement" .env
    trap - EXIT
    printf 'Local SOCKS5h proxy applied; reserve routes disabled. Previous settings saved to %s\n' "$backup"
    shift
fi
exec bash "$project_dir/deploy.sh" "$@"
