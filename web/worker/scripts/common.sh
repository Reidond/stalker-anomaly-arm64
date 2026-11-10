# Sourced by the other scripts: loads deploy.local.env and checks the settings.
set -euo pipefail

WORKER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ANOMALY_WEB_ENV:-$WORKER_DIR/deploy.local.env}"
if [ -f "$ENV_FILE" ]; then
  # shellcheck disable=SC1090
  . "$ENV_FILE"
fi
: "${ANOMALY_WEB_HOST:?set ANOMALY_WEB_HOST (see deploy.local.env.example)}"
: "${ANOMALY_WEB_BUCKET:?set ANOMALY_WEB_BUCKET (see deploy.local.env.example)}"
CF_BIN="${CF_BIN:-cf}"

# Resource names. Everything this deployment creates starts with anomaly-web-.
WORKER_NAME="anomaly-web-worker"
ACCESS_APP_NAME="anomaly-web-access"
ACCESS_POLICY_NAME="anomaly-web-allow"

case "$ANOMALY_WEB_BUCKET" in anomaly-web-*) ;; *) echo "bucket name must start with anomaly-web-" >&2; exit 1;; esac
case "$ANOMALY_WEB_HOST" in *[!A-Za-z0-9.-]*|"") echo "bad hostname" >&2; exit 1;; esac
