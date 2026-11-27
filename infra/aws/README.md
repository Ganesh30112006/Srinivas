# AWS deployment

One CloudFormation stack (`smartclass.yaml`) creates everything the app needs on AWS, next to the
AWS IoT Core setup that the recorders use (`backend/tools/aws_iot.py`):

```
 Recorder (ESP32) ──MQTT/TLS──► AWS IoT Core ◄──MQTT/TLS── smartclass-iot ┐
                                                                         │ one EC2 server
 Browser ──HTTPS──► https://<elastic-ip>.sslip.io ──► Caddy ──► API, web app, ASR, notes ┘
```

- **Server:** Ubuntu 24.04, `c7i.2xlarge` by default, set up by `infra/ec2/setup.sh` plus
  `bootstrap.sh` (IoT bridge certificate, first administrator). PostgreSQL, Redis and the app
  ports listen on localhost only; the security group opens 80 and 443. There is no SSH: use
  Session Manager (`aws ssm start-session --target <instance-id>`).
- **Address:** a fixed Elastic IP and the free `sslip.io` name for it, so Caddy can get a Let's
  Encrypt certificate without a domain.
- **Secrets** live in SSM Parameter Store, never in the template, the user data or the repository:
  `/smartclass/iot/endpoint`, `/smartclass/iot/cert`, `/smartclass/iot/key` (SecureString), and
  `/smartclass/bootstrap/admin` (SecureString JSON, read once on first boot; delete it afterwards).
- **CI/CD:** `.github/workflows/ci.yml` tests every push; `deploy.yml` deploys `main` after CI
  passes, logging in to AWS through GitHub OIDC (no stored keys) and running `infra/ec2/deploy.sh`
  on the server over SSM. While the server is stopped, deploys are skipped; starting it with
  `server.ps1 start` deploys the latest `main`.

## Day to day

```powershell
powershell -File infra/aws/server.ps1 start    # before a lecture
powershell -File infra/aws/server.ps1 stop     # afterwards (only the disk is billed)
powershell -File infra/aws/server.ps1 status
```

A recorder that was powered on while the server was stopped keeps recording to its SD card or
internal flash and uploads everything once the server is running again.

## Create the stack (once)

```bash
# Bridge certificate (once per account), then store it for the server
python -m tools.aws_iot backend
aws ssm put-parameter --name /smartclass/iot/endpoint --type String --value "$(python -m tools.aws_iot endpoint)"
aws ssm put-parameter --name /smartclass/iot/cert --type SecureString --value file://secrets/iot/lc-backend/cert.pem
aws ssm put-parameter --name /smartclass/iot/key  --type SecureString --value file://secrets/iot/lc-backend/private.key
# First administrator, subjects and recorders (JSON; see bootstrap.sh)
aws ssm put-parameter --name /smartclass/bootstrap/admin --type SecureString --value file://admin.json

aws cloudformation deploy --stack-name smartclass --template-file infra/aws/smartclass.yaml \
  --capabilities CAPABILITY_IAM \
  --parameter-overrides GitHubOidcProviderArn=arn:aws:iam::<account>:oidc-provider/token.actions.githubusercontent.com
```

Then set the repository variables `AWS_REGION`, `AWS_DEPLOY_ROLE_ARN` and
`SMARTCLASS_INSTANCE_ID` from the stack outputs. First boot takes about 15 minutes; follow it with
`aws ssm start-session` and `sudo tail -f /var/log/smartclass-bootstrap.log`.
