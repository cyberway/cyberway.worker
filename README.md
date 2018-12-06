golos.worker
------------

```sh
docker run -ti -u $(id -u):$(id -g) --rm -w /workspace -v $(pwd):/workspace distributex/eosio.cdt bash
(cd eosio.contracts && ./build.sh)
(cd contracts/golos.worker && cmake . && make)
(cd contracts/tests && cmake . && make)
```
