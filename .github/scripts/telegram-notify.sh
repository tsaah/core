#!/usr/bin/env bash

# Send a Telegram message using TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID.
# Skips silently when either variable is unset so forks without the secrets
# can still run the workflow.

set -euo pipefail

message="${1:-}"

if [ -z "$message" ]; then
  echo "Usage: $0 <message>" >&2
  exit 1
fi

if [ -z "${TELEGRAM_BOT_TOKEN:-}" ] || [ -z "${TELEGRAM_CHAT_ID:-}" ]; then
  echo "TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID is not set; skipping notification."
  exit 0
fi

# Telegram rejects messages longer than 4096 characters.
message="${message:0:3900}"

if ! curl -fsS --retry 3 --max-time 30 \
  "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
  --data-urlencode "chat_id=${TELEGRAM_CHAT_ID}" \
  --data-urlencode "text=${message}" \
  -o /dev/null; then
  echo "Failed to send Telegram notification." >&2
fi
