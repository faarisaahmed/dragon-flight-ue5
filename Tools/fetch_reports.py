#!/usr/bin/env python3
"""Pull problem reports (crashes, failed installs, F8 screenshots) into Reports/.

The game and the Windows installer post them to a private ntfy.sh topic (a free message drop:
no accounts, nothing for the tester to do). ntfy.sh keeps messages for about 12 hours, so this
runs every 10 minutes from a launchd agent (Tools/install_report_agent.sh) and saves each
report as a file:

    Reports/2026-11-05_14-02-11_crash.md
    Reports/2026-11-05_14-05-40_f8-screenshot.jpg

Run it by hand any time:  python3 Tools/fetch_reports.py
"""
import json
import os
import re
import sys
import time
import urllib.request

TOPIC = "dragonflight-b549fcbc08495fc8"   # same as DragonReport.cpp and Install.ps1
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Reports")
SEEN = os.path.join(OUT, ".seen")


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "dragonflight-reports"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def slug(s):
    s = re.sub(r"[^a-zA-Z0-9]+", "-", s or "report").strip("-").lower()
    return s[:40] or "report"


def main():
    os.makedirs(OUT, exist_ok=True)
    seen = set()
    if os.path.exists(SEEN):
        seen = set(open(SEEN).read().split())
    raw = get(f"https://ntfy.sh/{TOPIC}/json?poll=1&since=all").decode("utf-8", "replace")
    new = 0
    for line in raw.splitlines():
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if m.get("event") != "message" or m.get("id") in seen:
            continue
        stamp = time.strftime("%Y-%m-%d_%H-%M-%S", time.localtime(m.get("time", time.time())))
        title = m.get("title", "report")
        base = os.path.join(OUT, f"{stamp}_{slug(title)}")
        att = m.get("attachment")
        text = m.get("message", "")
        if att and att.get("url"):
            name = att.get("name", "file")
            data = get(att["url"])
            if name == "attachment.txt" or name.endswith(".txt"):
                # A long text report arrives as an attached file; it is the report itself.
                with open(base + ".md", "wb") as f:
                    f.write(f"# {title}\n\n".encode() + data)
            else:
                ext = os.path.splitext(name)[1] or ".bin"
                with open(base + ext, "wb") as f:
                    f.write(data)
                if text:
                    with open(base + ".md", "w") as f:
                        f.write(f"# {title}\n\n{text}\n\n![]({os.path.basename(base + ext)})\n")
        else:
            with open(base + ".md", "w") as f:
                f.write(f"# {title}\n\n{text}\n")
        seen.add(m["id"])
        new += 1
        print("saved", os.path.basename(base))
    with open(SEEN, "w") as f:
        f.write("\n".join(sorted(seen)))
    if new and sys.platform == "darwin" and os.environ.get("DF_NOTIFY"):
        os.system(f"osascript -e 'display notification \"{new} new report(s) in Reports/\" with title \"Dragon Flight\"'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
