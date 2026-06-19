import json, urllib.request

FILE = "to_be_deleted.json"
REMOVE_PROVIDER_URL = "http://localhost:8086/remove-provider"
REMOVE_DID_URL      = "http://localhost:8086/remove-did"

# Each entry in to_be_deleted.json must declare what to remove:
#   {"type": "provider", "trunk_name": "..."}              -> drops provider AND all its DIDs (cascade)
#   {"type": "did",      "trunk_name": "<did_row_id>"}     -> drops only that DID, provider stays
# For type=did, "trunk_name" carries the ps_provider_dids.id (the DID row's PK);
# the API receives it in the "provider_id" body field per current spec.

trunks = json.load(open(FILE, "r", encoding="utf-8"))


def post_json(url, payload):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status, r.read().decode()


for t in trunks:
    trunk_name = t.get("trunk_name")
    if not trunk_name:
        print("❌ skipping entry without trunk_name:", t)
        continue

    kind = (t.get("type") or "").strip().lower()
    if kind not in ("provider", "did"):
        print(f"❌ skipping {trunk_name}: \"type\" must be \"provider\" or \"did\" (got {t.get('type')!r})")
        continue

    if kind == "did":
        try:
            status, body = post_json(REMOVE_DID_URL, {"provider_id": trunk_name})
            print("✅ did", trunk_name, "→", status, body)
        except Exception as e:
            print("❌ did", trunk_name, "→", e)
        continue

    # kind == "provider": cascade-deletes all DIDs under this provider.
    try:
        status, body = post_json(REMOVE_PROVIDER_URL, {"trunk_name": trunk_name})
        print("✅ provider (+all dids)", trunk_name, "→", status, body)
    except Exception as e:
        print("❌ provider", trunk_name, "→", e)
