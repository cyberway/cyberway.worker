#!/bin/bash
set -euo pipefail

BRANCHNAME=${BUILDKITE_BRANCH:-master}

cd Docker
docker build -t cyberway/golos.worker --build-arg branch=${BRANCHNAME} .
