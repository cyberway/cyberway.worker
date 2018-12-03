#include <boost/test/unit_test.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/chain/abi_serializer.hpp>
#include <memory>
#include "Runtime/Runtime.h"

#include <fc/variant_object.hpp>
#include "contracts.hpp"

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::testing;
using namespace fc;
using namespace std;

using namespace ::eosio;
using mvo = fc::mutable_variant_object;

#define app_domain "golos.app"
#define TOKEN_NAME N(eosio.token)
#define WORKER_NAME N(golos.worker)
#define ASSERT_SUCCESS(action) BOOST_REQUIRE_EQUAL((action), success())

class base_contract
{
  protected:
    abi_serializer abi_ser;
    base_tester &tester; //TODO: be careful
    account_name code_account;

  public:
    base_contract(base_tester &tester, account_name account, const vector<uint8_t> &wasm, const vector<char> &abi_str) : tester(tester), code_account(account)
    {
        tester.set_code(code_account, wasm);
        tester.set_abi(code_account, abi_str.data());

        tester.produce_blocks();

        const auto &accnt = tester.control->db().get<account_object, by_name>(code_account);
        abi_def abi;
        BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
        abi_ser.set_abi(abi, tester.abi_serializer_max_time);
    }

    base_tester::action_result push_action(const account_name &signer, const action_name &name, const variant_object &data)
    {
        string action_type_name = abi_ser.get_action_type(name);

        action act;
        act.account = code_account;
        act.name = name;
        act.data = abi_ser.variant_to_binary(action_type_name, data, tester.abi_serializer_max_time);

        return tester.push_action(move(act), uint64_t(signer));
    }

    fc::variant get_table(name table_name, const char *struct_name, name scope)
    {
        vector<char> data = tester.get_row_by_account(code_account, scope, table_name, scope);
        return data.empty() ? fc::variant() : abi_ser.binary_to_variant(struct_name, data, tester.abi_serializer_max_time);
    }
};

class token_contract : public base_contract
{

  public:
    token_contract(base_tester &tester, account_name account) : base_contract(tester, account, contracts::token_wasm(), contracts::token_abi())
    {
    }

    base_tester::action_result create(account_name issuer,
                                      asset maximum_supply)
    {

        return push_action(issuer, N(create), mvo()("issuer", issuer)("maximum_supply", maximum_supply));
    }

    base_tester::action_result issue(account_name issuer, account_name to, asset quantity, string memo)
    {
        return push_action(issuer, N(issue), mvo()("to", to)("quantity", quantity)("memo", memo));
    }

    base_tester::action_result transfer(account_name from,
                                        account_name to,
                                        asset quantity,
                                        string memo)
    {
        return push_action(from, N(transfer), mvo()("from", from)("to", to)("quantity", quantity)("memo", memo));
    }

    base_tester::action_result open(account_name owner,
                                    const string &symbolname,
                                    account_name ram_payer)
    {
        return push_action(ram_payer, N(open), mvo()("owner", owner)("symbol", symbolname)("ram_payer", ram_payer));
    }
};

class worker_contract : public base_contract
{
  public:
    worker_contract(base_tester &tester, account_name account) : base_contract(tester, account, contracts::golos_worker_wasm(), contracts::golos_worker_abi())
    {
    }

    fc::variant get_proposals(name scope) {
        return base_contract::get_table(N(proposals), "proposal_t", scope);
    }

    fc::variant get_state(name scope) {
        return base_contract::get_table(N(state), "state_t", scope);
    }

    fc::variant get_tspecs(name scope) {
        return base_contract::get_table(N(tspecs), "tspec_data_t", scope);
    }
};

class golos_worker_tester : public tester
{
  protected:
    vector<name> delegates;
    vector<name> members;
    std::unique_ptr<token_contract> token;
    std::unique_ptr<worker_contract> worker;

  public:
    golos_worker_tester()
    {
        produce_blocks();

        for (int i = 0; i < 22; i++)
        {
            name delegate_name = string("delegate") + static_cast<char>('a' + i);
            delegates.push_back(delegate_name);
            create_account(delegate_name);

            name member_name = name(string("member") + static_cast<char>('a' + i));
            members.push_back(member_name);
            create_account(member_name);
            produce_blocks(2);
        }

        create_account(WORKER_NAME);
        create_account(TOKEN_NAME);
        create_account(N(golos.app));

        produce_blocks(2);

        base_tester &tester = dynamic_cast<base_tester &>(*this);
        token = make_unique<token_contract>(tester, TOKEN_NAME);
        worker = std::make_unique<worker_contract>(tester, WORKER_NAME);

        auto supply = asset::from_string("1000.000 APP");
        ASSERT_SUCCESS(token->create(TOKEN_NAME, supply));

        for (account_name &account : members)
        {
            ASSERT_SUCCESS(token->issue(TOKEN_NAME, account, asset::from_string("5.000 APP"), "initial issue"));
            ASSERT_SUCCESS(token->open(account, supply.get_symbol().to_string(), account));
            produce_blocks();
        }

        // create an application domain in the golos.worker
        ASSERT_SUCCESS(worker->push_action(N(golos.app), N(createpool), mvo()("app_domain", app_domain)("token_symbol", supply.get_symbol())));
        produce_blocks();

        // add some funds to golos.worker contract
        ASSERT_SUCCESS(token->issue(TOKEN_NAME, name(app_domain), asset::from_string("10.000 APP"), "initial issue"));
        ASSERT_SUCCESS(token->open(name(app_domain), supply.get_symbol().to_string(), name(app_domain)));
        ASSERT_SUCCESS(token->transfer(name(app_domain), WORKER_NAME, asset::from_string("10.000 APP"), app_domain));
        produce_blocks();
    }
};

BOOST_AUTO_TEST_SUITE(eosio_worker_tests)

BOOST_FIXTURE_TEST_CASE(proposal_CUD, golos_worker_tester)
try
{
    ASSERT_SUCCESS(worker->push_action(members[0], N(addpropos), mvo()("app_domain", app_domain)("proposal_id", 0)("author", members[0])("title", "Proposal #1")("description", "Description #1")));
    ASSERT_SUCCESS(worker->push_action(members[0], N(editpropos), mvo()("app_domain", app_domain)("proposal_id", 0)("title", "New Proposal #1")("description", "")));
    ASSERT_SUCCESS(worker->push_action(members[0], N(editpropos), mvo()("app_domain", app_domain)("proposal_id", 0)("title", "")("description", "")));
    ASSERT_SUCCESS(worker->push_action(members[0], N(delpropos), mvo()("app_domain", app_domain)("proposal_id", 0)));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(comment_CUD, golos_worker_tester)
try
{
    ASSERT_SUCCESS(worker->push_action(members[0], N(addpropos), mvo()("app_domain", app_domain)("proposal_id", 0)("author", members[0])("title", "Proposal #1")("description", "Description #1")));
    ASSERT_SUCCESS(worker->push_action(members[1], N(addcomment), mvo()("app_domain", app_domain)("proposal_id", 0)("comment_id", 0)("author", members[1])("data", mvo()("text", "Awesome!"))));
    ASSERT_SUCCESS(worker->push_action(members[1], N(editcomment), mvo()("app_domain", app_domain)("proposal_id", 0)("comment_id", 0)("data", mvo()("text", "Awesome!"))));
    ASSERT_SUCCESS(worker->push_action(members[1], N(delcomment), mvo()("app_domain", app_domain)("comment_id", 0)));
    ASSERT_SUCCESS(worker->push_action(members[0], N(delpropos), mvo()("app_domain", app_domain)("proposal_id", 0)));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(ux_flow_1, golos_worker_tester)
try
{
    ASSERT_SUCCESS(worker->push_action(members[0], N(addpropos), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("author", members[0])
        ("title", "Proposal #1")
        ("description", "Description #1")));

    ASSERT_SUCCESS(worker->push_action(members[0], N(addtspec), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("tspec_app_id", 1)
        ("author", members[0].to_string())
        ("tspec", mvo()
            ("text", "Technical specification #1")
            ("specification_cost", "1.000 APP")
            ("specification_eta", 1)
            ("development_cost", "1.000 APP")
            ("development_eta", 1)
            ("payments_count", 1)
            ("payment_interval", 1))));

    ASSERT_SUCCESS(worker->push_action(members[0], N(addtspec), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("tspec_app_id", 2)
        ("author", members[0].to_string())
        ("tspec", mvo()("text", "Technical specification #2")
        ("specification_cost", "2.000 APP")
        ("specification_eta", 1)
        ("development_cost", "2.000 APP")
        ("development_eta", 1)
        ("payments_count", 2)
        ("payment_interval", 1))));

    // vote for the 0 technical specification application
    uint64_t tspec_app_id = 1;
    int i = 0;
    for (const auto &account : delegates)
    {
        ASSERT_SUCCESS(worker->push_action(account, N(votetspec), mvo()
            ("app_domain", app_domain)
            ("tspec_app_id", tspec_app_id)
            ("author", account.to_string())
            ("vote", i % 2)
            ("comment_id", 100 + i)
            ("comment", mvo()("text", "Lorem Ipsum"))));
        i++;
    }

    // after this point (51% voted positively), proposal is in state that doesn't allow voting for another technical specification application
    // let's check it
    auto account = delegates[0];
    BOOST_REQUIRE_EQUAL(worker->push_action(delegates[0], N(votetspec), mvo()
            ("app_domain", app_domain)
            ("tspec_app_id", 1)
            ("author", account.to_string())
            ("vote", 1)("comment_id", 200)
            ("comment", mvo()("text", ""))),
        wasm_assert_msg("invalid state"));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(ux_flow_2, golos_worker_tester)
try
{
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()