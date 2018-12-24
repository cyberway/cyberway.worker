#include <boost/test/unit_test.hpp>
#include <boost/format.hpp>
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


#define ASSERT_SUCCESS(action) BOOST_REQUIRE_EQUAL((action), success())

const name worker_code_account = name("app.worker");
const name token_code_account = name("eosio.token");
const asset app_token_supply = asset::from_string("1000000.000 APP");
const asset app_fund_supply = asset::from_string("100.000 APP");
const asset initial_user_supply = asset::from_string("10.000 APP");
const asset proposal_deposit = asset::from_string("10.000 APP");

constexpr const char *long_text = "Lorem ipsum dolor sit amet, amet sint accusam sit te, te perfecto sadipscing vix, eam labore volumus dissentias ne. Est nonumy numquam fierent te. Te pri saperet disputando delicatissimi, pri semper ornatus ad. Paulo convenire argumentum cum te, te vix meis idque, odio tempor nostrum ius ad. Cu doctus mediocrem petentium his, eum sale errem timeam ne. Ludus debitis id qui, vix mucius antiopam ad. Facer signiferumque vis no, sale eruditi expetenda id ius.";
constexpr size_t delegates_count = 21;
constexpr size_t delegates_51 = delegates_count / 2 + 1;

enum state_t {
    STATE_TSPEC_APP = 1,
    STATE_TSPEC_CREATE,
    STATE_WORK,
    STATE_DELEGATES_REVIEW,
    STATE_PAYMENT,
    STATE_CLOSED
};

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

    fc::variant get_table_row(name table_name, const char *struct_name, name scope, uint64_t key)
    {
        vector<char> data = tester.get_row_by_account(code_account, scope, table_name, key);
        return data.empty() ? fc::variant() : abi_ser.binary_to_variant(struct_name, data, tester.abi_serializer_max_time);
    }

    size_t get_table_size(name table, uint64_t scope) {
        const auto& db = tester.control->db();
        const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( code_account, scope, table));
        if(!static_cast<bool>(t_id)) {
            return 0;
        }

        const auto& idx = db.get_index<chain::key_value_index, chain::by_scope_primary>();
        size_t count = 0;
        for (auto itr = idx.lower_bound(boost::make_tuple(t_id->id, 0)); itr != idx.end() && itr->t_id == t_id->id; itr++) {
            count++;
        }

        return count;
    }

    vector<fc::variant> get_table_rows(name table, const char *struct_name, uint64_t scope) {
        const auto& db = tester.control->db();
        const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( code_account, scope, table));
        if(!static_cast<bool>(t_id)) {
            return {};
        }

        const auto& idx = db.get_index<chain::key_value_index, chain::by_scope_primary>();
        vector<fc::variant> objects;
        for (auto itr = idx.lower_bound(boost::make_tuple(t_id->id, 0)); itr != idx.end() && itr->t_id == t_id->id; itr++) {
           vector<char> data(itr->value.size());
           memcpy(data.data(), itr->value.data(), data.size());
           objects.push_back(abi_ser.binary_to_variant(struct_name, data, tester.abi_serializer_max_time));
        }

        return objects;
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
                                        const string& memo)
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
        return get_table_row(N(accounts), "account", acc, symbol_code);
    }
};

class worker_contract : public base_contract
{
  public:
    worker_contract(base_tester &tester, account_name account) : base_contract(tester, account, contracts::golos_worker_wasm(), contracts::golos_worker_abi())
    {
    }

    fc::variant get_proposal(name scope, uint64_t id) {
        return base_contract::get_table_row(N(proposals), "proposal_t", scope, id);
    }

    uint8_t get_proposal_state(name scope, uint64_t proposal_id) {
        auto proposal = get_proposal(scope, proposal_id);
        return proposal["state"].as_int64();
    }

    fc::variant get_state(name scope) {
        return base_contract::get_table_row(N(state), "state_t", scope, 0);
    }

    fc::variant get_tspec(name scope, uint64_t id) {
        return base_contract::get_table_row(N(tspecs), "tspec_app_t", scope, id);
    }

    fc::variant get_proposal_comment(name scope, uint64_t id) {
        return base_contract::get_table_row(N(proposalsc), "comment_t", scope, id);
    }

    fc::variant get_fund(const name& scope, const name& fund_name) {
        return base_contract::get_table_row(N(funds), "fund_t", scope, fund_name);
    }

    size_t get_proposals_count(const uint64_t scope) {
        return base_contract::get_table_size(N(proposals), scope);
    }

    size_t get_tspecs_count(const uint64_t scope) {
        return base_contract::get_table_size(N(tspecs), scope);
    }

    size_t get_proposal_comments_count(const uint64_t scope) {
        return base_contract::get_table_size(N(proposalsc), scope);
    }

    size_t get_proposal_votes_count(const uint64_t scope) {
        return base_contract::get_table_size(N(proposalsv), scope);
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

        for (int i = 0; i < delegates_count; i++)
        {
            name delegate_name = string("delegate") + static_cast<char>('a' + i);
            delegates.push_back(delegate_name);
            create_account(delegate_name);

            name member_name = name(string("member") + static_cast<char>('a' + i));
            members.push_back(member_name);
            create_account(member_name);
            produce_blocks(2);
        }

        create_account(worker_code_account);
        create_account(token_code_account);
        create_account(N(golos.app));

        produce_blocks(2);

        base_tester &tester = dynamic_cast<base_tester &>(*this);
        token = make_unique<token_contract>(tester, token_code_account);
        worker = std::make_unique<worker_contract>(tester, worker_code_account);

        ASSERT_SUCCESS(token->create(token_code_account, app_token_supply));

        for (account_name &account : members) {
            ASSERT_SUCCESS(token->issue(token_code_account, account, initial_user_supply, "initial issue"));
            ASSERT_SUCCESS(token->open(account, initial_user_supply.get_symbol().to_string(), account));
            produce_blocks();
        }

        // create an application domain in the golos.worker
        ASSERT_SUCCESS(worker->push_action(worker_code_account, N(createpool), mvo()("token_symbol", app_token_supply.get_symbol())));
        produce_blocks();
        // add some funds to golos.worker contract
        ASSERT_SUCCESS(token->issue(token_code_account, worker_code_account, app_fund_supply, worker_code_account.to_string()));
        ASSERT_SUCCESS(token->open(worker_code_account, app_fund_supply.get_symbol().to_string(), worker_code_account));
        produce_blocks();

        auto fund = worker->get_fund(worker_code_account, worker_code_account);
        BOOST_REQUIRE(!fund.is_null());
        BOOST_REQUIRE_EQUAL(fund["quantity"], app_fund_supply.to_string());
    }

    void add_proposal(uint64_t proposal_id, const name& proposal_author, const name& tspec_author, const name& worker_account) {
        const uint64_t tspec_app_id = proposal_id * 100;
        const uint64_t other_tspec_app_id = tspec_app_id + 1;
        uint64_t comment_id = proposal_id  * 100;

        ASSERT_SUCCESS(worker->push_action(proposal_author, N(addpropos), mvo()
            ("proposal_id", proposal_id)
            ("author", proposal_author)
            ("title", boost::str(boost::format("Proposal #%d") % proposal_id))
            ("description", long_text)));

        produce_blocks(1);

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), 1);
        
        BOOST_TEST_MESSAGE("adding tspec");
        ASSERT_SUCCESS(worker->push_action(tspec_author, N(addtspec), mvo()
            ("proposal_id", proposal_id)
            ("tspec_app_id", tspec_app_id)
            ("author", tspec_author)
            ("tspec", mvo()
                ("text", "Technical specification #1")
                ("specification_cost", "5.000 APP")
                ("specification_eta", 1)
                ("development_cost", "5.000 APP")
                ("development_eta", 1)
                ("payments_count", 1)
                ("payments_interval", 1))));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_APP);

        ASSERT_SUCCESS(worker->push_action(tspec_author, N(addtspec), mvo()
            ("proposal_id", proposal_id)
            ("tspec_app_id", other_tspec_app_id)
            ("author", tspec_author)
            ("tspec", mvo()("text", "Technical specification #2")
                ("specification_cost", "5.000 APP")
                ("specification_eta", 1)
                ("development_cost", "5.000 APP")
                ("development_eta", 1)
                ("payments_count", 2)
                ("payments_interval", 1))));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_APP);

        // vote for the 0 technical specification application
        for (size_t i = 0; i < delegates_51; i++)
        {
            const name &delegate = delegates[i];

            ASSERT_SUCCESS(worker->push_action(delegate, N(approvetspec), mvo()
                ("tspec_app_id", tspec_app_id)
                ("author", delegate.to_string())
                ("comment_id", comment_id++)
                ("comment", mvo()("text", "Lorem Ipsum"))));
        }

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_CREATE);
        // if technical specification application was upvoted, `proposal_deposit` should be deposited from the application fund
        BOOST_REQUIRE_EQUAL(worker->get_proposal(worker_code_account, proposal_id)["deposit"], proposal_deposit.to_string());

        /* ok,technical specification application has been choosen,
        now technical specification application author should publish
        a final technical specification */
        BOOST_REQUIRE_EQUAL(worker->push_action(tspec_author, N(publishtspec), mvo()
            ("proposal_id", proposal_id)
            ("data", mvo()
                ("text", long_text)
                ("specification_cost", "5.000 APP")
                ("specification_eta", 1)
                ("development_cost", "5.000 APP")
                ("development_eta", 1)
                ("payments_count", 1)
                ("payments_interval", 1))), wasm_assert_msg("cost can't be modified"));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_CREATE);

        ASSERT_SUCCESS(worker->push_action(tspec_author, N(publishtspec), mvo()
            ("proposal_id", proposal_id)
            ("data", mvo()
                ("text", long_text)
                ("specification_cost", "0.000 APP")
                ("specification_eta", 1)
                ("development_cost", "0.000 APP")
                ("development_eta", 1)
                ("payments_count", 1)
                ("payments_interval", 1))));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_CREATE);

        ASSERT_SUCCESS(worker->push_action(tspec_author, N(startwork), mvo()
            ("proposal_id", proposal_id)
            ("worker", worker_account)));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_WORK);

        for (int i = 0; i < 5; i++) {
            ASSERT_SUCCESS(worker->push_action(worker_account, N(poststatus), mvo()
                ("proposal_id", proposal_id)
                ("comment_id", comment_id++)
                ("comment", mvo()
                    ("text", long_text))));
        }
    }
};

BOOST_AUTO_TEST_SUITE(eosio_worker_tests)


BOOST_FIXTURE_TEST_CASE(proposal_CUD, golos_worker_tester)
try
{
    for (uint64_t i = 0; i < 10; i++) {
        const uint64_t proposal_id = i;
        const name& author_account = members[i];
        return;
        ASSERT_SUCCESS(worker->push_action(author_account, N(addpropos), mvo()
            ("proposal_id", proposal_id)
            ("author", author_account)
            ("title", "Proposal #1")
            ("description", "Description #1")));
        return;
        auto proposal_row = worker->get_proposal(worker_code_account, proposal_id);
        BOOST_REQUIRE_EQUAL(proposal_row["state"], STATE_TSPEC_APP);
        BOOST_REQUIRE_EQUAL(proposal_row["title"], "Proposal #1");
        BOOST_REQUIRE_EQUAL(proposal_row["description"], "Description #1");
        BOOST_REQUIRE_EQUAL(proposal_row["author"], author_account.to_string());

        ASSERT_SUCCESS(worker->push_action(author_account, N(editpropos), mvo()
            ("proposal_id", proposal_id)
            ("title", "New Proposal #1")
            ("description", "")));

        BOOST_REQUIRE_EQUAL(worker->push_action(author_account, N(editpropos), mvo()
            ("proposal_id", proposal_id)
            ("title", "")
            ("description", "")), wasm_assert_msg("invalid arguments"));

        proposal_row = worker->get_proposal(worker_code_account, proposal_id);
        BOOST_REQUIRE_EQUAL(proposal_row["title"], "New Proposal #1");

        ASSERT_SUCCESS(worker->push_action(author_account, N(delpropos), mvo()
            ("proposal_id", proposal_id)));

        proposal_row = worker->get_proposal(worker_code_account, proposal_id);
        BOOST_REQUIRE(proposal_row.is_null());
    }
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(comment_CUD, golos_worker_tester)
try
{
    const uint64_t proposal_id = 0;

    ASSERT_SUCCESS(worker->push_action(members[0], N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", members[0])
        ("title", "Proposal #1")
        ("description", "Description #1")));

    constexpr uint64_t comments_count = 10;

    for (uint64_t i = 0; i < comments_count; i++) {
        const uint64_t comment_id = i;
        const name& comment_author = members[i];

        ASSERT_SUCCESS(worker->push_action(comment_author, N(addcomment), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", comment_id)
            ("author", comment_author)
            ("data", mvo()
                ("text", "Awesome!"))));

        // ensure fail when adding comment with same id
        BOOST_REQUIRE_EQUAL(worker->push_action(comment_author, N(addcomment), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", comment_id)
            ("author", comment_author)
            ("data", mvo()
                ("text", "Duplicate comment"))), wasm_assert_msg("comment exists"));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_comment(worker_code_account, comment_id)["data"]["text"].as_string(), "Awesome!");

        ASSERT_SUCCESS(worker->push_action(comment_author, N(editcomment), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", comment_id)
            ("data", mvo()
                ("text", "Fine!"))));

        // ensure fail when editing comment with empty text
        BOOST_REQUIRE_EQUAL(worker->push_action(comment_author, N(editcomment), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", comment_id)
            ("data", mvo()
                ("text", ""))), wasm_assert_msg("nothing to change"));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_comment(worker_code_account, comment_id)["data"]["text"].as_string(), "Fine!");
    }

    // check get_proposal_comments_count value is equal to comments_count after creating/editing comments
    BOOST_REQUIRE_EQUAL(worker->get_proposal_comments_count(worker_code_account), comments_count);

    for (uint64_t i = 0; i < comments_count; i++) {
        const uint64_t comment_id = i;
        const name& comment_author = members[i];

        ASSERT_SUCCESS(worker->push_action(comment_author, N(delcomment), mvo()
            ("comment_id", comment_id)));

        BOOST_REQUIRE(worker->get_proposal_comment(worker_code_account, comment_id).is_null());

        // ensure fail when deleting non-existing comment
        BOOST_REQUIRE_EQUAL(worker->push_action(comment_author, N(delcomment), mvo()
            ("comment_id", comment_id)), wasm_assert_msg("unable to find key"));

        // ensure fail when editing non-existing comment
        BOOST_REQUIRE_EQUAL(worker->push_action(comment_author, N(editcomment), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", comment_id)
            ("data", mvo()
                ("text", ""))), wasm_assert_msg("unable to find key"));
    }

    // check get_proposal_comments_count value is equal to 0 after deleting comments
    BOOST_REQUIRE_EQUAL(worker->get_proposal_comments_count(worker_code_account), 0);

    ASSERT_SUCCESS(worker->push_action(members[0], N(delpropos), mvo()
        ("proposal_id", proposal_id)));

    BOOST_REQUIRE(worker->get_proposal(worker_code_account, proposal_id).is_null());
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(vote_CUD, golos_worker_tester)
try
{
    const uint64_t proposal_id = 0;

    ASSERT_SUCCESS(worker->push_action(members[0], N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", members[0])
        ("title", "Proposal #1")
        ("description", "Description #1")));

    BOOST_REQUIRE_EQUAL(worker->get_proposal_votes_count(worker_code_account), 0);

    for (size_t i = 0; i < delegates.size(); i++)
    {
        name &delegate = delegates[i];

        ASSERT_SUCCESS(worker->push_action(delegate, N(votepropos), mvo()
            ("proposal_id", proposal_id)
            ("voter", delegate)
            ("positive", (i + 1) % 2)));
    }

    BOOST_REQUIRE_EQUAL(worker->get_proposal_votes_count(worker_code_account), delegates.size());

    // revote with the same `positive` value
    for (size_t i = 0; i < delegates.size(); i++)
    {
        name &delegate = delegates[i];

        BOOST_REQUIRE_EQUAL(worker->push_action(delegate, N(votepropos), mvo()
            ("proposal_id", proposal_id)
            ("voter", delegate)
            ("positive", (i + 1) % 2)), wasm_assert_msg("the vote already exists"));
    }

    BOOST_REQUIRE_EQUAL(worker->get_proposal_votes_count(worker_code_account), delegates.size());

    // revote with the different `positive` value
    for (size_t i = 0; i < delegates.size(); i++)
    {
        name &delegate = delegates[i];

        ASSERT_SUCCESS(worker->push_action(delegate, N(votepropos), mvo()
            ("proposal_id", proposal_id)
            ("voter", delegate)
            ("positive", (i) % 2)));
    }

    BOOST_REQUIRE_EQUAL(worker->get_proposal_votes_count(worker_code_account), delegates.size());

    auto votes = worker->get_table_rows(N(proposalsv), "vote_t", worker_code_account);
    BOOST_REQUIRE_EQUAL(delegates.size(), votes.size());
    // revote with the different `positive` value
    for (size_t i = 0; i < delegates.size(); i++)
    {
        name &delegate = delegates[i];
        fc::variant &vote = votes[i];

        BOOST_REQUIRE_EQUAL(vote["positive"], i % 2);
    }

    ASSERT_SUCCESS(worker->push_action(members[0], N(delpropos), mvo()
        ("proposal_id", proposal_id)));

    BOOST_REQUIRE_EQUAL(worker->get_proposal_votes_count(worker_code_account), 0);
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(technical_specification_application_CUD, golos_worker_tester)
try
{
    uint64_t comment_id = 0;

    for (uint64_t i = 0; i < 10; i++) {
        const uint64_t proposal_id = i;
        const name& proposal_author = members[i * 2];
        const name& tspec_author = members[i * 2 + 1];

        ASSERT_SUCCESS(worker->push_action(proposal_author, N(addpropos), mvo()
            ("proposal_id", proposal_id)
            ("author", proposal_author)
            ("title", "Proposal #1")
            ("description", "Description #1"))
        );

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_APP);

        for (uint64_t j = 0; j < 1; j++) {
            const uint64_t tspec_app_id = 100 * i + j;

            auto tspec_app = mvo()
                ("proposal_id", proposal_id)
                ("tspec_app_id", tspec_app_id)
                ("author", tspec_author)
                ("tspec", mvo()
                    ("text", "Technical specification")
                    ("specification_cost", "1.000 APP")
                    ("specification_eta", 1)
                    ("development_cost", "1.000 APP")
                    ("development_eta", 1)
                    ("payments_count", 1)
                    ("payments_interval", 1));

            ASSERT_SUCCESS(worker->push_action(tspec_author, N(addtspec), tspec_app));

            auto tspec_row = worker->get_tspec(worker_code_account, tspec_app_id);
            BOOST_REQUIRE_EQUAL(tspec_row["id"].as_int64(), tspec_app_id);
            REQUIRE_MATCHING_OBJECT(tspec_row["data"], tspec_app["tspec"]);

            ASSERT_SUCCESS(worker->push_action(tspec_author, N(edittspec), mvo()
                ("tspec_app_id", tspec_app_id)
                ("tspec", mvo()
                    ("text", "Technical specification")
                    ("specification_cost", "2.000 APP")
                    ("specification_eta", 2)
                    ("development_cost", "2.000 APP")
                    ("development_eta", 2)
                    ("payments_count", 2)
                    ("payments_interval", 2))));

            tspec_row = worker->get_tspec(worker_code_account, tspec_app_id);
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["specification_cost"].as_string(), "2.000 APP");
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["development_cost"].as_string(), "2.000 APP");

            const name& approver = delegates[0];
            ASSERT_SUCCESS(worker->push_action(approver, N(approvetspec), mvo()
                ("tspec_app_id", tspec_app_id)
                ("author", approver)
                ("comment_id", comment_id++)
                ("comment", mvo()("text", "Lorem Ipsum"))));

            ASSERT_SUCCESS(worker->push_action(approver, N(dapprovetspec), mvo()
                ("tspec_app_id", tspec_app_id)
                ("author", approver)));

            ASSERT_SUCCESS(worker->push_action(tspec_author, N(deltspec), mvo()
                ("tspec_app_id", tspec_app_id)));

            BOOST_REQUIRE(worker->get_tspec(worker_code_account, tspec_app_id).is_null());
        }

        ASSERT_SUCCESS(worker->push_action(proposal_author, N(delpropos), mvo()
            ("proposal_id", proposal_id)));

        BOOST_REQUIRE(worker->get_proposal(worker_code_account, proposal_id).is_null());
    }
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(proposal_removal, golos_worker_tester)
try
{
    const uint64_t proposal_id = 0;
    const name& proposal_author = members[0];
    const name& tspec_author = members[1];
    const name& comment_author = members[2];
    constexpr size_t relative_rows_count = 10;

    ASSERT_SUCCESS(worker->push_action(proposal_author, N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", proposal_author)
        ("title", "Proposal #1")
        ("description", "Description #1"))
    );

    BOOST_REQUIRE_EQUAL(worker->get_proposals_count(worker_code_account), 1);

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_APP);

    for (uint64_t j = 0; j < relative_rows_count; j++) {
        const uint64_t tspec_app_id = 100 + j;
        const uint64_t comment_id = 100 + j;

        ASSERT_SUCCESS(worker->push_action(comment_author, N(addcomment), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", comment_id)
            ("author", comment_author)
            ("data", mvo()
                ("text", "Awesome!"))));

        auto tspec_app = mvo()
            ("proposal_id", proposal_id)
            ("tspec_app_id", tspec_app_id)
            ("author", tspec_author)
            ("tspec", mvo()
                ("text", "Technical specification")
                ("specification_cost", "1.000 APP")
                ("specification_eta", 1)
                ("development_cost", "1.000 APP")
                ("development_eta", 1)
                ("payments_count", 1)
                ("payments_interval", 1));

        ASSERT_SUCCESS(worker->push_action(tspec_author, N(addtspec), tspec_app));
    }

    BOOST_REQUIRE_EQUAL(worker->get_proposal_comments_count(worker_code_account), relative_rows_count);
    BOOST_REQUIRE_EQUAL(worker->get_tspecs_count(worker_code_account), relative_rows_count);

    ASSERT_SUCCESS(worker->push_action(proposal_author, N(delpropos), mvo()
        ("proposal_id", proposal_id)));

    BOOST_REQUIRE_EQUAL(worker->get_proposals_count(worker_code_account), 0);
    BOOST_REQUIRE_EQUAL(worker->get_proposal_comments_count(worker_code_account), 0);
    BOOST_REQUIRE_EQUAL(worker->get_tspecs_count(worker_code_account), 0);

}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(application_fund, golos_worker_tester)
try
{
    for (uint64_t proposal_id = 0; proposal_id < 5; proposal_id++) {
        uint64_t comment_id = proposal_id  * 100;

        const name& proposal_author = members[proposal_id * 3];
        const name& tspec_author = members[proposal_id * 3 + 1];
        const name& worker_account = members[proposal_id * 3 + 2];

        add_proposal(proposal_id, proposal_author, tspec_author, worker_account);

        // revote for the proposal
        for (size_t i = 0; i < delegates.size(); i++)
        {
            name &delegate = delegates[i];

            ASSERT_SUCCESS(worker->push_action(delegate, N(votepropos), mvo()
                ("proposal_id", proposal_id)
                ("voter", delegate)
                ("positive", (i + 1) % 2)));
        }

        ASSERT_SUCCESS(worker->push_action(tspec_author, N(acceptwork), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", comment_id++)
            ("comment", mvo()
                ("text", long_text))));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_DELEGATES_REVIEW);

        for (size_t i = 0; i < delegates.size(); i++) {
            const name &delegate = delegates[i];
            ASSERT_SUCCESS(worker->push_action(delegate, N(reviewwork), mvo()
                ("proposal_id", proposal_id)
                ("reviewer", delegate.to_string())
                ("status", (i + 1) % 2)
                ("comment_id", comment_id++)
                ("comment", mvo()("text", "Lorem Ipsum"))));
        }

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_PAYMENT);

        ASSERT_SUCCESS(worker->push_action(worker_account, N(withdraw), mvo()
            ("proposal_id", proposal_id)));

        BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_CLOSED);

        auto worker_balance = token->get_account(worker_account, "3,APP");
        REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", "15.000 APP"));

        auto author_balance = token->get_account(tspec_author, "3,APP");
        REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", "15.000 APP"));
    }
}
FC_LOG_AND_RETHROW()

#
BOOST_FIXTURE_TEST_CASE(sponsored_fund, golos_worker_tester)
try
{
    name sponsor_account = members[0];
    name author_account = members[1];
    name worker_account = members[2];
    uint64_t proposal_id = 1;

    ASSERT_SUCCESS(worker->push_action(sponsor_account, N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", sponsor_account)
        ("title", "Proposal #1")
        ("description", "Description #1")));

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_APP);

    ASSERT_SUCCESS(token->transfer(sponsor_account, worker_code_account, asset::from_string("10.000 APP"), sponsor_account.to_string()));
    ASSERT_SUCCESS(worker->push_action(sponsor_account, N(setfund), mvo()
        ("proposal_id", proposal_id)
        ("fund_name", sponsor_account)
        ("quantity", "10.000 APP")));

    ASSERT_SUCCESS(worker->push_action(author_account, N(addtspec), mvo()
        ("proposal_id", proposal_id)
        ("tspec_app_id", 1)
        ("author", author_account)
        ("tspec", mvo()
            ("text", "Technical specification #1")
            ("specification_cost", "5.000 APP")
            ("specification_eta", 1)
            ("development_cost", "5.000 APP")
            ("development_eta", 1)
            ("payments_count", 1)
            ("payments_interval", 1))));

    ASSERT_SUCCESS(worker->push_action(author_account, N(addtspec), mvo()
        ("proposal_id", 1)
        ("tspec_app_id", 2)
        ("author", author_account)
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
        for (size_t i = 0; i < delegates_51; i++)
        {
            const name& account = delegates[i];

            ASSERT_SUCCESS(worker->push_action(account, N(approvetspec), mvo()
                ("tspec_app_id", tspec_app_id)
                ("author", account)
                ("comment_id", 100 + i)
                ("comment", mvo()("text", "Lorem Ipsum"))));
        }
    }


    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_TSPEC_CREATE);

    /* ok,technical specification application has been choosen,
    now technical specification application author should publish
    a final technical specification */
    ASSERT_SUCCESS(worker->push_action(author_account, N(publishtspec), mvo()
        ("proposal_id", proposal_id)
        ("data", mvo()
            ("text", long_text)
            ("specification_cost", "0.000 APP")
            ("specification_eta", 1)
            ("development_cost", "0.000 APP")
            ("development_eta", 1)
            ("payments_count", 1)
            ("payments_interval", 1))));

    ASSERT_SUCCESS(worker->push_action(author_account, N(startwork), mvo()
        ("proposal_id", proposal_id)
        ("worker", worker_account.to_string())));


    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_WORK);

    for (int i = 0; i < 5; i++) {
        ASSERT_SUCCESS(worker->push_action(worker_account, N(poststatus), mvo()
            ("proposal_id", proposal_id)
            ("comment_id", 300 + i)
            ("comment", mvo()
                ("text", long_text))));
    }

    ASSERT_SUCCESS(worker->push_action(author_account, N(acceptwork), mvo()
        ("proposal_id", proposal_id)
        ("comment_id", 400)
        ("comment", mvo()
            ("text", long_text))));


    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_DELEGATES_REVIEW);

    {
        int i = 0;
        for (const auto &account : delegates)
        {
            ASSERT_SUCCESS(worker->push_action(account, N(reviewwork), mvo()
                ("proposal_id", proposal_id)
                ("reviewer", account.to_string())
                ("status", (i + 1) % 2)
                ("comment_id", 500 + i)
                ("comment", mvo()("text", "Lorem Ipsum"))));
            i++;
        }
    }

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_PAYMENT);

    ASSERT_SUCCESS(worker->push_action(worker_account, N(withdraw), mvo()
        ("proposal_id", proposal_id)));

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_CLOSED);

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
        ("proposal_id", proposal_id)
        ("author", author_account)
        ("worker", worker_account)
        ("title", "Sponsored proposal #1")
        ("description", "Description #1")
        ("tspec", mvo()
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

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_DELEGATES_REVIEW);

    int i = 0;
    for (const auto &account : delegates) {
        ASSERT_SUCCESS(worker->push_action(account, N(reviewwork), mvo()
            ("proposal_id", proposal_id)
            ("reviewer", account.to_string())
            ("status", (i + 1) % 2)
            ("comment_id", 500 + i)
            ("comment", mvo()("text", "Lorem Ipsum"))));
        i++;
    }

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_PAYMENT);

    for (int i = 0; i < payments_count; i++) {
        ASSERT_SUCCESS(worker->push_action(worker_account, N(withdraw), mvo()
            ("proposal_id", 1)));
    }

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_CLOSED);

   auto worker_balance = token->get_account(worker_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", "15.000 APP"));

   auto author_balance = token->get_account(author_account, "3,APP");
   REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", "15.000 APP"));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(cancel_work_by_worker, golos_worker_tester)
try
{
    uint64_t proposal_id = 0;
    const name& proposal_author = members[proposal_id * 3];
    const name& tspec_author = members[proposal_id * 3 + 1];
    const name& worker_account = members[proposal_id * 3 + 2];

    add_proposal(proposal_id, proposal_author, tspec_author, worker_account);

    // the application fund quantity should be `propsal_deposit` less if proposal is in `STATE_TSPEC_CREATE`, `STATE_WORK`
    BOOST_REQUIRE_EQUAL(worker->get_fund(worker_code_account, worker_code_account)["quantity"], (app_fund_supply - proposal_deposit).to_string());

    ASSERT_SUCCESS(worker->push_action(worker_account, N(cancelwork), mvo()
        ("proposal_id", proposal_id)
        ("initiator", worker_account)));

    BOOST_REQUIRE_EQUAL(worker->push_action(worker_account, N(withdraw), mvo()
        ("proposal_id", proposal_id)), wasm_assert_msg("invalid state for withdraw"));

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_CLOSED);

    // if proposal is closed deposit should be refunded to the application fund
    BOOST_REQUIRE_EQUAL(worker->get_fund(worker_code_account, worker_code_account)["quantity"], app_fund_supply.to_string());

    auto worker_balance = token->get_account(worker_account, initial_user_supply.get_symbol().to_string());
    REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", initial_user_supply));

    auto author_balance = token->get_account(tspec_author, initial_user_supply.get_symbol().to_string());
    REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", initial_user_supply));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(cancel_work_by_tspec_author, golos_worker_tester)
try
{
    uint64_t proposal_id = 0;
    const name& proposal_author = members[proposal_id * 3];
    const name& tspec_author = members[proposal_id * 3 + 1];
    const name& worker_account = members[proposal_id * 3 + 2];

    add_proposal(proposal_id, proposal_author, tspec_author, worker_account);

    // the application fund quantity should be `propsal_deposit` less if proposal is in `STATE_TSPEC_CREATE`, `STATE_WORK`
    BOOST_REQUIRE_EQUAL(worker->get_fund(worker_code_account, worker_code_account)["quantity"], (app_fund_supply - proposal_deposit).to_string());

    ASSERT_SUCCESS(worker->push_action(tspec_author, N(cancelwork), mvo()
        ("proposal_id", proposal_id)
        ("initiator", tspec_author)));

    BOOST_REQUIRE_EQUAL(worker->push_action(worker_account, N(withdraw), mvo()
        ("proposal_id", proposal_id)), wasm_assert_msg("invalid state for withdraw"));

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_CLOSED);
    // if proposal is closed deposit should be refunded to the application fund
    BOOST_REQUIRE_EQUAL(worker->get_fund(worker_code_account, worker_code_account)["quantity"], app_fund_supply.to_string());

    auto worker_balance = token->get_account(worker_account, initial_user_supply.get_symbol().to_string());
    REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", initial_user_supply));

    auto author_balance = token->get_account(tspec_author, initial_user_supply.get_symbol().to_string());
    REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", initial_user_supply));
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(cancel_work_by_delegates, golos_worker_tester)
try
{
    uint64_t proposal_id = 0;
    uint64_t comment_id = 0;
    const name& proposal_author = members[proposal_id * 3];
    const name& tspec_author = members[proposal_id * 3 + 1];
    const name& worker_account = members[proposal_id * 3 + 2];

    add_proposal(proposal_id, proposal_author, tspec_author, worker_account);

    // the application fund quantity should be `propsal_deposit` less if proposal is in `STATE_TSPEC_CREATE`, `STATE_WORK`
    BOOST_REQUIRE_EQUAL(worker->get_fund(worker_code_account, worker_code_account)["quantity"], (app_fund_supply - proposal_deposit).to_string());

    for (size_t i = 0; i < delegates.size() * 3 / 4 + 1; i++) {
        const name &delegate = delegates[i];
        ASSERT_SUCCESS(worker->push_action(delegate, N(reviewwork), mvo()
            ("proposal_id", proposal_id)
            ("reviewer", delegate)
            ("status", 0)
            ("comment_id", comment_id++)
            ("comment", mvo()("text", "Lorem Ipsum"))));
    }

    BOOST_REQUIRE_EQUAL(worker->push_action(worker_account, N(withdraw), mvo()
        ("proposal_id", proposal_id)), wasm_assert_msg("invalid state for withdraw"));

    BOOST_REQUIRE_EQUAL(worker->get_proposal_state(worker_code_account, proposal_id), STATE_CLOSED);
    // if proposal is closed deposit should be refunded to the application fund
    BOOST_REQUIRE_EQUAL(worker->get_fund(worker_code_account, worker_code_account)["quantity"], app_fund_supply.to_string());

    auto worker_balance = token->get_account(worker_account, initial_user_supply.get_symbol().to_string());
    REQUIRE_MATCHING_OBJECT(worker_balance, mvo()("balance", initial_user_supply));

    auto author_balance = token->get_account(tspec_author, initial_user_supply.get_symbol().to_string());
    REQUIRE_MATCHING_OBJECT(author_balance, mvo()("balance", initial_user_supply));
}
FC_LOG_AND_RETHROW()


BOOST_AUTO_TEST_SUITE_END()