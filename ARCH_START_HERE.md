# Запуск Kufar-бота на Arch

Инструкция по запуску и обновлению бота. В репозитории нет готовых личных поисков:
`kufar-configuration.json` содержит пустой список `queries`. Свои запросы
добавляйте через Telegram; они сохраняются в локальном кеше сервера.

## Установка

```sh
sudo pacman -Syu --needed git
git clone https://github.com/IDDQDD/Kufar-Telegram-Notifier.git
cd Kufar-Telegram-Notifier
bash deploy-arch.sh --prepare
nano .env
```

Укажите действующий `TELEGRAM_BOT_TOKEN`, свой числовой `TELEGRAM_CHAT_ID`
и `TELEGRAM_ADMIN_ID`. Если переносите бота, восстановите конфигурацию в
`kufar-configuration.json`, а историю — в `data/cached-data.json` до запуска.
Переменная `KUFAR_RECIPIENTS_JSON`, если задана, переопределяет конфигурацию файла.
Личные запросы, токены, `.env` и рабочий кеш храните только на сервере.

```sh
bash deploy-arch.sh
sudo docker compose ps
sudo docker compose logs --tail=100 -f
```

Владелец управляет доступом через `/users`. Подключённые пользователи сами
добавляют и удаляют свои поиски через `/menu`. `/status` показывает состояние,
`/backup` сохраняет конфигурацию и историю для переноса.

## Обновление

```sh
git pull --ff-only && bash update.sh
```

Если установка сделана из ZIP, сначала подключите её к Git по инструкции
в [SERVER_SETUP.md](SERVER_SETUP.md). Обновление сохраняет `.env`, серверную
конфигурацию и историю. Пустые списки в GitHub не удаляют активные поиски
из серверного кеша.

Docker останавливать не требуется: скрипт собирает новую версию и заменяет
контейнер после успешных тестов. Если нужно вручную остановить только бота:

```sh
sudo docker compose stop bot
```

Данные остаются в `data/`. После остановки контейнера отдельно завершать процесс
бота не требуется. Не запускайте одновременно две копии с одним токеном.

Подробности: [DEPLOY_ARCH.md](DEPLOY_ARCH.md) и [SERVER_SETUP.md](SERVER_SETUP.md).
