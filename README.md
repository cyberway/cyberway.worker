golos.worker
------------

Download:
```sh
git clone https://github.com/cyberway/cyberway.worker/ -b develop --recursive
```

Build:
```sh
cd cyberway.worker
cd cyberway.contracts
./build.sh
cd ..
./build.sh
```

Test:
```sh
/build/tests/unit_test -l message -r detailed --run_test=golos_worker_tests/*
```
