#!/usr/bin/env bash
# Run from a clone: bash deploy-arch.sh [--prepare]
set -Eeuo pipefail
umask 077

fail() { printf '\nОшибка: %s\n' "$*" >&2; exit 1; }
ask() {
    local answer
    read -r -p "$1 [да/нет]: " answer || return 1
    [[ "$answer" == 'да' || "$answer" == 'Да' || "$answer" == 'y' || "$answer" == 'yes' ]]
}

mode="${1:-start}"
if [[ "$mode" == '--help' || "$mode" == '-h' ]]; then
    printf '%s\n' 'bash deploy-arch.sh --prepare — установить зависимости и подготовить .env без запуска' \
        'bash deploy-arch.sh — проверить настройки, собрать и запустить бота' \
        'Существующие .env, конфигурация и кеш не перезаписываются.'
    exit 0
fi
[[ $# -le 1 && ( "$mode" == 'start' || "$mode" == '--prepare' ) ]] || fail 'Неизвестные аргументы. Используйте --help.'
[[ -t 0 ]] || fail 'Запускайте в обычном терминале: нужны подтверждения.'
[[ -r /etc/arch-release ]] || fail 'Этот скрипт предназначен для Arch Linux. На других Linux используйте docker compose вручную.'

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd -- "$project_dir"
[[ -f compose.yaml && -f Dockerfile && -f .env.example && -f kufar-configuration.json ]] || fail 'Нужен полный клон репозитория, не только этот скрипт.'
trap 'printf "\nРазвёртывание прервано. Настройки и кеш не удалены. Проверьте сообщение выше.\n" >&2' ERR

privileged=()
if (( EUID != 0 )); then
    command -v sudo >/dev/null || fail 'Нужен sudo или запуск от root.'
    privileged=(sudo)
fi

missing=()
for package in docker docker-compose docker-buildx jq; do
    pacman -Q "$package" >/dev/null 2>&1 || missing+=("$package")
done
if (( ${#missing[@]} )); then
    printf 'Нужны пакеты: %s\n' "${missing[*]}"
    printf '%s\n' 'Arch требует полное обновление системы вместе с установкой пакетов (pacman -Syu).'
    ask 'Разрешить обновление системы и установку?' || fail 'Установка отменена.'
    "${privileged[@]}" pacman -Syu --needed "${missing[@]}"
fi

if ! systemctl is-active --quiet docker; then
    ask 'Включить Docker сейчас и при загрузке сервера?' || fail 'Без Docker продолжить нельзя.'
    "${privileged[@]}" systemctl enable --now docker
elif ! systemctl is-enabled --quiet docker; then
    ask 'Включить автозапуск Docker после перезагрузки?' || fail 'Автозапуск не настроен.'
    "${privileged[@]}" systemctl enable docker
fi

docker_cmd=("${privileged[@]}" docker)
"${docker_cmd[@]}" info >/dev/null
"${docker_cmd[@]}" compose version
mkdir -p -- data

created_env=false
if [[ ! -e .env ]]; then
    cp -- .env.example .env
    chmod 600 .env
    created_env=true
fi

printf '\nПапка проекта: %s\n' "$project_dir"
printf '%s\n' 'Кеш хранится в data/cached-data.json и переживает замену контейнера.'
if [[ "$created_env" == true || "$mode" == '--prepare' ]]; then
    printf '\n%s\n' 'Подготовка завершена. Бот пока НЕ запущен.' \
        '1. Откройте .env редактором и задайте токен, свой chat ID и ADMIN ID.' \
        '2. Если есть сохранённые запросы, укажите KUFAR_RECIPIENTS_JSON; иначе добавите их через меню бота.' \
        '3. Если есть резервная копия кеша, скопируйте её в data/cached-data.json.' \
        '4. Выполните ещё раз: bash deploy-arch.sh' \
        'Подробности: DEPLOY_ARCH.md. Не публикуйте .env и кеш.'
    exit 0
fi

# Never source .env as shell code, and never print its resolved secrets.
"${docker_cmd[@]}" compose config --quiet
jq empty kufar-configuration.json
if [[ -e data/cached-data.json ]]; then
    "${privileged[@]}" jq -e 'type == "object" or type == "array"' data/cached-data.json >/dev/null \
        || fail 'Кеш пустой или повреждён. Восстановите копию перед запуском.'
else
    printf '\n%s\n' 'ВНИМАНИЕ: кеша нет. Старые уведомления и изменения запросов из меню не перенесены.' \
        'Если есть резервная копия, сначала скопируйте её. Без копии история начнётся заново.'
    read -r -p 'Только для осознанного запуска с пустой историей введите NEW: ' confirmation
    [[ "$confirmation" == 'NEW' ]] || fail 'Запуск без кеша отменён.'
fi

printf '\n%s\n' 'При пустой конфигурации вы сможете добавить пользователей и запросы через меню бота.' \
    'Если где-либо работает другая копия этого бота с тем же токеном, остановите её.'
read -r -p 'Чтобы подтвердить это и запустить контейнер, введите START: ' confirmation
[[ "$confirmation" == 'START' ]] || fail 'Запуск отменён.'

# Build first: a failed build does not replace an already running container.
"${docker_cmd[@]}" compose build
"${docker_cmd[@]}" compose up -d --no-build
"${docker_cmd[@]}" compose ps
printf '\n%s\n' 'Контейнер запущен. Проверьте /menu и /status в Telegram.' \
    'Логи: sudo docker compose logs --tail=100 -f' \
    'Остановка: sudo docker compose stop' \
    'Настройки и кеш сохранены. Скрипт не удаляет старые Docker-образы или резервные копии.'
