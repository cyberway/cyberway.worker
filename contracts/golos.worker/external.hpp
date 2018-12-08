#pragma once

#include <eosiolib/eosio.hpp>
#include <eosiolib/action.hpp>
#include <eosiolib/asset.hpp>
#include <string>

static int constexpr witness_count = 21;
static int constexpr witness_count_51 = witness_count / 2 + 1;
static int constexpr wintess_count_75 = witness_count * 3 / 4 + 1;

// https://github.com/EOSIO/eosio.contracts/blob/6ca72e709faba179726a20571929a9eeaea47d08/eosio.token/include/eosio.token/eosio.token.hpp#L66
struct transfer_args {
       eosio::name  from;
       eosio::name  to;
       eosio::asset  quantity;
       std::string memo;
};
