import json, sys, urllib.request

# Optional: pass a trunks JSON file as the first arg, else default.
FILE = sys.argv[1] if len(sys.argv) > 1 else "phone_trunks.json"
ADD_PROVIDER_URL = "http://localhost:8086/add-provider"
ADD_DID_URL      = "http://localhost:8086/add-did"

ANSWER_WEBHOOK_URL = "https://api.vocallabs.ai/sip-answer-webhook"

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
    trunk_name = t.get("trunk_name") or t.get("username")

    provider_payload = {
        "provider_name": t.get("provider"),
        "username":      t.get("username"),
        "password":      t.get("password"),
        "domain":        t.get("ip"),
        "port":          int(t.get("port")),
        "registration":  bool(t.get("registration", t.get("register", False))),
    }

    try:
        status, body = post_json(ADD_PROVIDER_URL, provider_payload)
        print("✅ provider", trunk_name, "→", status, body)
    except Exception as e:
        print("❌ provider", trunk_name, "→", e)

    did_payload = {
        "trunk_name":         t.get("trunk_name"),
        "did":                t.get("caller_id"),
        "provider_id":        t.get("provider"),
        "answer_webhook_url": t.get("answer_webhook_url", ANSWER_WEBHOOK_URL),
    }

    try:
        status, body = post_json(ADD_DID_URL, did_payload)
        print("✅ did", trunk_name, t.get("caller_id"), "→", status, body)
    except Exception as e:
        print("❌ did", trunk_name, t.get("caller_id"), "→", e)

    # db_url = "https://arc.vocallabs.ai/v1/graphql"
    # db_x_hasura_secret = "d33a3c68fa9185529b40f27a6c0c52d88295c053d74ec60e5c0cec74568abe80"

    # query = """
    # mutation InsertPhoneNumber($object: vocallabs_phone_numbers_insert_input!) {
    #   insert_vocallabs_phone_numbers(objects: [$object]) {
    #     affected_rows
    #   }
    # }
    # """

    # variables = {
    #     "object": {
    #         "ip": t.get("ip"),
    #         "port": str(t.get("port")),
    #         "username": t.get("username"),
    #         "password": t.get("password"),
    #         "prefix": t.get("prefix"),
    #         "did": t.get("caller_id"),
    #         "provider": t.get("provider"),
    #         "country": t.get("country"),
    #         "trunk_name": t.get("trunk_name")
    #     }
    # }

    # gql_payload = {
    #     "query": query,
    #     "variables": variables
    # }

    # gql_data = json.dumps(gql_payload).encode("utf-8")

    # gql_req = urllib.request.Request(
    #     db_url,
    #     data=gql_data,
    #     method="POST",
    #     headers={
    #         "Content-Type": "application/json",
    #         "User-Agent": "Mozilla/5.0",
    #         "x-hasura-admin-secret": db_x_hasura_secret,
    #         "x-hasura-role": "admin"
    #     }
    # )

    # try:
    #     with urllib.request.urlopen(gql_req, timeout=30) as r:
    #         print("✅ DB →", r.read().decode())
    # except urllib.error.HTTPError as e:
    #     print("❌ DB insert →", e.code, e.reason)
    #     print(e.read().decode())