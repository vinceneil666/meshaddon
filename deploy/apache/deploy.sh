#!/bin/bash
# Deploy the configurator to an Apache + PHP host (shared hosting is fine) over SSH.
#   deploy/apache/deploy.sh user@host:path/to/webroot/gpio-configurator
# Copies web/, fw/manifest.json + fw/*.bin, mc.php and .htaccess. Needs rsync on both ends (falls back to scp).
# If the site's root .htaccess rewrites everything to an app (WordPress, BookStack …), exclude the folder there, e.g.
#   RewriteCond %{REQUEST_URI} !^/gpio-configurator(/|$)
set -euo pipefail
DEST=${1:?usage: deploy.sh user@host:path}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=$(mktemp -d); chmod 755 "$STAGE"; trap 'rm -rf "$STAGE"' EXIT
cp -r "$ROOT/web/." "$STAGE/"
mkdir -p "$STAGE/fw"; cp "$ROOT"/fw/manifest.json "$ROOT"/fw/*.bin "$STAGE/fw/" 2>/dev/null || echo "note: no local fw/ builds, firmware install will offer official builds only"
cp "$ROOT/deploy/apache/mc.php" "$STAGE/"
# CSP: allow exactly the one inline <script> in index.html (by hash), nothing injected
HASH=$(python3 -c 'import re,sys,hashlib,base64; s=open(sys.argv[1],encoding="utf-8").read(); m=re.findall(r"<script>(.*?)</script>",s,re.S); assert len(m)==1,"expected one inline script"; print("sha256-"+base64.b64encode(hashlib.sha256(m[0].encode()).digest()).decode())' "$STAGE/index.html")
sed "s|__INLINE_SCRIPT_HASH__|$HASH|" "$ROOT/deploy/apache/htaccess" > "$STAGE/.htaccess"
${RSYNC:-rsync} -rtz --perms --chmod=D755,F644 --delete --exclude .cache ${RSYNC_RSH:+-e "$RSYNC_RSH"} "$STAGE/" "$DEST/"
echo "deployed to $DEST"
