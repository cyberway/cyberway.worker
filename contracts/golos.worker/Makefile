
CONTRACT := golos.worker

SRC := golos.worker.cpp
CXX := eosiocpp

all: $(CONTRACT).wast $(CONTRACT).abi

$(CONTRACT).wast: $(SRC)
	$(CXX) -o $@ $<

$(CONTRACT).abi: $(SRC)
	$(CXX) -g $@.tmp $<
	cat $@.tmp | ./process-abi.py | tee $@
	rm $@.tmp

clean:
	rm -rf *.wast *.wasm
