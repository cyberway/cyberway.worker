#pragma once
#include "test_api_helper.hpp"


namespace eosio { namespace testing {

struct cyber_worker_api: base_contract_api {
    cyber_worker_api(golos_tester* tester, name code)
    :   base_contract_api(tester, code) {
    }

    action_result push_action(account_name signer, action_name name, const variant_object& data) {
        auto result = _tester->push_action(_code, name, signer, data);
        _tester->produce_block();
        return result;
    }

    fc::variant get_proposal(uint64_t id) {
        return _tester->get_chaindb_struct(_code, _code, N(proposals), id, "proposal_t");
    }

    uint8_t get_proposal_state(uint64_t proposal_id) {
        auto proposal = get_proposal(proposal_id);
        return proposal["state"].as_int64();
    }

    uint8_t get_tspec_state(uint64_t tspec_app_id) {
        auto tspec = get_tspec(tspec_app_id);
        return tspec["state"].as_int64();
    }

    fc::variant get_state(name scope) {
        return _tester->get_chaindb_struct(_code, scope, N(state), 0, "state_t");
    }

    fc::variant get_tspec(uint64_t id) {
        return _tester->get_chaindb_struct(_code, _code, N(tspecs), id, "tspec_app_t");
    }

    fc::variant get_proposal_comment(name scope, uint64_t id) {
        return _tester->get_chaindb_struct(_code, scope, N(proposalsc), id, "comment_t");
    }

    fc::variant get_comment(uint64_t id) {
        return _tester->get_chaindb_struct(_code, _code, N(comments), id, "comment_t");
    }

    fc::variant get_fund(const name& scope, const name& fund_name) {
        return _tester->get_chaindb_struct(_code, scope, N(funds), fund_name, "fund_t");
    }

    std::vector<variant> get_proposals(const uint64_t scope) {
        return _tester->get_all_chaindb_rows(_code, scope, N(proposals), false);
    }

    std::vector<variant> get_tspecs(const uint64_t scope) {
        return _tester->get_all_chaindb_rows(_code, scope, N(tspecs), false);
    }

    std::vector<variant> get_proposal_comments(const uint64_t scope) {
        return _tester->get_all_chaindb_rows(_code, scope, N(proposalsc), false);
    }

    std::vector<variant> get_comments() {
        return _tester->get_all_chaindb_rows(_code, _code, N(comments), false);
    }

    std::vector<variant> get_proposal_votes(const uint64_t scope) {
        return _tester->get_all_chaindb_rows(_code, scope, N(proposalsv), false);
    }
};


}} // eosio::testing
