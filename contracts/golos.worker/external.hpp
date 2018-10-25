#pragma once

#include <eosiolib/eosio.hpp>
#include <eosiolib/action.hpp>
#include <string>

static int constexpr witness_count = 21;
static int constexpr witness_count_51 = witness_count / 2 + 1;
static int constexpr wintess_count_75 = witness_count * 3 / 4;

struct transfer_args {
       account_name  from;
       account_name  to;
       eosio::asset         quantity;
       eosio::string        memo;
};
