#!/usr/bin/env python3
"""Python variant of adapter.sh for the fictional publisher "acme-pub".

Same contract, same behavior: read the JSON request, translate it into the
publisher's command line, exec the publisher. No shell is involved.
"""
import json
import os
import re
import sys


def fail(message):
    print(f"acme adapter: {message}", file=sys.stderr)
    sys.exit(64)


def name_from_hex(value):
    if not re.fullmatch(r"(?:2[1-9a-f]|[3-6][0-9a-f]|7[0-9a-e])+", value):
        fail("namespace or track name is not printable ASCII")
    text = bytes.fromhex(value).decode("ascii")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", text):
        fail("namespace or track name is not a plain name")
    return text


if os.environ.get("MOQ_INTEROP_DRIVER_CONTRACT_VERSION") != "1":
    fail("unsupported driver contract version")
request_file = os.environ.get("MOQ_INTEROP_DRIVER_REQUEST_FILE", "")
# ACME_PUB_BIN overrides the default: an executable named acme-pub next to this
# script, which is where a Docker Compose setup puts it (/opt/publisher).
publisher = os.environ.get(
    "ACME_PUB_BIN", os.path.join(os.path.dirname(os.path.abspath(__file__)), "acme-pub"))
if not request_file or not os.access(request_file, os.R_OK):
    fail("request file is unavailable")
if not os.access(publisher, os.X_OK):
    fail(f"publisher is not executable: {publisher}")

with open(request_file, encoding="utf-8") as handle:
    request = json.load(handle)

try:
    if request["schema_version"] != 1 or request["draft"] not in (18, 21):
        fail("unsupported or malformed request")
    transport, endpoint = request["transport"], request["endpoint"]
    if transport == "native_quic" and endpoint.startswith("moqt://"):
        acme_transport = "quic"
    elif transport == "webtransport" and endpoint.startswith("https://"):
        acme_transport = "wt"
    else:
        fail("transport and endpoint scheme do not match")
    fixture, ca_cert = request["fixture"], request["tls_ca"]
    if not (os.access(fixture, os.R_OK) and os.access(ca_cert, os.R_OK)):
        fail("fixture or TLS CA is unreadable")
    namespace = "/".join(name_from_hex(field) for field in request["namespace_hex"])
    track = name_from_hex(request["track_name_hex"])
    seconds = (int(request["scenario_timeout_ms"]) + 999) // 1000
    scenario_id = request["scenario_id"]
except (KeyError, TypeError, ValueError):
    fail("unsupported or malformed request")

announce = {
    "publish-track-under-single-period-namespace",
    "application-publish-track-in-session-namespace",
    "publish-distinct-content-tracks-in-same-scope",
    "d21-publisher-request-stream-placement",
}
mode = "announce" if scenario_id in announce else "serve"
args = [publisher, "--input", fixture, "--connect", endpoint,
        "--transport", acme_transport, "--draft", str(request["draft"]),
        "--namespace", namespace, "--track", track, "--ca", ca_cert,
        "--exit-after", str(seconds), "--mode", mode]
os.execv(publisher, args)
