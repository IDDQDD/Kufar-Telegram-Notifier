#!/usr/bin/env bash
# Portable Linux deployment with an existing Docker Engine + Compose plugin.
set -Eeuo pipefail
umask 077

fail() { printf '\nОшибка: %s\n' "$*" >&2; exit 1; }
mode="${1:-start}"
if [[ "$mode" == '--help' || "$mode" == '-h' ]]; then
    printf '%s\n' 'bash deploy.sh --prepare — создать .env и папку data без запуска' \
        'bash deploy.sh — собрать, проверить и запустить бота через Docker Compose' \
        'bash deploy.sh --clear-my-queries — обновить и удалить только запросы владельца' \
        'Настройки, кеш и резервные копии не перезаписываются. Инструкция: SERVER_SETUP.md'
    exit 0
fi
[[ $# -le 1 && ( "$mode" == start || "$mode" == --prepare || "$mode" == --clear-my-queries ) ]] || fail 'Неизвестные аргументы. Используйте --help.'
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd -- "$project_dir"
[[ -f compose.yaml && -f Dockerfile && -f .env.example && -f kufar-configuration.json ]] || fail 'Нужна полная папка проекта.'
mkdir -p -- data
created=false
if [[ ! -e .env ]]; then
    cp -- .env.example .env
    chmod 600 .env
    created=true
fi
if [[ "$created" == true || "$mode" == --prepare ]]; then
    printf '%s\n' 'Подготовлено. Бот пока не запущен.' \
        'Заполните TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID и TELEGRAM_ADMIN_ID в .env.' \
        'Для переноса положите резервную копию в data/cached-data.json и восстановите конфигурацию.' \
        'Затем выполните: bash deploy.sh'
    exit 0
fi

command -v docker >/dev/null || fail 'Установите Docker Engine и Compose: см. SERVER_SETUP.md.'
docker_cmd=(docker)
if ! docker info >/dev/null 2>&1; then
    command -v sudo >/dev/null || fail 'Нет доступа к Docker. Запустите Docker и проверьте права.'
    docker_cmd=(sudo docker)
fi
"${docker_cmd[@]}" info >/dev/null
"${docker_cmd[@]}" compose version
# Validate required variables without printing resolved secrets or sourcing .env.
"${docker_cmd[@]}" compose config --quiet
free_kib="$(df -Pk data | awk 'NR == 2 {print $4}')"
[[ "$free_kib" =~ ^[0-9]+$ ]] || fail 'Не удалось проверить свободное место.'
(( free_kib >= 1048576 )) || fail 'Для развёртывания оставьте хотя бы 1 ГиБ свободного места в разделе проекта.'

# A failed build/test leaves the current running container untouched.
"${docker_cmd[@]}" compose build
if [[ "$mode" == --clear-my-queries ]]; then
    "${docker_cmd[@]}" compose stop bot
    "${docker_cmd[@]}" compose run --rm --no-deps bot --clear-my-queries
fi
"${docker_cmd[@]}" compose up -d --no-build
container_id="$("${docker_cmd[@]}" compose ps -aq bot)"
[[ -n "$container_id" ]] || fail 'Контейнер бота не создан.'
initial_state="$("${docker_cmd[@]}" inspect --format '{{.State.Status}} {{.RestartCount}}' "$container_id")"
sleep 10
state="$("${docker_cmd[@]}" inspect --format '{{.State.Status}} {{.RestartCount}}' "$container_id")"
[[ "$state" == "running ${initial_state##* }" ]] || fail 'Бот остановился или перезапускается. Проверьте: sudo docker compose logs --tail=50'
"${docker_cmd[@]}" compose ps
printf '\n%s\n' 'Контейнер работает. В Telegram отправьте /menu и /status для проверки связи.' \
    'Логи: sudo docker compose logs --tail=100 -f' \
    'Остановка: sudo docker compose stop' \
    'Обновление: после обновления исходников снова выполните bash deploy.sh'
