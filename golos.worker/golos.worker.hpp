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
#define TIMESTAMP_MAX UINT32_MAX

#define LOG(format, ...) print_f("%::%: " format "\n", _self.to_string().c_str(), __FUNCTION__, ##__VA_ARGS__);

#define CHECK_POST(ID, AUTHOR) { \
    auto post = _comments.find(ID); \
    eosio::check(post != _comments.end(), "comment not exists"); \
    eosio::check(!post->parent_id, "comment is not root"); \
    eosio::check(post->author == AUTHOR, "comment not your"); \
}

#define CHECK_APPROVE_TSPEC(TSPEC, APPROVER) \
    require_app_delegate(approver); \
    eosio::check(TSPEC.state == tspec_app_t::STATE_CREATED, "invalid state"); \
    eosio::check(current_time_point().sec_since_epoch() <= TSPEC.created + voting_time_s, "approve time is over");

namespace golos
{
class [[eosio::contract]] worker : public contract
{
public:
    static constexpr uint32_t voting_time_s = 7 * 24 * 3600;

    using comment_id_t = uint64_t;
    struct [[eosio::table]] comment_t {
        comment_id_t id;
        std::optional<comment_id_t> parent_id;
        eosio::name author;

        EOSLIB_SERIALIZE(comment_t, (id)(parent_id)(author))

        uint64_t primary_key() const { return id; }
        std::optional<comment_id_t> by_parent() const { return parent_id; }
    };
    multi_index<"comments"_n, comment_t,
        indexed_by<"parent"_n, const_mem_fun<comment_t, std::optional<comment_id_t>, &comment_t::by_parent>>> _comments;

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

        bool empty(uint64_t foreign_id) const {
            auto index = votes.template get_index<"foreign"_n>();
            return index.find(foreign_id) == index.end();
        }

        void vote(uint64_t foreign_id, name voter, bool positive) {
            auto index = votes.template get_index<"foreign"_n>();
            for (auto vote_ptr = index.lower_bound(foreign_id); vote_ptr != index.upper_bound(foreign_id); vote_ptr++) {
                if (vote_ptr->voter == voter) {
                    eosio::check(vote_ptr->positive != positive, "the vote already exists");
                    votes.modify(votes.get(vote_ptr->id), voter, [&](auto &obj) {
                        obj.positive = positive;
                    });
                    return;
                }
            }
            votes.emplace(voter, [&](auto &obj) {
                obj.id = votes.available_primary_key();
                obj.foreign_id = foreign_id;
                obj.voter = voter;
                obj.positive = positive;
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

    voting_module_t<"proposalsv"_n> _proposal_votes;
    voting_module_t<"tspecv"_n> _tspec_votes;
    voting_module_t<"tspecrv"_n> _tspec_review_votes;

    struct tspec_data_t {
        asset specification_cost;
        uint32_t specification_eta;
        asset development_cost;
        uint32_t development_eta;
        uint16_t payments_count;
        uint32_t payments_interval;

        EOSLIB_SERIALIZE(tspec_data_t, (specification_cost)(specification_eta)(development_cost)(development_eta)(payments_count)(payments_interval))

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
            STATE_WIP,
            STATE_DELEGATES_REVIEW,
            STATE_PAYMENT,
            STATE_PAYMENT_COMPLETE,
            STATE_CLOSED_BY_AUTHOR,
            STATE_CLOSED_BY_WITNESSES,
            STATE_DISAPPROVED_BY_WITNESSES
        };

        enum review_status_t {
            STATUS_REJECT = 0,
            STATUS_ACCEPT = 1
        };

        comment_id_t id;
        comment_id_t foreign_id;
        eosio::name author;
        uint8_t state;
        tspec_data_t data;
        eosio::name fund_name;
        asset deposit;
        eosio::name worker;
        uint64_t work_begining_time;
        std::optional<comment_id_t> result_comment_id;
        uint8_t worker_payments_count;
        uint64_t next_payout;
        uint64_t created;
        uint64_t modified;

        EOSLIB_SERIALIZE(tspec_app_t, (id)(foreign_id)(author)(state)(data)(fund_name)(deposit)(worker)
            (work_begining_time)(result_comment_id)(worker_payments_count)(next_payout)
            (created)(modified))

        void modify(const tspec_data_t &that, bool limited = false) {
            data.update(that, limited);
            modified = TIMESTAMP_NOW;
        }

        uint64_t primary_key() const { return id; }
        uint64_t foreign_key() const { return foreign_id; }
        std::optional<comment_id_t> by_result() const { return result_comment_id; }
        uint64_t by_payout() const { return next_payout; }
        void set_state(state_t new_state) { state = new_state; }

    };
    multi_index<"tspecs"_n, tspec_app_t,
        indexed_by<"foreign"_n, const_mem_fun<tspec_app_t, uint64_t, &tspec_app_t::foreign_key>>,
        indexed_by<"resultc"_n, const_mem_fun<tspec_app_t, std::optional<comment_id_t>, &tspec_app_t::by_result>>,
        indexed_by<"payout"_n, const_mem_fun<tspec_app_t, uint64_t, &tspec_app_t::by_payout>>> _tspecs;

    struct [[eosio::table]] proposal_t {
        enum state_t {
            STATE_TSPEC_APP = 1,
            STATE_TSPEC_CHOSE
        };

        enum type_t {
            TYPE_TASK,
            TYPE_DONE
        };

        comment_id_t id;
        eosio::name author;
        uint8_t type;
        uint8_t state;
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

    struct tspecstate_event {
        comment_id_t id;
        uint8_t state;
    };

protected:
    void require_app_member(eosio::name account);
    void require_app_delegate(eosio::name account);

    auto get_state();
    const auto get_proposal(comment_id_t proposal_id);

    void deposit(tspec_app_t& tspec_app);
    void refund(tspec_app_t& tspec_app, eosio::name modifier);
    void close_tspec(name payer, const tspec_app_t& tspec_app, tspec_app_t::state_t state, const proposal_t& proposal);
    void send_tspecstate_event(const tspec_app_t& tspec_app, tspec_app_t::state_t state);
    void send_tspecerase_event(const tspec_app_t& tspec_app);
public:
    worker(eosio::name receiver, eosio::name code, eosio::datastream<const char *> ds) : contract(receiver, code, ds),
        _state(_self, _self.value),
        _proposals(_self, _self.value),
        _funds(_self, _self.value),   
        _comments(_self, _self.value),
        _proposal_votes(_self, _self.value),
        _tspec_review_votes(_self, _self.value),
        _tspecs(_self, _self.value),
        _tspec_votes(_self, _self.value) {}

    [[eosio::action]] void createpool(eosio::symbol token_symbol);

    [[eosio::action]] void addpropos(comment_id_t proposal_id, name author, uint8_t type);
    [[eosio::action]] void editpropos(comment_id_t proposal_id);
    [[eosio::action]] void delpropos(comment_id_t proposal_id);
    [[eosio::action]] void votepropos(comment_id_t proposal_id, eosio::name voter, uint8_t positive);

    [[eosio::action]] void addcomment(comment_id_t comment_id, eosio::name author, std::optional<comment_id_t> parent_id, const string& text);
    [[eosio::action]] void editcomment(comment_id_t comment_id, const string& text);
    [[eosio::action]] void delcomment(comment_id_t comment_id);

    [[eosio::action]] void addtspec(comment_id_t tspec_id, eosio::name author, comment_id_t proposal_id, const tspec_data_t& tspec, std::optional<name> worker);
    [[eosio::action]] void edittspec(comment_id_t tspec_id, const tspec_data_t &tspec, std::optional<name> worker);
    [[eosio::action]] void deltspec(comment_id_t tspec_id);
    [[eosio::action]] void apprtspec(comment_id_t tspec_id, name approver);
    [[eosio::action]] void dapprtspec(comment_id_t tspec_id, name approver);
    [[eosio::action]] void unapprtspec(comment_id_t tspec_id, name approver);
    [[eosio::action]] void startwork(comment_id_t tspec_id, name worker);
    [[eosio::action]] void cancelwork(comment_id_t tspec_id, eosio::name initiator);
    [[eosio::action]] void acceptwork(comment_id_t tspec_id, comment_id_t result_comment_id);
    [[eosio::action]] void unacceptwork(comment_id_t tspec_id);
    [[eosio::action]] void reviewwork(comment_id_t tspec_id, eosio::name reviewer, uint8_t status);
    [[eosio::action]] void payout(name ram_payer);

    void on_transfer(name from, name to, eosio::asset quantity, std::string memo);
};
} // namespace golos
