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

constexpr const char *long_text = "Lorem ipsum dolor sit amet, amet sint accusam sit te, te perfecto sadipscing vix, eam labore volumus dissentias ne. Est nonumy numquam fierent te. Te pri saperet disputando delicatissimi, pri semper ornatus ad. Paulo convenire argumentum cum te, te vix meis idque, odio tempor nostrum ius ad. Cu doctus mediocrem petentium his, eum sale errem timeam ne. Ludus debitis id qui, vix mucius antiopam ad. Facer signiferumque vis no, sale eruditi expetenda id ius.";

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

    fc::variant get_account(account_name acc, const string& symbolname)
    {
        auto symb = eosio::chain::symbol::from_string(symbolname);
        auto symbol_code = symb.to_symbol_code().value;
        vector<char> data = tester.get_row_by_account( N(eosio.token), acc, N(accounts), symbol_code );
        return data.empty() ? fc::variant() : abi_ser.binary_to_variant("account", data, tester.abi_serializer_max_time);
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

        for (int i = 0; i < 21; i++)
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
            ASSERT_SUCCESS(token->issue(TOKEN_NAME, account, asset::from_string("10.000 APP"), "initial issue"));
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

BOOST_FIXTURE_TEST_CASE(application_fund, golos_worker_tester)
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
            ("payments_interval", 1))));

    ASSERT_SUCCESS(worker->push_action(members[1], N(addtspec), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("tspec_app_id", 2)
        ("author", members[1].to_string())
        ("tspec", mvo()("text", "Technical specification #2")
            ("specification_cost", "2.000 APP")
            ("specification_eta", 1)
            ("development_cost", "2.000 APP")
            ("development_eta", 1)
            ("payments_count", 2)
            ("payments_interval", 1))));

    // vote for the 0 technical specification application
    uint64_t tspec_app_id = 1;
    {
        int i = 0;
        for (const auto &account : delegates)
        {
            ASSERT_SUCCESS(worker->push_action(account, N(votetspec), mvo()
                ("app_domain", app_domain)
                ("tspec_app_id", tspec_app_id)
                ("author", account.to_string())
                ("vote", (i + 1) % 2)
                ("comment_id", 100 + i)
                ("comment", mvo()("text", "Lorem Ipsum"))));
            i++;
        }
    }
    /* after this point (51% voted positively), proposal is in state that
      doesn't allow voting for another technical specification application
      let's check it */
    auto account = delegates[0];
    BOOST_REQUIRE_EQUAL(worker->push_action(delegates[0], N(votetspec), mvo()
            ("app_domain", app_domain)
            ("tspec_app_id", 2)
            ("author", account.to_string())
            ("vote", 1)("comment_id", 200)
            ("comment", mvo()("text", ""))),
        wasm_assert_msg("invalid state for votetspec"));

    /* ok,technical specification application has been choosen,
    now technical specification application author should publish
    a final technical specification */
    auto author_account = members[0];
    ASSERT_SUCCESS(worker->push_action(author_account, N(publishtspec), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("data", mvo()
            ("text", long_text)
            ("specification_cost", "5.000 APP")
            ("specification_eta", 1)
            ("development_cost", "5.000 APP")
            ("development_eta", 1)
            ("payments_count", 1)
            ("payments_interval", 1))));

    auto worker_account = members[2];
    ASSERT_SUCCESS(worker->push_action(author_account, N(startwork), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("worker", worker_account.to_string())));

    for (int i = 0; i < 5; i++) {
        ASSERT_SUCCESS(worker->push_action(worker_account, N(poststatus), mvo()
            ("app_domain", app_domain)
            ("proposal_id", 1)
            ("comment_id", 300 + i)
            ("comment", mvo()
                ("text", long_text))));
    }

    ASSERT_SUCCESS(worker->push_action(author_account, N(acceptwork), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("comment_id", 400)
        ("comment", mvo()
            ("text", long_text))));

    {
        int i = 0;
        for (const auto &account : delegates)
        {
            ASSERT_SUCCESS(worker->push_action(account, N(reviewwork), mvo()
                ("app_domain", app_domain)
                ("proposal_id", 1)
                ("reviewer", account.to_string())
                ("status", (i + 1) % 2)
                ("comment_id", 500 + i)
                ("comment", mvo()("text", "Lorem Ipsum"))));
            i++;
        }
    }

    ASSERT_SUCCESS(worker->push_action(worker_account, N(withdraw), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)));

   auto worker_balance = token->get_account(worker_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", "15.000 APP"));

   auto author_balance = token->get_account(author_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", "15.000 APP"));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(sponsored_fund, golos_worker_tester)
try
{
    name sponsor_account = members[0];
    name author_account = members[1];
    name worker_account = members[2];
    uint64_t proposal_id = 1;

    ASSERT_SUCCESS(worker->push_action(sponsor_account, N(addpropos), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)
        ("author", sponsor_account)
        ("title", "Proposal #1")
        ("description", "Description #1")));

    ASSERT_SUCCESS(token->transfer(sponsor_account, WORKER_NAME, asset::from_string("10.000 APP"), app_domain));
    ASSERT_SUCCESS(worker->push_action(sponsor_account, N(setfund), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)
        ("fund_name", sponsor_account.to_string())
        ("quantity", "10.000 APP")));

    ASSERT_SUCCESS(worker->push_action(author_account, N(addtspec), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)
        ("tspec_app_id", 1)
        ("author", author_account.to_string())
        ("tspec", mvo()
            ("text", "Technical specification #1")
            ("specification_cost", "5.000 APP")
            ("specification_eta", 1)
            ("development_cost", "5.000 APP")
            ("development_eta", 1)
            ("payments_count", 1)
            ("payments_interval", 1))));

    ASSERT_SUCCESS(worker->push_action(members[3], N(addtspec), mvo()
        ("app_domain", app_domain)
        ("proposal_id", 1)
        ("tspec_app_id", 2)
        ("author", members[3].to_string())
        ("tspec", mvo()("text", "Technical specification #2")
            ("specification_cost", "2.000 APP")
            ("specification_eta", 1)
            ("development_cost", "2.000 APP")
            ("development_eta", 1)
            ("payments_count", 2)
            ("payments_interval", 1))));

    // vote for the 0 technical specification application
    uint64_t tspec_app_id = 1;
    {
        int i = 0;
        for (const auto &account : delegates)
        {
            ASSERT_SUCCESS(worker->push_action(account, N(votetspec), mvo()
                ("app_domain", app_domain)
                ("tspec_app_id", tspec_app_id)
                ("author", account.to_string())
                ("vote", (i + 1) % 2)
                ("comment_id", 100 + i)
                ("comment", mvo()("text", "Lorem Ipsum"))));
            i++;
        }
    }

    /* ok,technical specification application has been choosen,
    now technical specification application author should publish
    a final technical specification */
    ASSERT_SUCCESS(worker->push_action(author_account, N(publishtspec), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)
        ("data", mvo()
            ("text", long_text)
            ("specification_cost", "5.000 APP")
            ("specification_eta", 1)
            ("development_cost", "5.000 APP")
            ("development_eta", 1)
            ("payments_count", 1)
            ("payments_interval", 1))));

    ASSERT_SUCCESS(worker->push_action(author_account, N(startwork), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)
        ("worker", worker_account.to_string())));

    for (int i = 0; i < 5; i++) {
        ASSERT_SUCCESS(worker->push_action(worker_account, N(poststatus), mvo()
            ("app_domain", app_domain)
            ("proposal_id", proposal_id)
            ("comment_id", 300 + i)
            ("comment", mvo()
                ("text", long_text))));
    }

    ASSERT_SUCCESS(worker->push_action(author_account, N(acceptwork), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)
        ("comment_id", 400)
        ("comment", mvo()
            ("text", long_text))));

    {
        int i = 0;
        for (const auto &account : delegates)
        {
            ASSERT_SUCCESS(worker->push_action(account, N(reviewwork), mvo()
                ("app_domain", app_domain)
                ("proposal_id", proposal_id)
                ("reviewer", account.to_string())
                ("status", (i + 1) % 2)
                ("comment_id", 500 + i)
                ("comment", mvo()("text", "Lorem Ipsum"))));
            i++;
        }
    }

    ASSERT_SUCCESS(worker->push_action(worker_account, N(withdraw), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)));

   auto sponsor_balance = token->get_account(sponsor_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(sponsor_balance, mvo()("balance", "0.000 APP"));

   auto worker_balance = token->get_account(worker_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", "15.000 APP"));

   auto author_balance = token->get_account(author_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", "15.000 APP"));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(developed_feature, golos_worker_tester)
try
{
    name &author_account = members[0];
    name &worker_account = members[1];
    uint64_t proposal_id = 1;
    int payments_count = 3;

    ASSERT_SUCCESS(worker->push_action(author_account, N(addpropos2), mvo()
        ("app_domain", app_domain)
        ("proposal_id", proposal_id)
        ("author", author_account)
        ("worker", worker_account)
        ("title", "Sponsored proposal #1")
        ("description", "Description #1")
        ("specification", mvo()
            ("text", long_text)
            ("specification_cost", "5.000 APP")
            ("specification_eta", 1)
            ("development_cost", "5.000 APP")
            ("development_eta", 1)
            ("payments_count", payments_count)
            ("payments_interval", 1)
        )
        ("comment_id", 100)
        ("comment", mvo()
            ("text", long_text))
        ));

    int i = 0;
    for (const auto &account : delegates) {
        ASSERT_SUCCESS(worker->push_action(account, N(reviewwork), mvo()
            ("app_domain", app_domain)
            ("proposal_id", proposal_id)
            ("reviewer", account.to_string())
            ("status", (i + 1) % 2)
            ("comment_id", 500 + i)
            ("comment", mvo()("text", "Lorem Ipsum"))));
        i++;
    }

    for (int i = 0; i < payments_count; i++) {
        ASSERT_SUCCESS(worker->push_action(worker_account, N(withdraw), mvo()
            ("app_domain", app_domain)
            ("proposal_id", 1)));
    }

   auto worker_balance = token->get_account(worker_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", "15.000 APP"));

   auto author_balance = token->get_account(author_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", "15.000 APP"));

}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()