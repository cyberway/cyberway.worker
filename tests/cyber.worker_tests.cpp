#include "golos_tester.hpp"
#include "cyber.worker_test_api.hpp"
#include "cyber.token_test_api.hpp"
#include <boost/test/unit_test.hpp>
#include <boost/format.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/chain/abi_serializer.hpp>
#include <memory>
#include "Runtime/Runtime.h"
#include <iostream>
#include <fc/variant_object.hpp>
#include "contracts.hpp"
#include <cyberway.contracts/common/config.hpp>

namespace cfg = cyber::config;
using namespace eosio;
using namespace eosio::chain;
using namespace eosio::testing;
using namespace fc;
using namespace std;
using namespace ::eosio;
using mvo = fc::mutable_variant_object;


#define ASSERT_SUCCESS(action) BOOST_REQUIRE_EQUAL((action), success())

const name worker_code_account = name("cyber.worker");
const asset app_token_supply = asset::from_string("1000000.000 APP");
const asset app_fund_supply = asset::from_string("100.000 APP");
const asset initial_user_supply = asset::from_string("10.000 APP");
const asset tspec_deposit = asset::from_string("10.000 APP");

constexpr const char *long_text = "Lorem ipsum dolor sit amet, amet sint accusam sit te, te perfecto sadipscing vix, eam labore volumus dissentias ne. Est nonumy numquam fierent te. Te pri saperet disputando delicatissimi, pri semper ornatus ad. Paulo convenire argumentum cum te, te vix meis idque, odio tempor nostrum ius ad. Cu doctus mediocrem petentium his, eum sale errem timeam ne. Ludus debitis id qui, vix mucius antiopam ad. Facer signiferumque vis no, sale eruditi expetenda id ius.";
constexpr size_t delegates_count = 21;
constexpr size_t delegates_51 = delegates_count / 2 + 1;

enum proposal_state_t {
    STATE_TSPEC_APP = 1,
    STATE_TSPEC_CHOSE
};

enum type_t {
    TYPE_TASK,
    TYPE_DONE
};

enum tspec_state_t {
    STATE_CREATED = 1,
    STATE_APPROVED,
    STATE_WORK,
    STATE_WIP,
    STATE_DELEGATES_REVIEW,
    STATE_PAYMENT,
    STATE_PAYMENT_COMPLETE,
    STATE_CLOSED_BY_AUTHOR,
    STATE_CLOSED_BY_WITNESSES
};

class cyber_worker_tester : public golos_tester {
protected:
    symbol _sym;
    cyber_worker_api worker;
    cyber_token_api token;
    vector<name> delegates;
    vector<name> members;
public:
    cyber_worker_tester() 
        : golos_tester(worker_code_account, false),
        _sym(3, "APP"),
        worker(this, worker_code_account),
        token({this, cfg::token_name, _sym})
    {
        create_accounts({cfg::token_name, _code});
        for (int i = 0; i < delegates_count; i++)
        {
            name delegate_name = string("delegate") + static_cast<char>('a' + i);
            delegates.push_back(delegate_name);
            name member_name = name(string("member") + static_cast<char>('a' + i));
            members.push_back(member_name);
        }
        create_accounts(delegates);
        create_accounts(members);
        produce_block();

        install_contract(cfg::token_name, contracts::token_wasm(), contracts::token_abi());
        install_contract(_code, contracts::worker_wasm(), contracts::worker_abi()); 

        init();
    }

    void init() {
        ASSERT_SUCCESS(token.create(cfg::token_name, app_token_supply));

        for (auto& account : members) {
            ASSERT_SUCCESS(token.issue(cfg::token_name, account, initial_user_supply, "initial issue"));
            ASSERT_SUCCESS(token.open(account, initial_user_supply.get_symbol(), account));
            produce_blocks();
        }

        ASSERT_SUCCESS(worker.push_action(_code, N(createpool), mvo()("token_symbol", app_token_supply.get_symbol())));
            produce_blocks();
        // add some funds to cyber.worker contract
        ASSERT_SUCCESS(token.issue(cfg::token_name, _code, app_fund_supply, _code.to_string()));
        ASSERT_SUCCESS(token.open(_code, app_fund_supply.get_symbol(), _code));
        produce_blocks();

        auto fund = worker.get_fund(_code, _code);
        BOOST_REQUIRE(!fund.is_null());
    }

    void add_proposal(uint64_t proposal_id, const name& proposal_author, uint64_t tspec_app_id, const name& tspec_author, const name& worker_account) {
        const uint64_t other_tspec_app_id = tspec_app_id + 1;

        ASSERT_SUCCESS(worker.push_action(proposal_author, N(addcomment), mvo()
            ("comment_id", proposal_id)
            ("author", proposal_author)
            ("text", long_text)));
        ASSERT_SUCCESS(worker.push_action(proposal_author, N(addpropos), mvo()
            ("proposal_id", proposal_id)
            ("author", proposal_author)
            ("type", static_cast<uint8_t>(TYPE_TASK))));

        produce_blocks(1);

        BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), 1);
        
        BOOST_TEST_MESSAGE("adding tspec");

        ASSERT_SUCCESS(worker.push_action(tspec_author, N(addcomment), mvo()
            ("comment_id", tspec_app_id)
            ("author", tspec_author)
            ("text", "Technical specification #1")));
        ASSERT_SUCCESS(worker.push_action(tspec_author, N(addtspec), mvo()
            ("tspec_id", tspec_app_id)
            ("author", tspec_author)
            ("proposal_id", proposal_id)
            ("tspec", mvo()
                ("specification_cost", "5.000 APP")
                ("development_cost", "5.000 APP")
                ("payments_count", 1)
                ("payments_interval", 1))));

        BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_APP);

        ASSERT_SUCCESS(worker.push_action(tspec_author, N(addcomment), mvo()
            ("comment_id", other_tspec_app_id)
            ("author", tspec_author)
            ("text", "Technical specification #2")));
        ASSERT_SUCCESS(worker.push_action(tspec_author, N(addtspec), mvo()
            ("tspec_id", other_tspec_app_id)
            ("author", tspec_author)
            ("proposal_id", proposal_id)
            ("tspec", mvo()
                ("specification_cost", "5.000 APP")
                ("development_cost", "5.000 APP")
                ("payments_count", 2)
                ("payments_interval", 1))));

        BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_APP);

        // vote for the 0 technical specification application
        ASSERT_SUCCESS(worker.push_action(_code, N(apprtspec), mvo()
            ("tspec_id", tspec_app_id)));

        BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_CHOSE);
        // if technical specification application was upvoted, `tspec_deposit` should be deposited from the application fund
        BOOST_REQUIRE_EQUAL(worker.get_tspec(tspec_app_id)["deposit"].as<asset>(), tspec_deposit);
        BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_app_id), STATE_APPROVED);

        BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_CHOSE);

        ASSERT_SUCCESS(worker.push_action(tspec_author, N(startwork), mvo()
            ("tspec_id", tspec_app_id)
            ("worker", worker_account)));

        BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_app_id), STATE_WORK);
        BOOST_REQUIRE_EQUAL(worker.get_tspec(tspec_app_id)["worker"], worker_account.to_string());
    }

    abi_serializer abi_ser;
};

BOOST_AUTO_TEST_SUITE(cyber_worker_tests)

BOOST_FIXTURE_TEST_CASE(comment_CUD, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: comment_CUD");

    uint64_t comment_id = 0;
    auto comment_author = members[comment_id];

    BOOST_TEST_MESSAGE("-- Adding root post");

    // ensure fail when adding comment below not-exist root comment
    BOOST_REQUIRE_EQUAL(worker.push_action(comment_author, N(addcomment), mvo()
        ("comment_id", comment_id)
        ("author", comment_author)
        ("parent_id", 100500)
        ("text", "Fake parent id")), wasm_assert_msg("parent comment not exists"));

    // ensure fail when adding comment with empty text
    BOOST_REQUIRE_EQUAL(worker.push_action(comment_author, N(addcomment), mvo()
        ("comment_id", comment_id)
        ("author", comment_author)
        ("text", "")), wasm_assert_msg("comment cannot be empty"));

    // normal case
    ASSERT_SUCCESS(worker.push_action(comment_author, N(addcomment), mvo()
        ("comment_id", comment_id)
        ("author", comment_author)
        ("text", "Root post")));

    // ensure fail when adding comment with same id
    BOOST_REQUIRE_EQUAL(worker.push_action(comment_author, N(addcomment), mvo()
        ("comment_id", comment_id)
        ("author", comment_author)
        ("text", "Duplicate comment")), wasm_assert_msg("already exists"));

    BOOST_REQUIRE_EQUAL(worker.get_comment(comment_id)["author"], comment_author.to_string());

    ASSERT_SUCCESS(worker.push_action(comment_author, N(editcomment), mvo()
        ("comment_id", comment_id)
        ("text", "Fine!")));

    // ensure fail when editing comment with empty text
    BOOST_REQUIRE_EQUAL(worker.push_action(comment_author, N(editcomment), mvo()
        ("comment_id", comment_id)
        ("text", "")), wasm_assert_msg("comment cannot be empty"));

    BOOST_TEST_MESSAGE("-- Adding child comments");

    constexpr uint64_t comments_count = 10;

    for (comment_id = 1; comment_id <= comments_count; comment_id++) {
        comment_author = members[comment_id];

        ASSERT_SUCCESS(worker.push_action(comment_author, N(addcomment), mvo()
            ("comment_id", comment_id)
            ("author", comment_author)
            ("parent_id", comment_id-1)
            ("text", "I am comment")));
    }

    // check comment count is equal to comments_count+1 after creating comments
    BOOST_REQUIRE_EQUAL(worker.get_comments().size(), comments_count+1);

    BOOST_TEST_MESSAGE("-- Deleting child comments");

    for (comment_id = comments_count; comment_id >= 1; comment_id--) {
        const name& comment_author = members[comment_id];

        // ensure fail when deleting comment with child
        BOOST_REQUIRE_EQUAL(worker.push_action(members[comment_id-1], N(delcomment), mvo()
            ("comment_id", comment_id-1)), wasm_assert_msg("comment has child comments"));

        ASSERT_SUCCESS(worker.push_action(comment_author, N(delcomment), mvo()
            ("comment_id", comment_id)));

        BOOST_REQUIRE(worker.get_comment(comment_id).is_null());

        // ensure fail when deleting non-existing comment
        BOOST_REQUIRE_EQUAL(worker.push_action(comment_author, N(delcomment), mvo()
            ("comment_id", comment_id)), wasm_assert_msg("unable to find key"));

        // ensure fail when editing non-existing comment
        BOOST_REQUIRE_EQUAL(worker.push_action(comment_author, N(editcomment), mvo()
            ("comment_id", comment_id)
            ("text", "Deleted")), wasm_assert_msg("unable to find key"));
    }

    // check comment count value is equal to 1 after deleting comments
    BOOST_REQUIRE_EQUAL(worker.get_comments().size(), 1);

    BOOST_TEST_MESSAGE("-- Deleting root post");

    comment_id = 0;
    comment_author = members[comment_id];

    ASSERT_SUCCESS(worker.push_action(comment_author, N(delcomment), mvo()
        ("comment_id", comment_id)));

    BOOST_REQUIRE(worker.get_comment(comment_id).is_null());

    // check comment count value is zero now
    BOOST_REQUIRE_EQUAL(worker.get_comments().size(), 0);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE(proposal_tests)

BOOST_FIXTURE_TEST_CASE(proposal_create, cyber_worker_tester) try {
    BOOST_TEST_MESSAGE("Testing: proposal_create");

    const auto& author_account = members[0];

    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", 0)
        ("author", author_account)
        ("text", "I am post")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", 1)
        ("author", author_account)
        ("parent_id", 0)
        ("text", "I am comment")));
    ASSERT_SUCCESS(worker.push_action(members[1], N(addcomment), mvo()
        ("comment_id", 2)
        ("author", members[1])
        ("text", "I am post")));

    BOOST_TEST_MESSAGE("-- wo post");
    BOOST_CHECK_EQUAL(wasm_assert_msg("comment not exists"), worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 3)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));

    BOOST_TEST_MESSAGE("-- comment instead of post");
    BOOST_CHECK_EQUAL(wasm_assert_msg("comment is not root"), worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 1)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));

    BOOST_TEST_MESSAGE("-- not own comment");
    BOOST_CHECK_EQUAL(wasm_assert_msg("comment not your"), worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 2)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));

    BOOST_TEST_MESSAGE("-- wrong type");
    BOOST_CHECK_EQUAL(wasm_assert_msg("wrong type"), worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE)+1)));

    BOOST_TEST_MESSAGE("-- wrong authority");
    BOOST_CHECK_EQUAL("missing authority of membera", worker.push_action(members[1], N(addpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));

    BOOST_TEST_MESSAGE("-- normal case");
    ASSERT_SUCCESS(worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));
    auto proposal_row = worker.get_proposal(0);
    BOOST_REQUIRE_EQUAL(proposal_row["id"], 0);
    BOOST_REQUIRE_EQUAL(proposal_row["type"], TYPE_TASK);
    BOOST_REQUIRE_EQUAL(proposal_row["author"], author_account.to_string());
    BOOST_REQUIRE_EQUAL(proposal_row["state"], STATE_TSPEC_APP);

    BOOST_TEST_MESSAGE("-- trying create with same id case");
    BOOST_CHECK_EQUAL(wasm_assert_msg("already exists"), worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(proposal_update, cyber_worker_tester) try {
    BOOST_TEST_MESSAGE("Testing: proposal_update");

    const auto& author_account = members[0];

    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", 0)
        ("author", author_account)
        ("text", "I am post")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));

    BOOST_TEST_MESSAGE("-- wrong type");
    BOOST_CHECK_EQUAL(wasm_assert_msg("wrong type"), worker.push_action(author_account, N(editpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE)+1)));

    BOOST_TEST_MESSAGE("-- not-exist");
    BOOST_CHECK_EQUAL(wasm_assert_msg("proposal has not been found"), worker.push_action(author_account, N(editpropos), mvo()
        ("proposal_id", 1)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE))));

    BOOST_TEST_MESSAGE("-- not own");
    BOOST_CHECK_EQUAL("missing authority of membera", worker.push_action(members[1], N(editpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE))));

    BOOST_TEST_MESSAGE("-- normal case");
    ASSERT_SUCCESS(worker.push_action(author_account, N(editpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE))));
    auto proposal_row = worker.get_proposal(0);
    BOOST_REQUIRE_EQUAL(proposal_row["type"], TYPE_DONE);

    BOOST_TEST_MESSAGE("-- with tspecs");
    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", 1)
        ("author", author_account)
        ("text", "I am post")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 1)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));
    auto tspec_app = mvo()
        ("tspec_id", 1)
        ("author", author_account)
        ("proposal_id", 1)
        ("tspec", mvo()
            ("specification_cost", "1.000 APP")
            ("development_cost", "1.000 APP")
            ("payments_count", 1)
            ("payments_interval", 1));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addtspec), tspec_app));
    BOOST_CHECK_EQUAL(wasm_assert_msg("proposal has tspecs"), worker.push_action(author_account, N(editpropos), mvo()
        ("proposal_id", 1)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE))));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(proposal_delete, cyber_worker_tester) try {
    BOOST_TEST_MESSAGE("Testing: proposal_delete");

    const auto& author_account = members[0];

    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", 0)
        ("author", author_account)
        ("text", "I am post")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", 0)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_TASK))));
    auto proposal_row = worker.get_proposal(0);
    BOOST_REQUIRE(!proposal_row.is_null());

    BOOST_REQUIRE_EQUAL(proposal_row["type"], TYPE_TASK);
    BOOST_TEST_MESSAGE("-- not-exist");
    BOOST_CHECK_EQUAL(wasm_assert_msg("proposal has not been found"), worker.push_action(author_account, N(delpropos), mvo()
        ("proposal_id", 1)));

    BOOST_TEST_MESSAGE("-- not own");
    BOOST_CHECK_EQUAL("missing authority of " + author_account.to_string(), worker.push_action(members[1], N(delpropos), mvo()
        ("proposal_id", 0)));

    BOOST_TEST_MESSAGE("-- normal case");
    proposal_row = worker.get_proposal(0);
    BOOST_REQUIRE(!proposal_row.is_null());
    ASSERT_SUCCESS(worker.push_action(author_account, N(delpropos), mvo()
        ("proposal_id", 0)));
    proposal_row = worker.get_proposal(0);
    BOOST_REQUIRE(proposal_row.is_null());

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(proposal_CUD, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: proposal_CUD");

    for (uint64_t i = 0; i < 10; i++) {
        const uint64_t proposal_id = i;
        const name& author_account = members[i];

        ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
            ("comment_id", proposal_id)
            ("author", author_account)
            ("text", "Proposal #1")));
        ASSERT_SUCCESS(worker.push_action(author_account, N(addpropos), mvo()
            ("proposal_id", proposal_id)
            ("author", author_account)
            ("type", static_cast<uint8_t>(TYPE_TASK))));
        auto proposal_row = worker.get_proposal(proposal_id);
        BOOST_REQUIRE_EQUAL(proposal_row["id"], proposal_id);
        BOOST_REQUIRE_EQUAL(proposal_row["type"], TYPE_TASK);
        BOOST_REQUIRE_EQUAL(proposal_row["author"], author_account.to_string());
        BOOST_REQUIRE_EQUAL(proposal_row["state"], STATE_TSPEC_APP);

        ASSERT_SUCCESS(worker.push_action(author_account, N(editpropos), mvo()
            ("proposal_id", proposal_id)
            ("type", static_cast<uint8_t>(TYPE_DONE))));
        proposal_row = worker.get_proposal(proposal_id);
        BOOST_REQUIRE_EQUAL(proposal_row["type"], TYPE_DONE);

        ASSERT_SUCCESS(worker.push_action(author_account, N(editpropos), mvo()
            ("proposal_id", proposal_id)
            ("type", static_cast<uint8_t>(TYPE_TASK))));
        proposal_row = worker.get_proposal(proposal_id);
        BOOST_REQUIRE_EQUAL(proposal_row["type"], TYPE_TASK);

        ASSERT_SUCCESS(worker.push_action(author_account, N(delpropos), mvo()
            ("proposal_id", proposal_id)));
        proposal_row = worker.get_proposal(proposal_id);
        BOOST_REQUIRE(proposal_row.is_null());
    }
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(proposal_removal, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: proposal_removal");

    const uint64_t proposal_id = 0;
    const name& proposal_author = members[0];
    const name& tspec_author = members[1];
    const name& comment_author = members[2];
    constexpr size_t relative_rows_count = 10;

    ASSERT_SUCCESS(worker.push_action(proposal_author, N(addcomment), mvo()
        ("comment_id", proposal_id)
        ("author", proposal_author)
        ("text", "Proposal #1")));
    ASSERT_SUCCESS(worker.push_action(proposal_author, N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", proposal_author)
        ("type", static_cast<uint8_t>(TYPE_TASK)))
    );

    BOOST_REQUIRE_EQUAL(worker.get_proposals(worker_code_account).size(), 1);

    BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_APP);

    for (uint64_t j = 0; j < relative_rows_count; j++) {
        const uint64_t tspec_app_id = 100 + j;
        const uint64_t comment_id = 1000 + j;

        ASSERT_SUCCESS(worker.push_action(comment_author, N(addcomment), mvo()
            ("comment_id", comment_id)
            ("author", comment_author)
            ("parent_id", proposal_id)
            ("text", "Awesome!")));

        ASSERT_SUCCESS(worker.push_action(tspec_author, N(addcomment), mvo()
            ("comment_id", tspec_app_id)
            ("author", tspec_author)
            ("text", "Awesome!")));
        auto tspec_app = mvo()
            ("tspec_id", tspec_app_id)
            ("author", tspec_author)
            ("proposal_id", proposal_id)
            ("tspec", mvo()
                ("specification_cost", "1.000 APP")
                ("development_cost", "1.000 APP")
                ("payments_count", 1)
                ("payments_interval", 1));

        ASSERT_SUCCESS(worker.push_action(tspec_author, N(addtspec), tspec_app));
    }

    BOOST_REQUIRE_EQUAL(worker.get_comments().size(), relative_rows_count * 2 + 1); // comments + tspec posts + 1 proposal post
    BOOST_REQUIRE_EQUAL(worker.get_tspecs(worker_code_account).size(), relative_rows_count);

    BOOST_REQUIRE_EQUAL(worker.push_action(proposal_author, N(delpropos), mvo()
        ("proposal_id", proposal_id)), wasm_assert_msg("proposal has tspecs"));

    for (uint64_t j = 0; j < relative_rows_count; j++) {
        const uint64_t tspec_app_id = 100 + j;
        ASSERT_SUCCESS(worker.push_action(tspec_author, N(deltspec), mvo()
                ("tspec_id", tspec_app_id)));
    }
    BOOST_REQUIRE_EQUAL(worker.get_tspecs(worker_code_account).size(), 0);

    ASSERT_SUCCESS(worker.push_action(proposal_author, N(delpropos), mvo()
        ("proposal_id", proposal_id)));

    BOOST_REQUIRE_EQUAL(worker.get_proposals(worker_code_account).size(), 0);
    BOOST_REQUIRE_EQUAL(worker.get_comments().size(), relative_rows_count * 2 + 1); // comments + tspec posts + 1 proposal post
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(done_proposal, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: done_proposal");

    auto& author_account = members[0];
    auto& worker_account = members[1];
    uint64_t proposal_id = 0;
    uint64_t tspec_id = 1;
    int payments_count = 3;

    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", proposal_id)
        ("author", author_account)
        ("text", "Sponsored proposal #1")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE))));

    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", tspec_id)
        ("author", author_account)
        ("text", "Technical specification #1")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addtspec), mvo()
        ("tspec_id", tspec_id)
        ("author", author_account)
        ("proposal_id", proposal_id)
        ("tspec", mvo()
            ("specification_cost", "5.000 APP")
            ("development_cost", "5.000 APP")
            ("payments_count", payments_count)
            ("payments_interval", 1))
        ("worker", worker_account)));

    // vote for the 0 technical specification application
    for (size_t i = 0; i < delegates_51; i++) {
        const auto& delegate = delegates[i];

        ASSERT_SUCCESS(worker.push_action(delegate, N(apprtspec), mvo()
            ("tspec_id", tspec_id)
            ("approver", delegate.to_string())));
    }

    BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_PAYMENT);

    for (int i = 0; i < payments_count; i++) {
        produce_block();
        ASSERT_SUCCESS(worker.push_action(worker_account, N(payout), mvo()
            ("ram_payer", worker_account)));
    }

    BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_PAYMENT_COMPLETE);
    BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_CHOSE);

    BOOST_REQUIRE_EQUAL(token.get_account(worker_account)["balance"], "15.000 APP");
    BOOST_REQUIRE_EQUAL(token.get_account(author_account)["balance"], "15.000 APP");
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_CASE(vote_CUD, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: vote_CUD");

    const uint64_t proposal_id = 0;

    ASSERT_SUCCESS(worker.push_action(members[0], N(addcomment), mvo()
        ("comment_id", proposal_id)
        ("author", members[0])
        ("text", "Proposal #1")));
    ASSERT_SUCCESS(worker.push_action(members[0], N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", members[0])
        ("type", static_cast<uint8_t>(TYPE_TASK))));

    for (size_t i = 0; i < delegates.size(); i++) {
        auto& delegate = delegates[i];
        if ((i + 1) % 2) {
            ASSERT_SUCCESS(worker.push_action(delegate, N(upvtpropos), mvo()
                ("proposal_id", proposal_id)
                ("voter", delegate)));
        } else {
            ASSERT_SUCCESS(worker.push_action(delegate, N(downvtpropos), mvo()
                ("proposal_id", proposal_id)
                ("voter", delegate)));
        }
    }
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(tspec_application_CUD, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: tspec_application_CUD");

    uint64_t comment_id = 0;

    for (uint64_t i = 0; i < 10; i++) {
        const uint64_t proposal_id = i;
        const name& proposal_author = members[i * 2];
        const name& tspec_author = members[i * 2 + 1];

        ASSERT_SUCCESS(worker.push_action(proposal_author, N(addcomment), mvo()
            ("comment_id", proposal_id)
            ("author", proposal_author)
            ("text", "Proposal #1")));
        ASSERT_SUCCESS(worker.push_action(proposal_author, N(addpropos), mvo()
            ("proposal_id", proposal_id)
            ("author", proposal_author)
            ("type", static_cast<uint8_t>(TYPE_TASK)))
        );

        BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_APP);

        for (uint64_t j = 0; j < 1; j++) {
            const uint64_t tspec_app_id = 100 * (i+1) + j;

            ASSERT_SUCCESS(worker.push_action(tspec_author, N(addcomment), mvo()
                ("comment_id", tspec_app_id)
                ("author", tspec_author)
                ("text", "Technical specification #1")));
            auto tspec_app = mvo()
                ("tspec_id", tspec_app_id)
                ("author", tspec_author)
                ("proposal_id", proposal_id)
                ("tspec", mvo()
                    ("specification_cost", "1.000 APP")
                    ("development_cost", "1.000 APP")
                    ("payments_count", 1)
                    ("payments_interval", 1));
            ASSERT_SUCCESS(worker.push_action(tspec_author, N(addtspec), tspec_app));

            auto tspec_row = worker.get_tspec(tspec_app_id);
            BOOST_REQUIRE_EQUAL(tspec_row["id"].as_int64(), tspec_app_id);
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["specification_cost"].as<asset>().to_string(), tspec_app["tspec"]["specification_cost"]);
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["development_cost"].as<asset>().to_string(), tspec_app["tspec"]["development_cost"]);
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["payments_count"], tspec_app["tspec"]["payments_count"]);
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["payments_interval"], tspec_app["tspec"]["payments_interval"]);

            ASSERT_SUCCESS(worker.push_action(tspec_author, N(edittspec), mvo()
                ("tspec_id", tspec_app_id)
                ("tspec", mvo()
                    ("specification_cost", "2.000 APP")
                    ("development_cost", "2.000 APP")
                    ("payments_count", 2)
                    ("payments_interval", 2))
                ("tspec_text", "Technical specification")));

            tspec_row = worker.get_tspec(tspec_app_id);
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["specification_cost"].as<asset>().to_string(), "2.000 APP");
            BOOST_REQUIRE_EQUAL(tspec_row["data"]["development_cost"].as<asset>().to_string(), "2.000 APP");

            ASSERT_SUCCESS(worker.push_action(tspec_author, N(deltspec), mvo()
                ("tspec_id", tspec_app_id)));

            BOOST_REQUIRE(worker.get_tspec(tspec_app_id).is_null());
        }

        ASSERT_SUCCESS(worker.push_action(proposal_author, N(delpropos), mvo()
            ("proposal_id", proposal_id)));

        BOOST_REQUIRE(worker.get_proposal(proposal_id).is_null());
    }
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(application_fund, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: application_fund");

    for (uint64_t proposal_id = 0; proposal_id < 5; proposal_id++) {
        uint64_t tspec_id = (proposal_id + 1) * 100;

        const name& proposal_author = members[proposal_id * 3];
        const name& tspec_author = members[proposal_id * 3 + 1];
        const name& worker_account = members[proposal_id * 3 + 2];

        add_proposal(proposal_id, proposal_author, tspec_id, tspec_author, worker_account);

        // revote for the proposal
        for (size_t i = 0; i < delegates.size(); i++) {
            auto& delegate = delegates[i];
            if ((i + 1) % 2) {
                ASSERT_SUCCESS(worker.push_action(delegate, N(upvtpropos), mvo()
                    ("proposal_id", proposal_id)
                    ("voter", delegate)));
            } else {
                ASSERT_SUCCESS(worker.push_action(delegate, N(downvtpropos), mvo()
                    ("proposal_id", proposal_id)
                    ("voter", delegate)));
            }
        }

        uint64_t comment_id = (proposal_id + 1) * 1000;
        ASSERT_SUCCESS(worker.push_action(tspec_author, N(addcomment), mvo()
            ("comment_id", comment_id)
            ("author", tspec_author)
            ("text", "Work done!")));
        ASSERT_SUCCESS(worker.push_action(tspec_author, N(acceptwork), mvo()
            ("tspec_id", tspec_id)
            ("result_comment_id", comment_id)));

        BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_DELEGATES_REVIEW);

        ASSERT_SUCCESS(worker.push_action(_code, N(apprwork), mvo()
            ("tspec_id", tspec_id)));

        BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_PAYMENT);

        ASSERT_SUCCESS(worker.push_action(worker_account, N(payout), mvo()
            ("ram_payer", worker_account)));

        BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_PAYMENT_COMPLETE);
        BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_CHOSE);

        BOOST_REQUIRE_EQUAL(token.get_account(worker_account)["balance"], "15.000 APP");
        BOOST_REQUIRE_EQUAL(token.get_account(tspec_author)["balance"], "15.000 APP");
    }
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(done_proposal, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: done_proposal");

    auto& author_account = members[0];
    auto& worker_account = members[1];
    uint64_t proposal_id = 0;
    uint64_t tspec_id = 1;
    int payments_count = 3;

    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", proposal_id)
        ("author", author_account)
        ("text", "Sponsored proposal #1")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addpropos), mvo()
        ("proposal_id", proposal_id)
        ("author", author_account)
        ("type", static_cast<uint8_t>(TYPE_DONE))));

    ASSERT_SUCCESS(worker.push_action(author_account, N(addcomment), mvo()
        ("comment_id", tspec_id)
        ("author", author_account)
        ("text", "Technical specification #1")));
    ASSERT_SUCCESS(worker.push_action(author_account, N(addtspec), mvo()
        ("tspec_id", tspec_id)
        ("author", author_account)
        ("proposal_id", proposal_id)
        ("tspec", mvo()
            ("specification_cost", "5.000 APP")
            ("development_cost", "5.000 APP")
            ("payments_count", payments_count)
            ("payments_interval", 1))
        ("worker", worker_account)));

    // vote for the 0 technical specification application
    ASSERT_SUCCESS(worker.push_action(_code, N(apprtspec), mvo()
        ("tspec_id", tspec_id)));

    BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_PAYMENT);

    for (int i = 0; i < payments_count; i++) {
        produce_block();
        ASSERT_SUCCESS(worker.push_action(worker_account, N(payout), mvo()
            ("ram_payer", worker_account)));
    }

    BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_PAYMENT_COMPLETE);
    BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_CHOSE);

    BOOST_REQUIRE_EQUAL(token.get_account(worker_account)["balance"], "15.000 APP");
    BOOST_REQUIRE_EQUAL(token.get_account(author_account)["balance"], "15.000 APP");
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(cancel_work_by_worker, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: cancel_work_by_worker");

    uint64_t proposal_id = 0;
    uint64_t tspec_id = 1;
    const name& proposal_author = members[proposal_id * 3];
    const name& tspec_author = members[proposal_id * 3 + 1];
    const name& worker_account = members[proposal_id * 3 + 2];

    add_proposal(proposal_id, proposal_author, tspec_id, tspec_author, worker_account);

    // the application fund quantity should be `tspec_deposit` less if tspec is approved
    BOOST_REQUIRE_EQUAL(worker.get_fund(worker_code_account, worker_code_account)["quantity"].as<asset>(), app_fund_supply - tspec_deposit);

    ASSERT_SUCCESS(worker.push_action(worker_account, N(cancelwork), mvo()
        ("tspec_id", tspec_id)
        ("initiator", worker_account)));

    BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_APPROVED);
    BOOST_REQUIRE_EQUAL(worker.get_tspec(tspec_id)["worker"], "");

    // the application fund quantity should not change
    BOOST_REQUIRE_EQUAL(worker.get_fund(worker_code_account, worker_code_account)["quantity"].as<asset>(), app_fund_supply - tspec_deposit);
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(cancel_work_by_tspec_author, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: cancel_work_by_tspec_author");

    uint64_t proposal_id = 0;
    uint64_t tspec_id = 1;
    const name& proposal_author = members[proposal_id * 3];
    const name& tspec_author = members[proposal_id * 3 + 1];
    const name& worker_account = members[proposal_id * 3 + 2];

    add_proposal(proposal_id, proposal_author, tspec_id, tspec_author, worker_account);

    // the application fund quantity should be `tspec_deposit` less if tspec is approved
    BOOST_REQUIRE_EQUAL(worker.get_fund(worker_code_account, worker_code_account)["quantity"].as<asset>(), app_fund_supply - tspec_deposit);

    ASSERT_SUCCESS(worker.push_action(tspec_author, N(cancelwork), mvo()
        ("tspec_id", tspec_id)
        ("initiator", tspec_author)));

    BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_APPROVED);
    BOOST_REQUIRE_EQUAL(worker.get_tspec(tspec_id)["worker"], "");

    // the application fund quantity should not change
    BOOST_REQUIRE_EQUAL(worker.get_fund(worker_code_account, worker_code_account)["quantity"].as<asset>(), app_fund_supply - tspec_deposit);
}
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(close_tspec_by_delegates, cyber_worker_tester)
try
{
    BOOST_TEST_MESSAGE("Testing: close_tspec_by_delegates");

    uint64_t proposal_id = 0;
    uint64_t tspec_id = 1;
    const name& proposal_author = members[proposal_id * 3];
    const name& tspec_author = members[proposal_id * 3 + 1];
    const name& worker_account = members[proposal_id * 3 + 2];

    add_proposal(proposal_id, proposal_author, tspec_id, tspec_author, worker_account);

    // the application fund quantity should be `tspec_deposit` less if tspec is approved
    BOOST_REQUIRE_EQUAL(worker.get_fund(worker_code_account, worker_code_account)["quantity"].as<asset>(), app_fund_supply - tspec_deposit);

    ASSERT_SUCCESS(worker.push_action(_code, N(dapprwork), mvo()
        ("tspec_id", tspec_id)));

    BOOST_REQUIRE_EQUAL(worker.get_tspec_state(tspec_id), STATE_CLOSED_BY_WITNESSES);
    BOOST_REQUIRE_EQUAL(worker.get_proposal_state(proposal_id), STATE_TSPEC_APP);

    // if tspec is closed deposit should be refunded to the application fund
    BOOST_REQUIRE_EQUAL(worker.get_fund(worker_code_account, worker_code_account)["quantity"].as<asset>(), app_fund_supply);

    BOOST_REQUIRE_EQUAL(token.get_account(worker_account)["balance"], initial_user_supply.to_string());
    BOOST_REQUIRE_EQUAL(token.get_account(tspec_author)["balance"], initial_user_supply.to_string());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
