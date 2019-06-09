golos.worker
------------

To build and run at first time:
```sh
docker run -ti -u $(id -u):$(id -g) --rm -w /workspace -v $(pwd):/workspace distributex/eosio.cdt bash
(git submodule update --init --recursive -f)
(cd eosio.contracts && ./build.sh)
(cd contracts/golos.worker && cmake .)
(cd contracts/tests && cmake .)
(cd contracts/golos.worker && make && cd ../tests && make && ./unit_test -l message -r detailed)
```

To build and run again, execute last command in same terminal.
