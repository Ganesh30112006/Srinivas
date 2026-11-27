<#
.SYNOPSIS
  Start, stop or check the Smart Classroom server on AWS.

.EXAMPLE
  powershell -File infra/aws/server.ps1 start    # power on, update to the latest main, print the address
  powershell -File infra/aws/server.ps1 stop     # power off (only the disk is billed while stopped)
  powershell -File infra/aws/server.ps1 status

  Needs the AWS CLI logged in to the account that owns the stack (aws login / aws configure).
  The web address stays the same across stops: the server has a fixed (Elastic) IP.
#>
param(
  [Parameter(Mandatory = $true)][ValidateSet('start', 'stop', 'status')][string]$Action,
  [string]$Stack = 'smartclass',
  [string]$Region = 'ap-south-1'
)
$ErrorActionPreference = 'Stop'

function Get-Output([string]$key) {
  aws cloudformation describe-stacks --region $Region --stack-name $Stack `
    --query "Stacks[0].Outputs[?OutputKey=='$key'].OutputValue" --output text
}

$instance = Get-Output 'InstanceId'
$site = Get-Output 'SiteUrl'
$state = aws ec2 describe-instances --region $Region --instance-ids $instance `
  --query 'Reservations[0].Instances[0].State.Name' --output text

switch ($Action) {
  'status' {
    "Server $instance is $state. Address: $site"
  }
  'stop' {
    if ($state -eq 'stopped') { "Already stopped."; break }
    aws ec2 stop-instances --region $Region --instance-ids $instance | Out-Null
    "Stopping $instance. Recorders keep recording offline and upload when the server is back."
  }
  'start' {
    if ($state -ne 'running') {
      aws ec2 start-instances --region $Region --instance-ids $instance | Out-Null
      "Starting $instance..."
      aws ec2 wait instance-running --region $Region --instance-ids $instance
    }
    "Waiting for the server to accept commands..."
    for ($i = 0; $i -lt 60; $i++) {
      $ping = aws ssm describe-instance-information --region $Region `
        --filters "Key=InstanceIds,Values=$instance" --query 'InstanceInformationList[0].PingStatus' --output text
      if ($ping -eq 'Online') { break }
      Start-Sleep -Seconds 5
    }
    "Updating to the latest main (takes a few minutes)..."
    $cmd = aws ssm send-command --region $Region --instance-ids $instance `
      --document-name AWS-RunShellScript --timeout-seconds 1800 --comment 'deploy on start' `
      --parameters 'commands=["set -eu","cd /opt/smartclass-src","git fetch --quiet origin main","git checkout -q -B main origin/main","bash infra/ec2/deploy.sh"]' `
      --query Command.CommandId --output text
    do {
      Start-Sleep -Seconds 10
      $status = aws ssm get-command-invocation --region $Region --command-id $cmd --instance-id $instance `
        --query Status --output text 2>$null
    } while ($status -in @('Pending', 'InProgress', 'Delayed', $null, ''))
    if ($status -ne 'Success') {
      "Update failed ($status); the previous version is still running. Details:"
      aws ssm get-command-invocation --region $Region --command-id $cmd --instance-id $instance `
        --query StandardErrorContent --output text
    }
    "Ready: $site"
  }
}
