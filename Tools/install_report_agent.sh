#!/bin/bash
# Installs a launchd agent that runs Tools/fetch_reports.py every 10 minutes, so reports from
# testers land in Reports/ by themselves (ntfy.sh only keeps them ~12 hours).
#   bash Tools/install_report_agent.sh            install / update
#   bash Tools/install_report_agent.sh remove     uninstall
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABEL=com.dragonflight.reports
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
if [ "$1" = "remove" ]; then rm -f "$PLIST"; echo "removed"; exit 0; fi
mkdir -p "$HOME/Library/LaunchAgents" "$ROOT/Reports"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array>
    <string>/usr/bin/python3</string><string>$ROOT/Tools/fetch_reports.py</string>
  </array>
  <key>EnvironmentVariables</key><dict><key>DF_NOTIFY</key><string>1</string></dict>
  <key>StartInterval</key><integer>600</integer>
  <key>RunAtLoad</key><true/>
  <key>StandardOutPath</key><string>$ROOT/Reports/.agent.log</string>
  <key>StandardErrorPath</key><string>$ROOT/Reports/.agent.log</string>
</dict></plist>
EOF
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "installed: reports every 10 min into $ROOT/Reports"
