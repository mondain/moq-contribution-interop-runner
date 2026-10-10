#!/usr/bin/env bash
# requirements/schema.json: the moq-lite-06 baseline validates, the optional boolean "reviewed" flag is accepted,
# and a non-boolean flag is rejected. Existing catalogs without the flag still validate.
set -euo pipefail

python3 -c 'import jsonschema' 2>/dev/null || { echo "SKIP: python3 jsonschema module required"; exit 77; }
root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
python3 - "$root_dir/requirements" <<'PY'
import copy, json, os, sys
import jsonschema

base = sys.argv[1]
schema = json.load(open(os.path.join(base, "schema.json")))
validator = jsonschema.Draft202012Validator(schema)

lite = json.load(open(os.path.join(base, "moq-lite-06.json")))
validator.validate(lite)
assert lite["draft"] == 106 and lite["complete"] is False
assert lite["requirements"], "baseline must have rows"
# Hand-classified rows drop the flag (absent means reviewed); since L2b every row is classified, so the baseline
# holds no flag at all and the schema is exercised on a copy that sets one.
assert all("reviewed" not in row for row in lite["requirements"]), "L2b classified every row"
unreviewed = 0
assert validator.is_valid(lite)
flagged = copy.deepcopy(lite)
flagged["requirements"][unreviewed]["reviewed"] = False
assert validator.is_valid(flagged)

for name in ("draft18.json", "draft21.json", "draft22.json"):
    validator.validate(json.load(open(os.path.join(base, name))))

for bad in ("no", 0, None, 1):
    broken = copy.deepcopy(lite)
    broken["requirements"][unreviewed]["reviewed"] = bad
    assert not validator.is_valid(broken), f"reviewed={bad!r} must be invalid"
extra = copy.deepcopy(lite)
extra["requirements"][0]["unknown"] = True
assert not validator.is_valid(extra)
flagless = copy.deepcopy(flagged)
del flagless["requirements"][unreviewed]["reviewed"]
assert validator.is_valid(flagless)
print("lite catalog schema passed")
PY
