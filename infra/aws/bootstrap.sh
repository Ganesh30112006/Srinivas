#!/usr/bin/env bash
# First boot of the AWS server (run by the CloudFormation user data in infra/aws/smartclass.yaml).
#
#   SITE_ADDRESS=<ip>.sslip.io AWS_REGION=ap-south-1 bash infra/aws/bootstrap.sh
#
# 1. Runs infra/ec2/setup.sh (packages, database, services, Caddy with HTTPS).
# 2. Installs the AWS IoT bridge's certificate from SSM Parameter Store (/smartclass/iot/*) and
#    starts smartclass-iot, so recorders connect through AWS IoT Core.
# 3. If /smartclass/bootstrap/admin exists, creates the first administrator, subjects and
#    recorders from it (tools/bootstrap.py). Delete that parameter afterwards.
# Secrets never appear in the user data or on the command line. Safe to re-run.
set -euo pipefail

SITE_ADDRESS="${SITE_ADDRESS:?set SITE_ADDRESS}"
export AWS_REGION="${AWS_REGION:-ap-south-1}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETC_DIR=/etc/smartclass
APP_DIR=/opt/smartclass
APP_USER=smartclass

param() { aws ssm get-parameter --with-decryption --name "$1" --query Parameter.Value --output text; }
has_param() { aws ssm get-parameter --name "$1" >/dev/null 2>&1; }
set_env() {  # set KEY=VALUE in backend.env, replacing an existing line
  local key="$1" value="$2" file="${ETC_DIR}/backend.env"
  if grep -q "^${key}=" "${file}"; then
    sed -i "s#^${key}=.*#${key}=${value}#" "${file}"
  else
    echo "${key}=${value}" >> "${file}"
  fi
}

echo "==> Server setup"
SITE_ADDRESS="${SITE_ADDRESS}" bash "${REPO_DIR}/infra/ec2/setup.sh"

echo "==> Settings for this deployment"
# Titan works on any account; Cohere needs an AWS Marketplace payment method (see backend.env.example).
set_env BEDROCK_EMBED_MODEL_ID amazon.titan-embed-text-v2:0
set_env BEDROCK_LLM_MODEL_ID "global.anthropic.claude-opus-4-6-v1,apac.amazon.nova-lite-v1:0"

echo "==> AWS IoT Core bridge"
if has_param /smartclass/iot/endpoint; then
  install -d -o root -g "${APP_USER}" -m 0750 "${ETC_DIR}/iot"
  ( umask 027
    param /smartclass/iot/cert > "${ETC_DIR}/iot/cert.pem"
    param /smartclass/iot/key > "${ETC_DIR}/iot/private.key" )
  chown root:"${APP_USER}" "${ETC_DIR}/iot/cert.pem" "${ETC_DIR}/iot/private.key"
  set_env IOT_ENDPOINT "$(param /smartclass/iot/endpoint)"
  set_env IOT_CERT_FILE "${ETC_DIR}/iot/cert.pem"
  set_env IOT_KEY_FILE "${ETC_DIR}/iot/private.key"
  systemctl enable smartclass-iot
else
  echo "    /smartclass/iot/endpoint not set: recorders can only use the WebSocket gateway."
fi

echo "==> First administrator, subjects and recorders"
if has_param /smartclass/bootstrap/admin; then
  # JSON: {"email","name","password","subjects":["CS501=Operating Systems",...],
  #        "devices":["id=..,serial=..,token_hash=..",...],
  #        "users":[{"email","name","role":"lecturer|student|admin","password"},...]}
  spec="$(param /smartclass/bootstrap/admin)"
  args=(--admin-email "$(jq -r .email <<<"${spec}")" --admin-name "$(jq -r '.name // "Administrator"' <<<"${spec}")")
  while IFS= read -r s; do [[ -n "${s}" ]] && args+=(--subject "${s}"); done < <(jq -r '.subjects[]?' <<<"${spec}")
  while IFS= read -r d; do [[ -n "${d}" ]] && args+=(--device "${d}"); done < <(jq -r '.devices[]?' <<<"${spec}")
  (
    cd "${APP_DIR}/backend"
    set -a; . "${ETC_DIR}/backend.env"; set +a
    ADMIN_PASSWORD="$(jq -r .password <<<"${spec}")" BOOTSTRAP_USERS="$(jq -c '.users // []' <<<"${spec}")" \
      sudo -E -u "${APP_USER}" "${APP_DIR}/backend/.venv/bin/python" -m tools.bootstrap "${args[@]}"
  )
fi

echo "==> Restart services"
systemctl restart smartclass-api smartclass-gateway smartclass-asr smartclass-notes
systemctl restart smartclass-iot || true
echo "Bootstrap done: https://${SITE_ADDRESS}"
