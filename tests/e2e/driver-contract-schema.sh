#!/usr/bin/env bash
# adapters/contract.schema.json: the draft is the integer 18, 21 or 22 or the string "moq-lite-06".
set -euo pipefail

python3 -c 'import jsonschema' 2>/dev/null || { echo "SKIP: python3 jsonschema module required"; exit 77; }
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
python3 - "$root_dir/adapters/contract.schema.json" <<'PY'
import json, sys
import jsonschema

schema = json.load(open(sys.argv[1]))
base = {
    "schema_version": 1, "run_id": "r", "scenario_id": "s", "endpoint": "https://h/moq",
    "draft": 22, "transport": "webtransport", "namespace_hex": ["6d65646961"],
    "track_name_hex": "766964655f31", "fixture": "/f", "tls_ca": "/ca", "log_dir": "/l",
    "scenario_timeout_ms": 1000, "process_timeout_ms": 2000,
}
validator = jsonschema.Draft202012Validator(schema)
def valid(draft):
    return validator.is_valid({**base, "draft": draft})

for draft in (18, 21, 22, "moq-lite-06"):
    assert valid(draft), f"draft {draft!r} must be valid"
for draft in (106, "22", "moq-lite-05", 19, 22.5, True, None, "Moq-Lite-06"):
    assert not valid(draft), f"draft {draft!r} must be invalid"
print("driver contract schema passed")
PY
