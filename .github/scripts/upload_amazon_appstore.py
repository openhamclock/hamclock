#!/usr/bin/env python3
"""
Upload an APK and update release notes (recentChanges) on the Amazon Appstore
using the Amazon App Submission REST API.
"""

import argparse
import glob
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

LWA_TOKEN_URL = "https://api.amazon.com/auth/o2/token"
API_BASE_URL = "https://developer.amazon.com/api/appstore/v1/applications"


def get_lwa_token(client_id: str, client_secret: str) -> str:
    """Request Login with Amazon (LWA) OAuth access token."""
    print("Requesting Login with Amazon (LWA) access token...")
    payload = urllib.parse.urlencode({
        "grant_type": "client_credentials",
        "client_id": client_id,
        "client_secret": client_secret,
        "scope": "appstore::apps:readwrite",
    }).encode("utf-8")

    req = urllib.request.Request(
        LWA_TOKEN_URL,
        data=payload,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            return data["access_token"]
    except urllib.error.HTTPError as e:
        error_msg = e.read().decode("utf-8", errors="replace")
        print(f"Error authenticating with Amazon LWA: HTTP {e.code}: {error_msg}", file=sys.stderr)
        sys.exit(1)


def api_request(url: str, token: str, method: str = "GET", data: bytes = None, headers: dict = None) -> tuple:
    """Perform an authenticated request to the Amazon App Submission API."""
    req_headers = {
        "Authorization": f"Bearer {token}",
    }
    if headers:
        req_headers.update(headers)

    req = urllib.request.Request(url, data=data, headers=req_headers, method=method)
    try:
        with urllib.request.urlopen(req) as resp:
            resp_data = resp.read()
            resp_headers = resp.headers
            return resp.status, resp_headers, resp_data
    except urllib.error.HTTPError as e:
        error_msg = e.read().decode("utf-8", errors="replace")
        print(f"API Error [{method} {url}]: HTTP {e.code}: {error_msg}", file=sys.stderr)
        sys.exit(1)


def create_or_get_edit(app_id: str, token: str) -> str:
    """Create a new edit session (upcoming version) for the application."""
    print(f"Creating edit session for App ID: {app_id}...")
    url = f"{API_BASE_URL}/{app_id}/edits"
    _, _, data = api_request(url, token, method="POST", headers={"Content-Type": "application/json"})
    edit_info = json.loads(data.decode("utf-8"))
    edit_id = edit_info.get("id")
    print(f"Edit session created with ID: {edit_id}")
    return edit_id


def upload_apk(app_id: str, edit_id: str, token: str, apk_path: str):
    """Upload or replace APK in the active edit session."""
    print(f"Checking existing APKs in edit session {edit_id}...")
    apks_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/apks"
    _, _, data = api_request(apks_url, token, method="GET")
    existing_apks = json.loads(data.decode("utf-8"))

    file_size = os.path.getsize(apk_path)
    print(f"Uploading APK ({apk_path}, {file_size} bytes)...")

    with open(apk_path, "rb") as f:
        apk_bytes = f.read()

    headers = {
        "Content-Type": "application/vnd.android.package-archive",
    }

    if existing_apks and len(existing_apks) > 0:
        apk_id = existing_apks[0].get("id")
        print(f"Replacing existing APK (ID: {apk_id})...")
        upload_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/apks/{apk_id}/replace"
        api_request(upload_url, token, method="PUT", data=apk_bytes, headers=headers)
    else:
        print("Uploading new APK...")
        upload_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/apks/upload"
        api_request(upload_url, token, method="POST", data=apk_bytes, headers=headers)

    print("APK upload complete.")


def update_release_notes(app_id: str, edit_id: str, token: str, notes_file: str):
    """Update recentChanges (release notes) for the listing."""
    if not notes_file or not os.path.isfile(notes_file):
        print(f"No release notes file found at '{notes_file}', skipping release notes update.")
        return

    with open(notes_file, "r", encoding="utf-8", errors="replace") as f:
        notes_content = f.read().strip()

    if not notes_content:
        print("Release notes file is empty, skipping release notes update.")
        return

    print(f"Updating release notes (recentChanges) from {notes_file}...")

    # Get available listings
    listings_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/listings"
    _, _, data = api_request(listings_url, token, method="GET")
    listings = json.loads(data.decode("utf-8"))

    target_languages = []
    if isinstance(listings, list) and listings:
        target_languages = [l.get("language") for l in listings if "language" in l]
    elif isinstance(listings, dict):
        target_languages = list(listings.keys())

    if not target_languages:
        target_languages = ["en-US"]

    for lang in target_languages:
        listing_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/listings/{lang}"
        _, headers, list_data = api_request(listing_url, token, method="GET")
        etag = headers.get("ETag")
        listing_json = json.loads(list_data.decode("utf-8"))

        print(f"Updating recentChanges for language: {lang}")
        listing_json["recentChanges"] = notes_content

        put_headers = {
            "Content-Type": "application/json",
        }
        if etag:
            put_headers["If-Match"] = etag

        payload = json.dumps(listing_json).encode("utf-8")
        api_request(listing_url, token, method="PUT", data=payload, headers=put_headers)

    print("Release notes updated successfully.")


def commit_edit(app_id: str, edit_id: str, token: str):
    """Commit the edit session to submit for review."""
    print(f"Committing edit session {edit_id} for review...")
    commit_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/commit"
    api_request(commit_url, token, method="POST")
    print("Edit committed successfully! Upcoming version submitted for review.")


def resolve_apk_path(pattern: str) -> str:
    """Find the APK file matching the given glob pattern."""
    matches = glob.glob(pattern)
    if not matches:
        raise FileNotFoundError(f"No APK file found matching pattern: {pattern}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description="Upload APK and update release notes on Amazon Appstore")
    parser.add_argument("--client-id", required=True, help="Amazon LWA Client ID")
    parser.add_argument("--client-secret", required=True, help="Amazon LWA Client Secret")
    parser.add_argument("--app-id", required=True, help="Amazon Appstore App ID")
    parser.add_argument("--apk", required=True, help="Path or glob pattern for the release APK")
    parser.add_argument("--notes-file", default="", help="Path to release notes text file")

    args = parser.parse_args()

    apk_file = resolve_apk_path(args.apk)
    print(f"Target APK: {apk_file}")

    token = get_lwa_token(args.client_id, args.client_secret)
    edit_id = create_or_get_edit(args.app_id, token)

    upload_apk(args.app_id, edit_id, token, apk_file)

    if args.notes_file:
        update_release_notes(args.app_id, edit_id, token, args.notes_file)

    commit_edit(args.app_id, edit_id, token)
    print("Amazon Appstore upload and release notes update completed successfully.")


if __name__ == "__main__":
    main()
