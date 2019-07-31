#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/action.hpp>
#include <eosio/time.hpp>
#include <eosio/crypto.hpp>
#include <eosio/singleton.hpp>
#include <eosio/symbol.hpp>
#include <eosio/name.hpp>
#include <eosio/serialize.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <cyberway.contracts/cyber.token/include/cyber.token/cyber.token.hpp>
#include <cyberway.contracts/common/dispatchers.hpp>
#include "config.hpp"

using namespace eosio;
using namespace std;

#define ZERO_ASSET eosio::asset(0, get_state().token_symbol)
#define TIMESTAMP_UNDEFINED 0
#define TIMESTAMP_NOW eosio::current_time_point().sec_since_epoch()

#define LOG(format, ...) print_f("%::%: " format "\n", _self.to_string().c_str(), __FUNCTION__, ##__VA_ARGS__);

namespace golos
{
class [[eosio::contract]] worker : public contract
{
public:
    static constexpr uint32_t voting_time_s = 7 * 24 * 3600;

    using comment_id_t = uint64_t;
    #define COMMENT_ROOT 0
    struct [[eosio::table]] comment_t {
        comment_id_t id;
        comment_id_t parent_id;
        eosio::name author;

        uint64_t primary_key() const { return id; }
        uint64_t get_secondary_1() const { return parent_id; }
    };

    struct [[eosio::table]] vote_t {
        uint64_t id;
        name voter;
        uint64_t foreign_id;
        bool positive;

        uint64_t primary_key() const { return id; }
        uint64_t get_secondary_1() const { return foreign_id; }
    };

    template <eosio::name::raw TableName>
    struct voting_module_t {
        multi_index<TableName, vote_t, indexed_by<"foreign"_n, const_mem_fun<vote_t, uint64_t, &vote_t::get_secondary_1>>> votes;

        voting_module_t(const eosio::name& code, uint64_t scope) : votes(code, scope) {}

        size_t count_positive(uint64_t foreign_id) const {
            auto index = votes.template get_index<name("foreign")>();
            return std::count_if(index.lower_bound(foreign_id), index.upper_bound(foreign_id), [&](const vote_t &vote) {
                return vote.positive;
            });
        }

        size_t count_negative(uint64_t foreign_id) const {
            auto index = votes.template get_index<"foreign"_n>();
            return std::count_if(index.lower_bound(foreign_id), index.upper_bound(foreign_id), [&](const vote_t &vote) {
                return !vote.positive;
            });
        }

        void vote(const vote_t &vote) {
            auto index = votes.template get_index<"foreign"_n>();
            for (auto vote_ptr = index.lower_bound(vote.foreign_id); vote_ptr != index.upper_bound(vote.foreign_id); vote_ptr++) {
                if (vote_ptr->voter == vote.voter) {
                    eosio::check(vote_ptr->positive != vote.positive, "the vote already exists");
                    votes.modify(votes.get(vote_ptr->id), vote.voter, [&](auto &obj) {
                        obj.positive = vote.positive;
                    });
                    return;
                }
            }
            votes.emplace(vote.voter, [&](auto &obj) {
                obj = vote;
                obj.id = votes.available_primary_key();
            });
        }

        void erase_all(uint64_t foreign_id) {
            auto index = votes.template get_index<name("foreign")>();
            auto ptr = index.lower_bound(foreign_id);
            while (ptr != index.upper_bound(foreign_id)) {
                uint64_t id = ptr->id;
                ptr++;
                votes.erase(votes.get(id));
            }
        }

        void erase(uint64_t foreign_id, const eosio::name &voter) {
            auto index = votes.template get_index<name("foreign")>();
            for (auto ptr = index.lower_bound(foreign_id); ptr != index.upper_bound(foreign_id); ptr++) {
                if (ptr->voter == voter) {
                    votes.erase(votes.get(ptr->id));
                    break;
                }
            }
        }
    };

    template <eosio::name::raw TableName>
    struct approve_module_t: protected voting_module_t<TableName> {
        using voting_module_t<TableName>::count_positive;
        using voting_module_t<TableName>::erase_all;

        approve_module_t(const eosio::name& code, uint64_t scope): voting_module_t<TableName>::voting_module_t(code, scope) {}

        void approve(uint64_t foreign_id, const eosio::name &approver) {
            vote_t v {
                .voter = approver,
                .positive = true,
                .foreign_id = foreign_id
            };

            voting_module_t<TableName>::vote(v);
        }

        void unapprove(uint64_t foreign_id, const eosio::name &approver) {
            voting_module_t<TableName>::erase(foreign_id, approver);
        }
    };

    typedef uint64_t tspec_id_t;
    struct tspec_data_t {
        asset specification_cost;
        uint32_t specification_eta;
        asset development_cost;
        uint32_t development_eta;
        uint16_t payments_count;
        uint32_t payments_interval;

        void update(const tspec_data_t &that, bool limited) {
            bool modified = false;

            if (that.specification_cost.amount != 0) {
                eosio::check(!limited, "cost can't be modified");
                specification_cost = that.specification_cost;
                modified = true;
            }

            if (that.specification_eta != 0) {
                specification_eta = that.specification_eta;
                modified = true;
            }

            if (that.development_cost.amount != 0) {
                eosio::check(!limited, "cost can be modified");
                development_cost = that.development_cost;
                modified = true;
            }

            if (that.development_eta != 0) {
                development_eta = that.development_eta;
                modified = true;
            }

            if (that.payments_count != 0) {
                payments_count = that.payments_count;
                modified = true;
            }

            eosio::check(modified, "nothing to modify");
        }
    };

    struct [[eosio::table]] tspec_app_t {
        enum state_t {
            STATE_CREATED = 1,
            STATE_APPROVED,
            STATE_WORK,
            STATE_DELEGATES_REVIEW,
            STATE_PAYMENT,
            STATE_PAYMENT_COMPLETE,
            STATE_CLOSED
        };

        enum review_status_t {
            STATUS_REJECT = 0,
            STATUS_ACCEPT = 1
        };

        tspec_id_t id;
        tspec_id_t foreign_id;
        eosio::name author;
        comment_id_t comment_id;
        uint8_t state;
        tspec_data_t data;
        eosio::name fund_name;
        asset deposit;
        eosio::name worker;
        uint64_t work_begining_time;
        comment_id_t result_comment_id;
        uint8_t worker_payments_count;
        uint64_t payment_begining_time;
        uint64_t created;
        uint64_t modified;

        void modify(const tspec_data_t &that, bool limited = false) {
            data.update(that, limited);
            modified = TIMESTAMP_NOW;
        }

        uint64_t primary_key() const { return id; }
        uint64_t foreign_key() const { return foreign_id; }
        void set_state(state_t new_state) { state = new_state; }

    };
    multi_index<"tspecs"_n, tspec_app_t, indexed_by<"foreign"_n, const_mem_fun<tspec_app_t, uint64_t, &tspec_app_t::foreign_key>>> _proposal_tspecs;

    using proposal_id_t = uint64_t;
    struct [[eosio::table]] proposal_t {
        enum state_t {
            STATE_TSPEC_APP = 1,
            STATE_TSPEC_CHOSE
        };

        enum type_t {
            TYPE_TASK,
            TYPE_DONE
        };

        proposal_id_t id;
        eosio::name author;
        comment_id_t comment_id;
        uint8_t type;
        uint8_t state;
        tspec_id_t tspec_id;
        uint64_t created;
        uint64_t modified;

        uint64_t primary_key() const { return id; }
        void set_state(state_t new_state) { state = new_state; }
    };
    multi_index<"proposals"_n, proposal_t> _proposals;

    struct [[eosio::table("state")]] state_t {
        eosio::symbol token_symbol;
    };
    singleton<"state"_n, state_t> _state;

    struct [[eosio::table]] fund_t {
        eosio::name owner;
        asset quantity;

        uint64_t primary_key() const { return owner.value; }
    };
    multi_index<"funds"_n, fund_t> _funds;
    multi_index<"comments"_n, comment_t,
        indexed_by<"parent"_n, const_mem_fun<comment_t, uint64_t, &comment_t::get_secondary_1>>> _comments;
    voting_module_t<"proposalsv"_n> _proposal_votes;
    approve_module_t<"proposalstsv"_n> _proposal_tspec_votes;
    voting_module_t<"tspecrv"_n> _tspec_review_votes;

protected:
    void require_app_member(eosio::name account);
    void require_app_delegate(eosio::name account);

    auto get_state();
    const auto get_proposal(proposal_id_t proposal_id);

    void deposit(tspec_app_t& tspec_app);
    void choose_proposal_tspec(proposal_t & proposal, const tspec_app_t &tspec_app);
    void pay_tspec_author(tspec_app_t& tspec_app);
    void refund(tspec_app_t& tspec_app, eosio::name modifier);
    void close_tspec(name payer, const tspec_app_t& tspec_app, tspec_app_t::state_t state, const proposal_t& proposal);
    void del_tspec(const tspec_app_t &tspec_app);
public:
    worker(eosio::name receiver, eosio::name code, eosio::datastream<const char *> ds) : contract(receiver, code, ds),
        _state(_self, _self.value),
        _proposals(_self, _self.value),
        _funds(_self, _self.value),   
        _comments(_self, _self.value),
        _proposal_votes(_self, _self.value),
        _tspec_review_votes(_self, _self.value),
        _proposal_tspecs(_self, _self.value),
        _proposal_tspec_votes(_self, _self.value) {}

    [[eosio::action]] void createpool(eosio::symbol token_symbol);

    [[eosio::action]] void addpropos(proposal_id_t proposal_id, const eosio::name& author, comment_id_t comment_id);
    [[eosio::action]] void addproposdn(proposal_id_t proposal_id, const eosio::name& author, comment_id_t comment_id, const eosio::name& worker,
        const tspec_data_t& tspec);
    [[eosio::action]] void editpropos(proposal_id_t proposal_id);
    [[eosio::action]] void delpropos(proposal_id_t proposal_id);
    [[eosio::action]] void votepropos(proposal_id_t proposal_id, eosio::name voter, uint8_t positive);

    [[eosio::action]] void addcomment(comment_id_t comment_id, eosio::name author, comment_id_t parent_id, const string& text);
    [[eosio::action]] void editcomment(comment_id_t comment_id, const string& text);
    [[eosio::action]] void delcomment(comment_id_t comment_id);

    [[eosio::action]] void addtspec(tspec_id_t tspec_app_id, eosio::name author, proposal_id_t proposal_id, comment_id_t comment_id, const tspec_data_t& tspec);
    [[eosio::action]] void edittspec(tspec_id_t tspec_app_id, const tspec_data_t &tspec);
    [[eosio::action]] void deltspec(tspec_id_t tspec_app_id);
    [[eosio::action]] void approvetspec(tspec_id_t tspec_app_id, eosio::name author);
    [[eosio::action]] void dapprovetspec(tspec_id_t tspec_app_id, eosio::name author);
    [[eosio::action]] void startwork(tspec_id_t tspec_app_id, eosio::name worker);
    [[eosio::action]] void cancelwork(tspec_id_t tspec_app_id, eosio::name initiator);
    [[eosio::action]] void acceptwork(tspec_id_t tspec_app_id, comment_id_t comment_id);
    [[eosio::action]] void reviewwork(tspec_id_t tspec_app_id, eosio::name reviewer, uint8_t status);
    [[eosio::action]] void withdraw(tspec_id_t tspec_app_id);

    void on_transfer(name from, name to, eosio::asset quantity, std::string memo);
};
} // namespace golos
