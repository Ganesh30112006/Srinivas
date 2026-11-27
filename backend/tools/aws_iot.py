"""Sets up AWS IoT Core for the recorders and the backend bridge.

  python -m tools.aws_iot backend                 # policy + certificate for app.iot.main
  python -m tools.aws_iot device <device_uuid>    # thing + certificate for one recorder
  python -m tools.aws_iot endpoint                # prints the account's IoT data endpoint

Certificates and keys are written to backend/secrets/iot/<name>/ (git-ignored) and are never
printed. Re-running is safe: existing policies are updated, and a thing that already has a
certificate on disk is left alone. Uses the AWS credentials and region of the current shell.

Each recorder's thing name is its device UUID (from `python -m tools.register_device`), and its
MQTT client id must be the same; the device policy uses that to limit it to its own topics.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import boto3
from botocore.exceptions import ClientError

SECRETS = Path(__file__).resolve().parent.parent / "secrets" / "iot"
DEVICE_POLICY = "lc-device-policy"
BACKEND_POLICY = "lc-backend-policy"
BACKEND_THING = "lc-backend"
PREFIX = "lc"


def _arn(region: str, account: str, kind: str, name: str) -> str:
    return f"arn:aws:iot:{region}:{account}:{kind}/{name}"


def device_policy(region: str, account: str) -> dict[str, Any]:
    thing = "${iot:Connection.Thing.ThingName}"
    return {
        "Version": "2012-10-17",
        "Statement": [
            {
                "Effect": "Allow",
                "Action": "iot:Connect",
                "Resource": _arn(region, account, "client", thing),
                "Condition": {"Bool": {"iot:Connection.Thing.IsAttached": "true"}},
            },
            {
                "Effect": "Allow",
                "Action": "iot:Publish",
                "Resource": _arn(region, account, "topic", f"{PREFIX}/{thing}/up"),
            },
            {
                "Effect": "Allow",
                "Action": "iot:Subscribe",
                "Resource": _arn(region, account, "topicfilter", f"{PREFIX}/{thing}/down"),
            },
            {
                "Effect": "Allow",
                "Action": "iot:Receive",
                "Resource": _arn(region, account, "topic", f"{PREFIX}/{thing}/down"),
            },
        ],
    }


def backend_policy(region: str, account: str) -> dict[str, Any]:
    return {
        "Version": "2012-10-17",
        "Statement": [
            {
                "Effect": "Allow",
                "Action": "iot:Connect",
                "Resource": _arn(region, account, "client", f"{BACKEND_THING}*"),
            },
            {
                "Effect": "Allow",
                "Action": "iot:Subscribe",
                "Resource": _arn(region, account, "topicfilter", f"{PREFIX}/+/up"),
            },
            {
                "Effect": "Allow",
                "Action": "iot:Receive",
                "Resource": _arn(region, account, "topic", f"{PREFIX}/*/up"),
            },
            {
                "Effect": "Allow",
                "Action": "iot:Publish",
                "Resource": _arn(region, account, "topic", f"{PREFIX}/*/down"),
            },
        ],
    }


def ensure_policy(iot: Any, name: str, document: dict[str, Any]) -> None:
    text = json.dumps(document)
    try:
        iot.create_policy(policyName=name, policyDocument=text)
        print(f"created policy {name}")
        return
    except ClientError as exc:
        if exc.response["Error"]["Code"] != "ResourceAlreadyExistsException":
            raise
    current = iot.get_policy(policyName=name)
    if json.loads(current["policyDocument"]) == document:
        return
    # A policy keeps at most 5 versions: drop the oldest non-default one first.
    versions = iot.list_policy_versions(policyName=name)["policyVersions"]
    old = sorted((v for v in versions if not v["isDefaultVersion"]), key=lambda v: int(v["versionId"]))
    if len(versions) >= 5 and old:
        iot.delete_policy_version(policyName=name, policyVersionId=old[0]["versionId"])
    iot.create_policy_version(policyName=name, policyDocument=text, setAsDefault=True)
    print(f"updated policy {name}")


def ensure_thing_with_cert(iot: Any, thing: str, policy: str) -> Path:
    out = SECRETS / thing
    cert_file, key_file = out / "cert.pem", out / "private.key"
    try:
        iot.create_thing(thingName=thing)
        print(f"created thing {thing}")
    except ClientError as exc:
        if exc.response["Error"]["Code"] != "ResourceAlreadyExistsException":
            raise
    if cert_file.is_file() and key_file.is_file():
        print(f"certificate for {thing} already in {out}; keeping it")
        return out
    created = iot.create_keys_and_certificate(setAsActive=True)
    out.mkdir(parents=True, exist_ok=True)
    cert_file.write_text(created["certificatePem"])
    key_file.write_text(created["keyPair"]["PrivateKey"])
    (out / "certificate_arn.txt").write_text(created["certificateArn"] + "\n")
    iot.attach_policy(policyName=policy, target=created["certificateArn"])
    iot.attach_thing_principal(thingName=thing, principal=created["certificateArn"])
    print(f"created certificate for {thing} in {out}")
    return out


def endpoint(iot: Any) -> str:
    return str(iot.describe_endpoint(endpointType="iot:Data-ATS")["endpointAddress"])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("backend")
    dev = sub.add_parser("device")
    dev.add_argument("device_id")
    sub.add_parser("endpoint")
    args = parser.parse_args()

    session = boto3.session.Session()
    region = session.region_name or "ap-south-1"
    iot = session.client("iot", region_name=region)
    account = session.client("sts").get_caller_identity()["Account"]
    host = endpoint(iot)
    if args.cmd == "endpoint":
        print(host)
        return 0
    if args.cmd == "backend":
        ensure_policy(iot, BACKEND_POLICY, backend_policy(region, account))
        out = ensure_thing_with_cert(iot, BACKEND_THING, BACKEND_POLICY)
        print("\nAdd to backend/.env:")
        print(f"  IOT_ENDPOINT={host}")
        print(f"  IOT_CERT_FILE={out / 'cert.pem'}")
        print(f"  IOT_KEY_FILE={out / 'private.key'}")
        return 0
    import uuid

    try:
        thing = str(uuid.UUID(args.device_id))
    except ValueError:
        print("error: device_id must be the device UUID from tools.register_device", file=sys.stderr)
        return 1
    ensure_policy(iot, DEVICE_POLICY, device_policy(region, account))
    out = ensure_thing_with_cert(iot, thing, DEVICE_POLICY)
    print("\nProvision the recorder with:")
    print(f"  python firmware/tools/provision.py --port COM3 --device-id {thing} --token <token> \\")
    print(f"      --iot-endpoint {host} --cert {out / 'cert.pem'} --key {out / 'private.key'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
