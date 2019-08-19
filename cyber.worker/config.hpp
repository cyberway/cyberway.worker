#pragma once

#include <cyberway.contracts/common/config.hpp>

namespace cyber { namespace config {

constexpr int witness_count = 21;
constexpr int witness_count_51 = witness_count / 2 + 1;
constexpr int witness_count_75 = witness_count * 3 / 4 + 1;

constexpr unsigned payout_expiration_sec  = 3*60*60;
constexpr unsigned payout_sender_id = 1;
constexpr size_t max_payed_tspecs_per_action = 10;

const uint16_t max_comment_title_length = 256;
const char* err_too_long_title = "title is longer than 256";
}} // cyber::config
