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

using mvo = fc::mutable_variant_object;

class base_contract
{
  protected:
    abi_serializer abi_ser;
    base_tester &tester; //TODO: be careful
    account_name contract_account_name;

  public:
    base_contract(base_tester &tester, account_name contract_account_name, const vector<uint8_t> &wasm, const vector<char> &abi_str) : tester(tester)
    {
        contract_account_name = contract_account_name;

        tester.set_code(contract_account_name, wasm);
        tester.set_abi(contract_account_name, abi_str.data());

        tester.produce_blocks();

        const auto &accnt = tester.control->db().get<account_object, by_name>(contract_account_name);
        abi_def abi;
        BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
        abi_ser.set_abi(abi, tester.abi_serializer_max_time);
    }

    base_tester::action_result push_action(const account_name &signer, const action_name &name, const variant_object &data)
    {
        string action_type_name = abi_ser.get_action_type(name);

        action act;
        act.account = contract_account_name;
        act.name = name;
        act.data = abi_ser.variant_to_binary(action_type_name, data, tester.abi_serializer_max_time);

        return tester.push_action(move(act), uint64_t(signer));
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

        return push_action(N(eosio.token), N(create), mvo()("issuer", issuer)("maximum_supply", maximum_supply));
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
};

class golos_worker_tester : public tester
{
  protected:
    vector<account_name> delegates;
    vector<account_name> members;
    std::unique_ptr<token_contract> token;
    std::unique_ptr<worker_contract> worker;

  public:
    golos_worker_tester()
    {
        produce_blocks();

        for (int i = 0; i < 22; i++)
        {
            string delegate_name = string("delegate") + static_cast<char>('a' + i);
            delegates.push_back(name(delegate_name));
            create_account(name(delegate_name));

            string member_name = string("member") + static_cast<char>('a' + i);
            members.push_back(name(member_name));
            create_account(name(member_name));
        }

        create_account(N(golos.worker));
        create_account(N(eosio.token));
        produce_blocks(2);

        base_tester &tester = dynamic_cast<base_tester&>(*this);
        token = make_unique<token_contract>(tester, N(eosio.token));
        worker = std::make_unique<worker_contract>(tester, account_name(N(golos.worker)));
    }
};

BOOST_AUTO_TEST_SUITE(eosio_worker_tests)

BOOST_FIXTURE_TEST_CASE(create_tests, golos_worker_tester)
try
{
    token->create(members[0], asset::from_string("1000.000 APP"));
    for (account_name &account: members) {
        token->issue(members[0], account, asset::from_string("5.000 APP"), "initial issue");
        produce_blocks();
    }

    produce_blocks();
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()