#pragma once

#include <cyberway.contracts/common/config.hpp>

namespace golos { namespace config {

using namespace cyber::config;

constexpr int witness_count = 21;
constexpr int witness_count_51 = witness_count / 2 + 1;
constexpr int witness_count_75 = witness_count * 3 / 4 + 1;

constexpr unsigned payout_expiration_sec  = 3*60*60;
constexpr unsigned payout_sender_id = 1;
constexpr size_t max_payed_tspecs_per_action = 10;

}} // golos::config
