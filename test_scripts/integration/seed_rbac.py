#!/usr/bin/env python3
import json
import urllib.request
import urllib.error
import ssl
import sys

# Standard default IDs for users in fake OWSEC service
USER_ID_A = "19232181-669f-42b1-bc5f-d505c04237ba"
USER_ID_B = "99b59972-2f76-44d3-ad05-aa93ebab6017"
USER_ID_C = "c66fdb8c-6894-4fe9-aae5-86e8f0f2ff75"
USER_ID_D = "138087ea-54f3-4972-bf1f-53463fba40e4"
USER_ID_CSR_A = "f917c90f-1ee6-4df2-8a24-cfb9f7caab71"

OWPROV_URL = "https://openwifi.wlan.local:16005/api/v1"
MANAGEMENT_RESOURCES = ["entity", "venue", "operator", "inventory", "configuration", "managementPolicy", "managementRole"]

# Unverified SSL Context for self-signed certificates
ctx = ssl._create_unverified_context()

def request(method, path, body=None):
    url = f"{OWPROV_URL}{path}"
    headers = {
        "Authorization": "Bearer root-token",
        "Accept": "application/json",
        "Content-Type": "application/json"
    }
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, context=ctx) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8")
        print(f"HTTP Error {e.code} on {method} {path}: {err_body}", file=sys.stderr)
        raise e
    except Exception as e:
        print(f"Error on {method} {path}: {e}", file=sys.stderr)
        raise e

def main():
    print("Seeding OWProv hierarchy...")

    # 0. Get the default operator ID
    print("Fetching default operator...")
    ops = request("GET", "/operator")
    default_op_id = ""
    for op in ops.get("operators", []):
        if "Default" in op.get("name", ""):
            default_op_id = op["id"]
            break
    if not default_op_id:
        print("Default operator not found, using fallback", file=sys.stderr)
        default_op_id = "9045322f-ecd7-4be3-bb0a-ad624ee276ab"
    print(f"-> Default Operator: {default_op_id}")

    # 1. Create operators A and B under root entity (0000-0000-0000)
    print("Creating Operator A...")
    op_a = request("POST", "/operator/00000000-0000-0000-0000-000000000000", {
        "name": "Operator A",
        "description": "Operator A description",
        "registrationId": "operator-a",
        "entityId": "0000-0000-0000"
    })
    op_a_id = op_a["id"]
    ent_a_id = op_a["entityId"]
    print(f"-> Operator A: {op_a_id}, Entity A: {ent_a_id}")

    print("Creating Operator B...")
    op_b = request("POST", "/operator/00000000-0000-0000-0000-000000000000", {
        "name": "Operator B",
        "description": "Operator B description",
        "registrationId": "operator-b",
        "entityId": "0000-0000-0000"
    })
    op_b_id = op_b["id"]
    ent_b_id = op_b["entityId"]
    print(f"-> Operator B: {op_b_id}, Entity B: {ent_b_id}")

    # 2. Create operators C and D under Entity A
    print("Creating Operator C under Entity A...")
    op_c = request("POST", "/operator/00000000-0000-0000-0000-000000000000", {
        "name": "Operator C",
        "description": "Operator C description",
        "registrationId": "operator-c",
        "entityId": ent_a_id
    })
    op_c_id = op_c["id"]
    ent_c_id = op_c["entityId"]
    print(f"-> Operator C: {op_c_id}, Entity C: {ent_c_id}")

    print("Creating Operator D under Entity A...")
    op_d = request("POST", "/operator/00000000-0000-0000-0000-000000000000", {
        "name": "Operator D",
        "description": "Operator D description",
        "registrationId": "operator-d",
        "entityId": ent_a_id
    })
    op_d_id = op_d["id"]
    ent_d_id = op_d["entityId"]
    print(f"-> Operator D: {op_d_id}, Entity D: {ent_d_id}")

    # 3. Create management policies
    print("Creating Management Policies...")
    def make_policy(name, entity_id, user_id, access=None):
        if access is None:
            access = ["READ", "LIST", "CREATE", "UPDATE", "MODIFY", "DELETE"]
        scope = json.dumps({
            "type": "entity",
            "entityId": entity_id,
            "includeVenues": True,
            "includeChildEntities": True,
        }, separators=(",", ":"))
        return request("POST", "/managementPolicy/00000000-0000-0000-0000-000000000000", {
            "entity": entity_id,
            "name": name,
            "entries": [
                {
                    "users": [user_id],
                    "resources": [resource],
                    "access": access,
                    "policy": scope
                }
                for resource in MANAGEMENT_RESOURCES
            ]
        })

    pol_a = make_policy("policy-A", ent_a_id, USER_ID_A)
    pol_b = make_policy("policy-B", ent_b_id, USER_ID_B)
    pol_c = make_policy("policy-C", ent_c_id, USER_ID_C)
    pol_d = make_policy("policy-D", ent_d_id, USER_ID_D)
    pol_csr_a = make_policy("policy-CSR-A", ent_a_id, USER_ID_CSR_A, ["READ", "LIST"])
    
    pol_a_id = pol_a["id"]
    pol_b_id = pol_b["id"]
    pol_c_id = pol_c["id"]
    pol_d_id = pol_d["id"]
    pol_csr_a_id = pol_csr_a["id"]
    print(f"-> Policies: A={pol_a_id}, B={pol_b_id}, C={pol_c_id}, D={pol_d_id}, CSR_A={pol_csr_a_id}")

    # 4. Create management roles
    print("Creating Management Roles...")
    def make_role(name, entity_id, policy_id, user_id):
        return request("POST", "/managementRole/00000000-0000-0000-0000-000000000000", {
            "entity": entity_id,
            "name": name,
            "managementPolicy": policy_id,
            "users": [user_id]
        })

    role_a = make_role("role-A", ent_a_id, pol_a_id, USER_ID_A)
    role_b = make_role("role-B", ent_b_id, pol_b_id, USER_ID_B)
    role_c = make_role("role-C", ent_c_id, pol_c_id, USER_ID_C)
    role_d = make_role("role-D", ent_d_id, pol_d_id, USER_ID_D)
    role_csr_a = make_role("role-CSR-A", ent_a_id, pol_csr_a_id, USER_ID_CSR_A)

    role_a_id = role_a["id"]
    role_b_id = role_b["id"]
    role_c_id = role_c["id"]
    role_d_id = role_d["id"]
    role_csr_a_id = role_csr_a["id"]
    print(f"-> Roles: A={role_a_id}, B={role_b_id}, C={role_c_id}, D={role_d_id}, CSR_A={role_csr_a_id}")

    # Print out environment variables to source
    env_vars = {
        "OWPROV_OPERATOR_DEFAULT": default_op_id,
        "OWPROV_ENTITY_A": ent_a_id,
        "OWPROV_ENTITY_B": ent_b_id,
        "OWPROV_ENTITY_C": ent_c_id,
        "OWPROV_ENTITY_D": ent_d_id,
        "OWPROV_OPERATOR_A": op_a_id,
        "OWPROV_OPERATOR_B": op_b_id,
        "OWPROV_OPERATOR_C": op_c_id,
        "OWPROV_OPERATOR_D": op_d_id,
        "OWPROV_POLICY_A": pol_a_id,
        "OWPROV_POLICY_B": pol_b_id,
        "OWPROV_POLICY_C": pol_c_id,
        "OWPROV_POLICY_D": pol_d_id,
        "OWPROV_POLICY_CSR_A": pol_csr_a_id,
        "OWPROV_ROLE_A": role_a_id,
        "OWPROV_ROLE_B": role_b_id,
        "OWPROV_ROLE_C": role_c_id,
        "OWPROV_ROLE_D": role_d_id,
        "OWPROV_ROLE_CSR_A": role_csr_a_id,
    }

    print("\nSUCCESS! Seeded variables:")
    for k, v in env_vars.items():
        print(f"export {k}=\"{v}\"")

    with open("seeded_env.sh", "w") as f:
        for k, v in env_vars.items():
            f.write(f"export {k}=\"{v}\"\n")
    print("\nSaved variables to test_scripts/integration/seeded_env.sh")

if __name__ == "__main__":
    main()
