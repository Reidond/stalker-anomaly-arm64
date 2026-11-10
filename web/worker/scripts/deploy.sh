#!/usr/bin/env bash
# Renders wrangler.jsonc from wrangler.example.jsonc and deploys the Worker on the custom domain.
# Refuses to run unless the Access application for this deployment exists (scripts/setup-access.sh).
# Uses wrangler, not cf: in cf 1.0.0-beta.28 `cf deploy` ignores wrangler.jsonc, and its own
# cloudflare.config.ts (open beta) has no field for the assets directory or run_worker_first.
# Extra arguments go to `wrangler deploy` (for example --dry-run).
. "$(dirname "$0")/common.sh"

found=$("$CF_BIN" zero-trust access applications list 2>/dev/null | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
apps = d if isinstance(d, list) else d.get("applications") or d.get("result") or []
host, name = sys.argv[1], sys.argv[2]
print("yes" if any(a.get("name") == name and a.get("domain") == host and a.get("policies") for a in apps) else "no")
' "$ANOMALY_WEB_HOST" "$ACCESS_APP_NAME")
if [ "$found" != "yes" ]; then
  echo "No Access application $ACCESS_APP_NAME with a policy for $ANOMALY_WEB_HOST. Run scripts/setup-access.sh first." >&2
  exit 1
fi

sed -e "s|__BUCKET__|$ANOMALY_WEB_BUCKET|" -e "s|__HOSTNAME__|$ANOMALY_WEB_HOST|" \
  "$WORKER_DIR/wrangler.example.jsonc" > "$WORKER_DIR/wrangler.jsonc"
cd "$WORKER_DIR"
exec "${WRANGLER_BIN:-wrangler}" deploy -c wrangler.jsonc "$@"
