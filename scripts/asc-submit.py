#!/usr/bin/env python3
"""
asc-submit.py - send an uploaded Helldrop build to App Store review, without the
web UI. Picks up where scripts/ios-appstore.sh stops: waits until Apple finishes
processing the build, attaches it to the App Store version (created if missing),
declares export compliance, optionally replaces the App Review notes, and
submits. A version Apple rejected is resubmitted in its existing submission.

Requires:
  - ASC_KEY_ID + ASC_ISSUER_ID env vars
  - ~/.appstoreconnect/private_keys/AuthKey_<KEY_ID>.p8
  - /usr/bin/python3 (Homebrew python has no jwt)

Usage:
  scripts/asc-submit.py <version> <build>        e.g. scripts/asc-submit.py 1.0 2

Environment:
  ASC_WHATS_NEW          public release note (ignored for the first version)
  ASC_REVIEW_NOTES_FILE  text file that replaces App Review Information > Notes
                         (4000 characters at most)
"""
import datetime as dt
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import jwt

BUNDLE_ID = "app.ximg.helldrop"
PLATFORM = "IOS"
RELEASE_TYPE = "AFTER_APPROVAL"  # vs "MANUAL" or "SCHEDULED"
API_BASE = "https://api.appstoreconnect.apple.com"


def make_token() -> str:
    key_id = os.environ["ASC_KEY_ID"]
    issuer_id = os.environ["ASC_ISSUER_ID"]
    key_path = pathlib.Path.home() / ".appstoreconnect" / "private_keys" / f"AuthKey_{key_id}.p8"
    private_key = key_path.read_text()
    payload = {
        "iss": issuer_id,
        "iat": int(dt.datetime.now(dt.timezone.utc).timestamp()),
        "exp": int(dt.datetime.now(dt.timezone.utc).timestamp()) + 20 * 60,
        "aud": "appstoreconnect-v1",
    }
    return jwt.encode(payload, private_key, algorithm="ES256", headers={"kid": key_id})


def req(method: str, path: str, *, token: str, body=None, query=None):
    url = API_BASE + path
    if query:
        url += "?" + urllib.parse.urlencode(query, doseq=True)
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Authorization", f"Bearer {token}")
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request) as resp:
            text = resp.read().decode()
            return json.loads(text) if text else {}
    except urllib.error.HTTPError as e:
        err_body = e.read().decode()
        print(f"\n{method} {url} -> {e.code}\n{err_body}", file=sys.stderr)
        raise


def find_app(token, bundle_id):
    j = req("GET", "/v1/apps", token=token, query={"filter[bundleId]": bundle_id})
    if not j.get("data"):
        sys.exit(f"No app with bundleId={bundle_id}")
    return j["data"][0]


def wait_for_build(token, app_id, version, build_number, timeout_min=45, interval=30):
    """Poll until build <build_number> of <version> reaches processingState=VALID."""
    deadline = time.time() + timeout_min * 60
    last_state = None
    while time.time() < deadline:
        j = req(
            "GET",
            "/v1/builds",
            token=token,
            query={
                "filter[app]": app_id,
                "filter[preReleaseVersion.version]": version,
                "filter[version]": build_number,
                "sort": "-uploadedDate",
                "limit": "5",
            },
        )
        builds = j.get("data", [])
        if builds:
            b = builds[0]
            state = b["attributes"]["processingState"]
            if state != last_state:
                print(f"  build {version} ({build_number}) state: {state}")
                last_state = state
            if state == "VALID":
                return b
            if state == "FAILED":
                sys.exit(f"Build processing FAILED for {version} ({build_number})")
        else:
            if last_state != "missing":
                print(f"  no build {version} ({build_number}) visible yet, waiting...")
                last_state = "missing"
        time.sleep(interval)
    sys.exit(f"Timed out waiting for build {version} ({build_number}) to process")


def find_prior_version(token, app_id, exclude_version):
    """Last accepted (or any) App Store version we can copy localizations from."""
    j = req(
        "GET",
        f"/v1/apps/{app_id}/appStoreVersions",
        token=token,
        query={
            "filter[platform]": PLATFORM,
            "limit": "20",
        },
    )
    # API doesn't allow sort on this relationship endpoint, so newest-first
    # sort client-side via createdDate.
    candidates = sorted(
        [v for v in j.get("data", []) if v["attributes"]["versionString"] != exclude_version],
        key=lambda v: v["attributes"].get("createdDate", ""),
        reverse=True,
    )
    return candidates[0] if candidates else None


def get_or_create_version(token, app_id, version, build_id):
    j = req(
        "GET",
        f"/v1/apps/{app_id}/appStoreVersions",
        token=token,
        query={
            "filter[platform]": PLATFORM,
            "filter[versionString]": version,
        },
    )
    if j.get("data"):
        ver = j["data"][0]
        print(f"  reusing existing App Store version record {version} ({ver['id']})")
    else:
        body = {
            "data": {
                "type": "appStoreVersions",
                "attributes": {
                    "platform": PLATFORM,
                    "versionString": version,
                    "releaseType": RELEASE_TYPE,
                },
                "relationships": {
                    "app": {"data": {"type": "apps", "id": app_id}},
                    "build": {"data": {"type": "builds", "id": build_id}},
                },
            }
        }
        created = req("POST", "/v1/appStoreVersions", token=token, body=body)
        ver = created["data"]
        print(f"  created App Store version {version} ({ver['id']})")
    return ver


def attach_build(token, version_id, build_id):
    """Make sure the build is the one attached to the version."""
    body = {"data": {"type": "builds", "id": build_id}}
    req("PATCH", f"/v1/appStoreVersions/{version_id}/relationships/build", token=token, body=body)
    print("  build attached")


def copy_localizations(token, src_version_id, dst_version_id, whats_new):
    """Mirror localized text fields from a prior version into the new one."""
    src = req(
        "GET",
        f"/v1/appStoreVersions/{src_version_id}/appStoreVersionLocalizations",
        token=token,
        query={"limit": "20"},
    ).get("data", [])
    dst = req(
        "GET",
        f"/v1/appStoreVersions/{dst_version_id}/appStoreVersionLocalizations",
        token=token,
        query={"limit": "20"},
    ).get("data", [])
    dst_by_locale = {d["attributes"]["locale"]: d for d in dst}

    fields = [
        "description",
        "keywords",
        "marketingUrl",
        "promotionalText",
        "supportUrl",
    ]
    # ASC_DESCRIPTION_FILE replaces the en-US description instead of copying the
    # prior version's (Apple only lets a description change ride a new version).
    desc_file = os.environ.get("ASC_DESCRIPTION_FILE")
    for s in src:
        locale = s["attributes"]["locale"]
        attrs = {f: s["attributes"].get(f) for f in fields if s["attributes"].get(f) is not None}
        if desc_file and locale == "en-US":
            attrs["description"] = pathlib.Path(desc_file).read_text().strip()
        attrs["whatsNew"] = whats_new
        if locale in dst_by_locale:
            d = dst_by_locale[locale]
            req(
                "PATCH",
                f"/v1/appStoreVersionLocalizations/{d['id']}",
                token=token,
                body={
                    "data": {
                        "type": "appStoreVersionLocalizations",
                        "id": d["id"],
                        "attributes": attrs,
                    }
                },
            )
            print(f"  updated localization {locale}")
        else:
            req(
                "POST",
                "/v1/appStoreVersionLocalizations",
                token=token,
                body={
                    "data": {
                        "type": "appStoreVersionLocalizations",
                        "attributes": {"locale": locale, **attrs},
                        "relationships": {
                            "appStoreVersion": {
                                "data": {"type": "appStoreVersions", "id": dst_version_id}
                            }
                        },
                    }
                },
            )
            print(f"  created localization {locale}")


def set_export_compliance(token, build_id):
    body = {
        "data": {
            "type": "builds",
            "id": build_id,
            "attributes": {"usesNonExemptEncryption": False},
        }
    }
    try:
        req("PATCH", f"/v1/builds/{build_id}", token=token, body=body)
    except urllib.error.HTTPError as e:
        # 409 = already declared (ITSAppUsesNonExemptEncryption in Info.plist)
        if e.code != 409:
            raise
        print("  export compliance: already declared on build")
        return
    print("  export compliance: no non-exempt encryption")


def set_review_notes(token, version_id, notes):
    """Replace App Review Information > Notes on the version."""
    if len(notes) > 4000:
        sys.exit(f"review notes are {len(notes)} characters; Apple allows 4000")
    detail = req("GET", f"/v1/appStoreVersions/{version_id}/appStoreReviewDetail", token=token).get("data")
    if not detail:
        sys.exit("the version has no App Review Information yet; fill in the contact once in App Store Connect")
    req(
        "PATCH",
        f"/v1/appStoreReviewDetails/{detail['id']}",
        token=token,
        body={"data": {"type": "appStoreReviewDetails", "id": detail["id"],
                       "attributes": {"notes": notes}}},
    )
    print(f"  review notes replaced ({len(notes)} characters)")


def submit_for_review(token, app_id, version_id):
    """Submit the version. A submission Apple sent back (UNRESOLVED_ISSUES)
    still holds the version: resubmit that one, as "Resubmit to App Review"
    does in the web UI. Otherwise create a reviewSubmission (or reuse an open,
    unsubmitted one left by an earlier failed run) and add the version to it."""
    rejected = req(
        "GET",
        f"/v1/apps/{app_id}/reviewSubmissions",
        token=token,
        query={"filter[state]": "UNRESOLVED_ISSUES", "filter[platform]": PLATFORM},
    ).get("data", [])
    if rejected:
        rs_id = rejected[0]["id"]
        print(f"  resubmitting rejected reviewSubmission {rs_id}")
        try:
            req(
                "PATCH",
                f"/v1/reviewSubmissions/{rs_id}",
                token=token,
                body={"data": {"type": "reviewSubmissions", "id": rs_id,
                               "attributes": {"submitted": True}}},
            )
        except urllib.error.HTTPError as e:
            if e.code != 409:
                raise
            # Seen 2026-11-24: after a rejection the API answers "Version is not
            # ready to be submitted yet" however long you wait. The build and
            # the notes above are saved; the last step exists only in the web UI.
            sys.exit(
                "\nThe build and notes are saved, but Apple's API will not resubmit a rejected version.\n"
                "Finish in App Store Connect: open the version page, click 'Update Review',\n"
                "then 'Resubmit to App Review' on the submission page:\n"
                f"  https://appstoreconnect.apple.com/apps/{app_id}/distribution/ios/version/inflight"
            )
        print("  RESUBMITTED for review")
        return
    open_rs = req(
        "GET",
        f"/v1/apps/{app_id}/reviewSubmissions",
        token=token,
        query={"filter[state]": "READY_FOR_REVIEW", "filter[platform]": PLATFORM},
    ).get("data", [])
    if open_rs:
        rs_id = open_rs[0]["id"]
        print(f"  reusing open reviewSubmission {rs_id}")
    else:
        rs = req(
            "POST",
            "/v1/reviewSubmissions",
            token=token,
            body={
                "data": {
                    "type": "reviewSubmissions",
                    "attributes": {"platform": PLATFORM},
                    "relationships": {"app": {"data": {"type": "apps", "id": app_id}}},
                }
            },
        )
        rs_id = rs["data"]["id"]
        print(f"  created reviewSubmission {rs_id}")
    req(
        "POST",
        "/v1/reviewSubmissionItems",
        token=token,
        body={
            "data": {
                "type": "reviewSubmissionItems",
                "relationships": {
                    "reviewSubmission": {
                        "data": {"type": "reviewSubmissions", "id": rs_id}
                    },
                    "appStoreVersion": {
                        "data": {"type": "appStoreVersions", "id": version_id}
                    },
                },
            }
        },
    )
    print("  added version as a submission item")
    req(
        "PATCH",
        f"/v1/reviewSubmissions/{rs_id}",
        token=token,
        body={
            "data": {
                "type": "reviewSubmissions",
                "id": rs_id,
                "attributes": {"submitted": True},
            }
        },
    )
    print(f"  SUBMITTED for review: https://appstoreconnect.apple.com")


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: scripts/asc-submit.py <version> <build>, e.g. 1.0 2")
    version, build_number = sys.argv[1], sys.argv[2]
    print(f"==> Submitting iOS {version} ({build_number}) for App Store review")

    token = make_token()
    app = find_app(token, BUNDLE_ID)
    app_id = app["id"]
    print(f"  app: {app['attributes']['name']} ({BUNDLE_ID}) -> {app_id}")

    print(f"==> Waiting for build {version} ({build_number}) to finish processing")
    build = wait_for_build(token, app_id, version, build_number)
    build_id = build["id"]
    print(f"  build ready: {build_id}")
    token = make_token()  # the wait can outlast a 20-minute token

    prior = find_prior_version(token, app_id, version)

    print("==> Attaching the build to the App Store version")
    ver = get_or_create_version(token, app_id, version, build_id)
    version_id = ver["id"]
    attach_build(token, version_id, build_id)

    set_export_compliance(token, build_id)

    if prior:
        whats_new = os.environ.get("ASC_WHATS_NEW") or sys.exit("set ASC_WHATS_NEW (the public release note)")
        print(f"==> Copying the listing from {prior['attributes']['versionString']} + setting What's New")
        copy_localizations(token, prior["id"], version_id, whats_new)

    notes_file = os.environ.get("ASC_REVIEW_NOTES_FILE")
    if notes_file:
        set_review_notes(token, version_id, pathlib.Path(notes_file).read_text().strip())

    print("==> Submitting for review")
    submit_for_review(token, app_id, version_id)
    print("DONE.")


if __name__ == "__main__":
    main()
