#!/usr/bin/env bash
# Creates the Cloudflare Access application for the whole hostname, with one allow policy that holds
# only the logged-in account's own email (read from `cf auth whoami` now, never stored in a file).
# Does nothing when an application with the same name exists. Pass --dry-run to only print the request.
. "$(dirname "$0")/common.sh"

DRY=()
[ "${1:-}" = "--dry-run" ] && DRY=(--dry-run)

existing=$("$CF_BIN" zero-trust access applications list 2>/dev/null | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
apps = d if isinstance(d, list) else d.get("applications") or d.get("result") or []
print(next((a["id"] for a in apps if a.get("name") == sys.argv[1]), ""))
' "$ACCESS_APP_NAME")
if [ -n "$existing" ]; then
  echo "Access application $ACCESS_APP_NAME already exists; leaving it alone."
  exit 0
fi

# Identity provider: the account's own Cloudflare login, when it is configured. Otherwise Access
# offers every provider, which the email-only policy still restricts.
idp=$("$CF_BIN" zero-trust identity-providers list 2>/dev/null | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
l = d if isinstance(d, list) else d.get("identity_providers") or d.get("result") or []
print(next((i["id"] for i in l if i.get("type") == "cloudflare"), ""))
')

body=$("$CF_BIN" auth whoami 2>/dev/null | ANOMALY_WEB_HOST="$ANOMALY_WEB_HOST" ACCESS_APP_NAME="$ACCESS_APP_NAME" \
  ACCESS_POLICY_NAME="$ACCESS_POLICY_NAME" IDP="$idp" python3 -I -c '
import json, os, sys
who = json.load(sys.stdin)
email = who.get("email")
if not email:
    sys.exit("cf auth whoami returned no email; log in first")
app = {
    "type": "self_hosted",
    "name": os.environ["ACCESS_APP_NAME"],
    "destinations": [{"type": "public", "uri": os.environ["ANOMALY_WEB_HOST"]}],
    "domain": os.environ["ANOMALY_WEB_HOST"],
    "session_duration": "24h",
    "app_launcher_visible": False,
    "http_only_cookie_attribute": True,
    "options_preflight_bypass": False,
    "policies": [{
        "name": os.environ["ACCESS_POLICY_NAME"],
        "decision": "allow",
        "include": [{"email": {"email": email}}],
        "exclude": [],
        "require": [],
    }],
}
if os.environ.get("IDP"):
    app["allowed_idps"] = [os.environ["IDP"]]
    app["auto_redirect_to_identity"] = True
print(json.dumps(app))
')

if [ ${#DRY[@]} -gt 0 ]; then
  # Show the request without the email.
  echo "$body" | python3 -I -c '
import json, sys
b = json.load(sys.stdin)
for p in b["policies"]:
    for rule in p["include"]:
        rule["email"]["email"] = "<your login email>"
print(json.dumps(b, indent=2))'
  exit 0
fi
"$CF_BIN" zero-trust access applications create --body "$body"
