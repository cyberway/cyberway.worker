FROM distributex/eosio.cdt

ARG branch=master
ADD https://api.github.com/repos/GolosChain/golos.worker/git/refs/heads/$branch /etc/version.json
RUN git clone https://github.com/GolosChain/golos.worker -b $branch --recursive
RUN cd golos.worker/eosio.contracts \
    && ./build.sh
RUN cd golos.worker/contracts/golos.worker \
    && cmake . \
    && make
RUN cd golos.worker/contracts/tests \
    && cmake . \
    && make
