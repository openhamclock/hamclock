# Written initially by ChatGPT at the request and to the specs of Burns Fisher, WB1FJ
# Burns Fisher claims no copyright and declares this software to be totally free and
# unrestricted to anyone without the need for any license.  It may also be incorporated
# into any proprietary software.
#
# Fetches the latest TLEs from AMSATÕs dailytle.txt.
# Updates the local file so each satellite in user-esats.txt is replaced with the corresponding (same-name) TLE from AMSAT.
#!/usr/bin/env python3

import requests
import sys
from pathlib import Path

LOCAL_FILE = "user-esats.txt"
REMOTE_URL = "https://www.amsat.org/tle/dailytle.txt"

def load_tle_file(filename):
    """Load TLEs from a local file into a dict {satellite_name: [line1, line2]}."""
    tle_dict = {}
    with open(filename, "r") as f:
        lines = [line.strip() for line in f if line.strip()]
    for i in range(0, len(lines), 3):
        if i + 2 < len(lines):
            name, l1, l2 = lines[i:i+3]
            tle_dict[name] = [l1, l2]
    return tle_dict

def fetch_remote_tles(url):
    """Fetch and parse remote TLEs into a dict {satellite_name: [line1, line2]}."""
    resp = requests.get(url, timeout=15)
    resp.raise_for_status()
    lines = [line.strip() for line in resp.text.splitlines() if line.strip()]
    tle_dict = {}
    for i in range(0, len(lines), 3):
        if i + 2 < len(lines):
            name, l1, l2 = lines[i:i+3]
            tle_dict[name] = [l1, l2]
    return tle_dict

def update_local_tles(local_file, remote_tles):
    """Update local TLEs with remote ones if available."""
    local_tles = load_tle_file(local_file)
    updated_tles = {}
    for name, tle in local_tles.items():
        if name in remote_tles:
            updated_tles[name] = remote_tles[name]
            print(f"Updated: {name}")
        else:
            updated_tles[name] = tle
            print(f"No update available for: {name}")
    # Write back
    with open(local_file, "w") as f:
        for name, (l1, l2) in updated_tles.items():
            f.write(f"{name}\n{l1}\n{l2}\n")

def main():
    if not Path(LOCAL_FILE).exists():
        print(f"Error: {LOCAL_FILE} not found.")
        sys.exit(1)

    print("Fetching remote TLEs...")
    try:
        remote_tles = fetch_remote_tles(REMOTE_URL)
    except Exception as e:
        print(f"Failed to fetch remote TLEs: {e}")
        sys.exit(1)

    print("Updating local TLEs...")
    update_local_tles(LOCAL_FILE, remote_tles)
    print("Done.")

if __name__ == "__main__":
    main()
