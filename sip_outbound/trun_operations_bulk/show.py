import json, urllib.request

FILE = "phone_trunks.json"
URL  = "http://localhost:8086/show-accounts"

req = urllib.request.Request(URL, method="GET",
                            headers={"Content-Type": "application/json"})
try:
    with urllib.request.urlopen(req, timeout=30) as r:
        print("✅", r.status, r.read().decode())
except Exception as e:
    print("❌", "→", e)