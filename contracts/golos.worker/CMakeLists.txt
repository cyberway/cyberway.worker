cmake_minimum_required(VERSION 3.5)
project(golos.worker VERSION 1.0.0)

find_package(eosio.cdt)

add_contract( worker golos.worker golos.worker.cpp )