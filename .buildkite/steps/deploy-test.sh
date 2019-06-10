#/bin/bash
set -euo pipefail

cd Docker

# Run unit-tests
sleep 10s
docker run -ti cyberway/golos.worker /bin/bash -c '/golos.worker/contracts/tests/unit_test -l message -r detailed'
result=$?

exit $result
