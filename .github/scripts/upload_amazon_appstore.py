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
    }, safe=":").encode("utf-8")

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


def api_request(url: str, token: str, method: str = "GET", data: bytes = None, headers: dict = None, allow_error_codes: list = None) -> tuple:
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
        if allow_error_codes and e.code in allow_error_codes:
            return e.code, e.headers, e.read()
        error_msg = e.read().decode("utf-8", errors="replace")
        print(f"API Error [{method} {url}]: HTTP {e.code}: {error_msg}", file=sys.stderr)
        sys.exit(1)


def create_or_get_edit(app_id: str, token: str) -> str:
    """Create a new edit session (upcoming version) or reuse existing active edit session."""
    url = f"{API_BASE_URL}/{app_id}/edits"

    # First check if an edit session is already active
    print(f"Checking for existing active edit session for App ID: {app_id}...")
    status, _, data = api_request(url, token, method="GET", allow_error_codes=[400, 404])
    if status == 200:
        try:
            edit_info = json.loads(data.decode("utf-8"))
            edit_id = edit_info.get("id")
            if edit_id:
                print(f"Found active edit session with ID: {edit_id}")
                return edit_id
        except Exception:
            pass

    print(f"Creating edit session for App ID: {app_id}...")
    status, _, data = api_request(url, token, method="POST", headers={"Content-Type": "application/json"}, allow_error_codes=[409])
    if status == 200:
        try:
            edit_info = json.loads(data.decode("utf-8")) if data else {}
        except Exception:
            edit_info = {}
        edit_id = edit_info.get("id")
        print(f"Edit session created with ID: {edit_id}")
        return edit_id
    elif status == 409:
        print("Edit session already exists (409 Conflict). Fetching active edit session...")
        _, _, data = api_request(url, token, method="GET")
        try:
            edit_info = json.loads(data.decode("utf-8")) if data else {}
        except Exception:
            edit_info = {}
        edit_id = edit_info.get("id")
        print(f"Using active edit session with ID: {edit_id}")
        return edit_id
    else:
        print(f"Failed to create or get edit session: HTTP {status}", file=sys.stderr)
        sys.exit(1)


def get_existing_binaries(app_id: str, edit_id: str, token: str) -> list:
    """Retrieve existing binaries (APKs / VPKGs) and their ETags in the active edit session."""
    print(f"Checking existing binaries in edit session {edit_id}...")
    apks_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/apks"
    status, _, data = api_request(apks_url, token, method="GET", allow_error_codes=[400, 404])
    if status != 200 or not data:
        return []

    raw_text = data.decode("utf-8", errors="replace").strip()
    existing_list = []
    if raw_text:
        try:
            parsed = json.loads(raw_text)
            if isinstance(parsed, list):
                existing_list = parsed
            elif isinstance(parsed, dict):
                existing_list = parsed.get("apks", [parsed] if "id" in parsed else [])
        except Exception as e:
            print(f"Could not parse existing APKs list ({e}), treating as no existing binaries.")
            return []

    detailed_binaries = []
    for item in existing_list:
        binary_id = item.get("id")
        if not binary_id:
            continue
        single_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/apks/{binary_id}"
        s_status, s_headers, s_data = api_request(single_url, token, method="GET", allow_error_codes=[400, 404])
        etag = s_headers.get("ETag") if s_headers else None
        details = {}
        if s_status == 200 and s_data:
            try:
                details = json.loads(s_data.decode("utf-8"))
            except Exception:
                pass
        if not etag and details:
            etag = details.get("etag") or details.get("eTag")

        detailed_binaries.append({
            "id": binary_id,
            "etag": etag,
            "summary": item,
            "details": details,
        })

    print(f"Found {len(detailed_binaries)} existing binary entry(ies) in edit session.")
    return detailed_binaries


def find_matching_binary(detailed_binaries: list, file_path: str) -> dict:
    """Find an existing binary entry that corresponds to the file being uploaded (APK vs VPKG)."""
    if not detailed_binaries:
        return None

    is_vpkg = file_path.lower().endswith(".vpkg")

    # 1. Look for explicit indication in summary/details
    for entry in detailed_binaries:
        combined_text = json.dumps(entry).lower()
        if is_vpkg and ("vpkg" in combined_text or "vega" in combined_text):
            return entry
        elif not is_vpkg and ("vpkg" not in combined_text and "vega" not in combined_text):
            if ".apk" in combined_text or '"apk' in combined_text:
                return entry

    # 2. If looking for APK and only 1 binary exists that does not appear to be Vega:
    if not is_vpkg and len(detailed_binaries) == 1:
        entry = detailed_binaries[0]
        combined_text = json.dumps(entry).lower()
        if "vpkg" not in combined_text and "vega" not in combined_text:
            return entry

    # 3. If looking for VPKG and only 1 binary exists and it does appear to be Vega:
    if is_vpkg and len(detailed_binaries) == 1:
        entry = detailed_binaries[0]
        combined_text = json.dumps(entry).lower()
        if "vpkg" in combined_text or "vega" in combined_text:
            return entry

    # 4. If looking for APK in multi-binary set, return first non-Vega entry:
    if not is_vpkg:
        for entry in detailed_binaries:
            combined_text = json.dumps(entry).lower()
            if "vpkg" not in combined_text and "vega" not in combined_text:
                return entry

    return None


def upload_binary(
    app_id: str,
    edit_id: str,
    token: str,
    file_path: str,
    binary_label: str = "APK",
    existing_binaries: list = None,
    continue_on_error: bool = False,
) -> bool:
    """Upload or replace a binary file (APK or VPKG) in the active edit session."""
    file_name = os.path.basename(file_path)
    file_size = os.path.getsize(file_path)
    is_vpkg = file_path.lower().endswith(".vpkg")

    print(f"Preparing to upload {binary_label}: {file_name} ({file_size} bytes)...")

    with open(file_path, "rb") as f:
        file_bytes = f.read()

    content_type = "application/octet-stream" if is_vpkg else "application/vnd.android.package-archive"
    headers = {
        "Content-Type": content_type,
        "fileName": file_name,
    }

    matched_entry = find_matching_binary(existing_binaries or [], file_path)

    if matched_entry:
        binary_id = matched_entry["id"]
        etag = matched_entry.get("etag")
        print(f"Replacing existing {binary_label} (ID: {binary_id}, ETag: {etag or 'None'})...")
        upload_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/apks/{binary_id}/replace"
        replace_headers = dict(headers)
        if etag:
            replace_headers["If-Match"] = etag

        status, _, resp_data = api_request(
            upload_url,
            token,
            method="PUT",
            data=file_bytes,
            headers=replace_headers,
            allow_error_codes=[400, 404, 409, 412, 415, 422, 500] if continue_on_error else None,
        )
    else:
        print(f"Uploading new {binary_label} ({file_name})...")
        upload_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/apks/upload"
        status, _, resp_data = api_request(
            upload_url,
            token,
            method="POST",
            data=file_bytes,
            headers=headers,
            allow_error_codes=[400, 404, 409, 412, 415, 422, 500] if continue_on_error else None,
        )

    if status not in (200, 201, 204):
        err = resp_data.decode("utf-8", errors="replace") if resp_data else ""
        if continue_on_error:
            print(
                f"Warning: Failed to upload {binary_label} '{file_name}' to Amazon Appstore (HTTP {status}): {err}\n"
                f"You can upload '{file_name}' manually in the Amazon Developer Console under the Upcoming Version.",
                file=sys.stderr,
            )
            return False
        else:
            print(f"Error: Failed to upload {binary_label} '{file_name}' (HTTP {status}): {err}", file=sys.stderr)
            sys.exit(1)

    print(f"{binary_label} upload complete.")
    return True


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
    status, _, data = api_request(listings_url, token, method="GET", allow_error_codes=[400, 404])
    if status != 200:
        print(f"Failed to fetch listings (HTTP {status}), skipping release notes update.")
        return

    try:
        listings = json.loads(data.decode("utf-8"))
    except Exception as e:
        print(f"Failed to parse listings response: {e}, defaulting to ['en-US'].")
        listings = {}

    target_languages = []
    if isinstance(listings, dict):
        if "listings" in listings:
            listings_data = listings["listings"]
            if isinstance(listings_data, dict):
                target_languages = list(listings_data.keys())
            elif isinstance(listings_data, list):
                target_languages = [
                    l.get("language") for l in listings_data
                    if isinstance(l, dict) and "language" in l
                ]
        elif "language" in listings:
            target_languages = [listings["language"]]
        else:
            target_languages = list(listings.keys())
    elif isinstance(listings, list):
        target_languages = [
            l.get("language") for l in listings
            if isinstance(l, dict) and "language" in l
        ]

    # Filter out anything that is not a valid language code (e.g. 'listings' or non-strings)
    target_languages = [
        lang for lang in target_languages
        if lang and isinstance(lang, str) and lang.lower() != "listings" and len(lang) <= 10
    ]

    if not target_languages:
        target_languages = ["en-US"]

    print(f"Found listing language(s): {target_languages}")

    for lang in target_languages:
        listing_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/listings/{lang}"
        status, headers, list_data = api_request(listing_url, token, method="GET", allow_error_codes=[400, 404])
        if status != 200:
            print(f"Listing for language '{lang}' not found or unavailable (HTTP {status}), skipping.")
            continue
        etag = headers.get("ETag")
        try:
            listing_json = json.loads(list_data.decode("utf-8"))
        except Exception as e:
            print(f"Failed to parse listing JSON for language '{lang}': {e}, skipping.")
            continue

        print(f"Updating recentChanges for language: {lang}")
        listing_json["recentChanges"] = notes_content

        put_headers = {
            "Content-Type": "application/json",
        }
        if etag:
            put_headers["If-Match"] = etag

        payload = json.dumps(listing_json).encode("utf-8")
        put_status, _, put_data = api_request(
            listing_url, token, method="PUT", data=payload, headers=put_headers, allow_error_codes=[400, 404, 412]
        )
        if put_status in (200, 204):
            print(f"Release notes updated successfully for language: {lang}")
        else:
            print(f"Warning: Failed to update release notes for language '{lang}' (HTTP {put_status}): {put_data.decode('utf-8', errors='replace')}", file=sys.stderr)

    print("Release notes update step completed.")


def commit_edit(app_id: str, edit_id: str, token: str):
    """Commit the edit session to submit for review."""
    print(f"Retrieving current ETag for edit session {edit_id}...")
    etag = None

    # First try fetching edit by ID
    status, headers, data = api_request(
        f"{API_BASE_URL}/{app_id}/edits/{edit_id}", token, method="GET", allow_error_codes=[400, 404]
    )
    if status == 200:
        etag = headers.get("ETag")

    # If not found on specific edit URL, query the /edits endpoint
    if not etag:
        status, headers, data = api_request(
            f"{API_BASE_URL}/{app_id}/edits", token, method="GET", allow_error_codes=[400, 404]
        )
        if status == 200:
            etag = headers.get("ETag")
            if not etag:
                try:
                    edit_json = json.loads(data.decode("utf-8"))
                    etag = edit_json.get("etag") or edit_json.get("eTag")
                except Exception:
                    pass

    print(f"Committing edit session {edit_id} for review (ETag: {etag})...")
    commit_url = f"{API_BASE_URL}/{app_id}/edits/{edit_id}/commit"
    commit_headers = {}
    if etag:
        commit_headers["If-Match"] = etag

    api_request(commit_url, token, method="POST", headers=commit_headers)
    print("Edit committed successfully! Upcoming version submitted for review.")


def resolve_file_path(pattern: str, desc: str = "file") -> str:
    """Find a file matching the given glob pattern."""
    matches = glob.glob(pattern)
    if not matches:
        raise FileNotFoundError(f"No {desc} found matching pattern: {pattern}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description="Upload APK / VPKG and update release notes on Amazon Appstore")
    parser.add_argument("--client-id", required=True, help="Amazon LWA Client ID")
    parser.add_argument("--client-secret", required=True, help="Amazon LWA Client Secret")
    parser.add_argument("--app-id", required=True, help="Amazon Appstore App ID")
    parser.add_argument("--apk", required=True, help="Path or glob pattern for the release APK")
    parser.add_argument(
        "--vpkg",
        action="append",
        default=[],
        help="Path or glob pattern for the Vega OS release VPKG (can be specified multiple times)",
    )
    parser.add_argument(
        "--vpkg-continue-on-error",
        action="store_true",
        default=True,
        help="Warn and continue if Vega VPKG upload fails rather than aborting release (default: true)",
    )
    parser.add_argument("--notes-file", default="", help="Path to release notes text file")
    parser.add_argument(
        "--skip-commit",
        action="store_true",
        help="Upload binaries and update notes without committing edit session (leaves draft for Live App Testing)",
    )

    args = parser.parse_args()

    apk_file = resolve_file_path(args.apk, "APK")
    print(f"Target APK: {apk_file}")

    token = get_lwa_token(args.client_id, args.client_secret)
    edit_id = create_or_get_edit(args.app_id, token)

    # 1. Fetch current binaries attached to this edit session
    existing_binaries = get_existing_binaries(args.app_id, edit_id, token)

    # 2. Upload or replace the Android APK
    upload_binary(
        args.app_id,
        edit_id,
        token,
        apk_file,
        binary_label="APK",
        existing_binaries=existing_binaries,
        continue_on_error=False,
    )

    # 3. Upload or replace Vega OS (.vpkg) packages if provided
    if args.vpkg:
        vpkg_files = []
        for pattern in args.vpkg:
            matched = glob.glob(pattern)
            if matched:
                for m in matched:
                    if m not in vpkg_files:
                        vpkg_files.append(m)
            elif os.path.isfile(pattern) and pattern not in vpkg_files:
                vpkg_files.append(pattern)

        if not vpkg_files:
            print(f"Notice: No VPKG files found matching pattern(s): {args.vpkg}. Skipping VPKG upload.")
        else:
            for vpkg_file in vpkg_files:
                # Refresh binary list so we know about recently uploaded/replaced APK
                existing_binaries = get_existing_binaries(args.app_id, edit_id, token)
                upload_binary(
                    args.app_id,
                    edit_id,
                    token,
                    vpkg_file,
                    binary_label="Vega VPKG",
                    existing_binaries=existing_binaries,
                    continue_on_error=args.vpkg_continue_on_error,
                )

    # 4. Update release notes if provided
    if args.notes_file:
        update_release_notes(args.app_id, edit_id, token, args.notes_file)

    # 5. Commit or leave as draft
    if args.skip_commit:
        print(f"Skipping edit commit (--skip-commit specified). Edit session {edit_id} remains active as a draft.")
        print("You can now open the Amazon Developer Console -> Live App Testing -> Start a new test -> 'Copy from upcoming version'.")
    else:
        commit_edit(args.app_id, edit_id, token)

    print("Amazon Appstore upload and release notes update completed successfully.")


if __name__ == "__main__":
    main()
