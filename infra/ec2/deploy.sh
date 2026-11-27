#!/usr/bin/env bash
# Deploys a new version onto a server prepared by setup.sh.
#
# Usage (as root, from the updated checkout, e.g. after `git pull`):
#   sudo bash infra/ec2/deploy.sh
#
# Steps: sync code -> install Python deps -> run migrations -> build frontend ->
# restart services one by one -> health check. The database is backed up first.
set -euo pipefail

APP_USER=smartclass
APP_DIR=/opt/smartclass
ETC_DIR=/etc/smartclass
WEB_DIR=/var/www/smartclass
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo)." >&2
  exit 1
fi

echo "==> Backup before migrating"
systemctl start smartclass-backup.service

echo "==> Sync code"
if [[ "${REPO_DIR}" != "${APP_DIR}" ]]; then
  rsync -a --delete \
    --exclude '.git' --exclude 'node_modules' --exclude '.venv' --exclude '__pycache__' \
    --exclude 'frontend/dist' --exclude 'backend/data' --exclude 'backend/.env' --exclude 'backend/secrets' \
    "${REPO_DIR}/" "${APP_DIR}/"
fi
chown -R root:"${APP_USER}" "${APP_DIR}"
chmod -R g+rX,o-rwx "${APP_DIR}"

echo "==> Python dependencies and migrations"
"${APP_DIR}/backend/.venv/bin/pip" install --quiet -e "${APP_DIR}/backend[asr,pdf]"
chown -R root:"${APP_USER}" "${APP_DIR}/backend"
chmod -R g+rX,o-rwx "${APP_DIR}/backend"
(
  cd "${APP_DIR}/backend"
  set -a; . "${ETC_DIR}/backend.env"; set +a
  sudo -E -u "${APP_USER}" "${APP_DIR}/backend/.venv/bin/alembic" upgrade head
)

echo "==> Frontend"
(
  cd "${APP_DIR}/frontend"
  HUSKY=0 npm ci --no-audit --no-fund
  npm run build
  rsync -a --delete --chown=root:root --chmod=D755,F644 dist/ "${WEB_DIR}/"  # public files: Caddy must read them
)

echo "==> systemd units and Caddyfile"
install -m 0644 "${APP_DIR}"/infra/systemd/*.service "${APP_DIR}"/infra/systemd/*.timer /etc/systemd/system/
install -m 0755 "${APP_DIR}/infra/backup/pg-backup.sh" /usr/local/bin/smartclass-backup
systemctl daemon-reload
# Servers set up before the retention timer existed get it here (no-op once enabled).
systemctl enable --now smartclass-retention.timer

echo "==> Restart services"
# Workers first (they resume from Redis/PostgreSQL), then the gateway (devices reconnect and
# resend un-ACKed chunks), then the API (browsers reconnect and replay from their last seq).
units=(smartclass-notes smartclass-asr smartclass-gateway smartclass-api)
# The AWS IoT bridge, on servers that have its certificate (infra/aws/bootstrap.sh).
systemctl is-enabled --quiet smartclass-iot 2>/dev/null && units=(smartclass-notes smartclass-asr smartclass-iot smartclass-gateway smartclass-api)
for unit in "${units[@]}"; do
  systemctl restart "${unit}"
  sleep 2
  systemctl is-active --quiet "${unit}" || { journalctl -u "${unit}" -n 50 --no-pager; exit 1; }
done

echo "==> Health"
for _ in $(seq 1 15); do
  if curl -fsS http://127.0.0.1:8000/health && curl -fsS http://127.0.0.1:8001/health; then
    echo
    echo "Deployed."
    exit 0
  fi
  sleep 2
done
echo "Health check failed." >&2
exit 1
