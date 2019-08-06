<img width="400" src="./docs/logo.jpg" />  

*****  
[![Build status](https://badge.buildkite.com/68a58cc2c60ce8427a129607bbbe72d775d28f2583faf93228.svg?branch=develop)](https://buildkite.com/cyberway/create-cyberway-dot-worker-image)

cyberway.worker
------------
Dependencies:
* [cyberway latest](https://github.com/cyberway/cyberway/tree/develop)
* [cyberway.cdt latest](https://github.com/cyberway/cyberway.cdt/tree/develop)

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
build/tests/unit_test -l message -r detailed --run_test=golos_worker_tests/*
```
