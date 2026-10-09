#!/usr/bin/env bash
# Attach Git to an existing archive installation, preserving runtime settings.
set -Eeuo pipefail
umask 077

fail() { printf '\nОшибка: %s\n' "$*" >&2; exit 1; }
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    printf '%s\n' 'bash setup-git.sh [папка бота] — подключить установку к Git и обновить контейнер' \
        'По умолчанию используется текущая папка. .env, конфигурация и история сохраняются.'
    exit 0
fi
[[ $# -le 1 ]] || fail 'Используйте --help.'
project_dir="$(cd -- "${1:-$PWD}" && pwd -P)"
cd -- "$project_dir"
[[ -f .env && -f kufar-configuration.json && -f compose.yaml ]] \
    || fail 'Перейдите в папку бота: в ней должны быть .env, compose.yaml и kufar-configuration.json.'
command -v git >/dev/null || fail 'Установите Git, затем повторите команду.'

if [[ -e .git ]]; then
    git_root="$(git rev-parse --show-toplevel)"
    [[ "$(cd -- "$git_root" && pwd -P)" == "$project_dir" ]] || fail 'Не удалось определить репозиторий этой папки.'
    git pull --ff-only
    exec bash "$project_dir/update.sh"
fi
command -v tar >/dev/null || fail 'Нужен tar для резервной копии текущих файлов.'
repository_url="${KUFAR_REPOSITORY_URL:-https://github.com/IDDQDD/Kufar-Telegram-Notifier.git}"
staging="$(mktemp -d "${TMPDIR:-/tmp}/kufar-git.XXXXXXXX")"
cleanup() { rm -rf -- "$staging"; }
trap cleanup EXIT

# Complete the network operation before changing the current installation.
git clone --no-checkout --single-branch --branch main -- "$repository_url" "$staging/repository"
git -C "$staging/repository" read-tree HEAD
git -C "$staging/repository" config core.autocrlf false

# Save the original source and settings. Runtime history stays in its existing place.
mkdir -p -- backups
backup="$project_dir/backups/before-git-$(date +%Y%m%d-%H%M%S)-$$.tar.gz"
existing=()
for name in src include tests CMakeLists.txt Dockerfile compose.yaml .dockerignore .gitignore .gitattributes \
    .env .env.example kufar-configuration.json deploy.sh deploy-arch.sh update.sh setup-git.sh \
    README.md SERVER_SETUP.md DEPLOY_ARCH.md ARCH_START_HERE.md; do
    if [[ -e "$name" ]]; then existing+=("$name"); fi
done
tar -czf "$backup" -- "${existing[@]}"
chmod 600 "$backup"
printf 'Исходные файлы и настройки сохранены: %s\n' "$backup"

# Attach the real clone's metadata. Restore code only; keep server configuration.
mv -- "$staging/repository/.git" "$project_dir/.git"
git restore --worktree -- . ':(exclude)kufar-configuration.json' ':(exclude).env' \
    ':(exclude)cached-data.json' ':(exclude)data/**' ':(exclude)backups/**'
git update-index --skip-worktree -- kufar-configuration.json
printf '%s\n' 'Git подключён к ветке main. Настройки и история сохранены.' \
    'Следующее обновление: git pull --ff-only && bash update.sh'
bash "$project_dir/update.sh"
