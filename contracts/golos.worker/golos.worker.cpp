#include <eosiolib/eosio.hpp>
#include <eosiolib/asset.hpp>
#include <eosiolib/action.hpp>
#include <eosiolib/time.hpp>
#include <eosiolib/crypto.h>
#include <eosiolib/singleton.hpp>
#include <eosiolib/symbol.hpp>
#include <eosiolib/name.hpp>
#include <eosiolib/serialize.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "external.hpp"
#include "app_dispatcher.hpp"

using namespace eosio;
using namespace std;

#define TOKEN_ACCOUNT "eosio.token"_n
#define ZERO_ASSET eosio::asset(0, get_state().token_symbol)
#define TIMESTAMP_UNDEFINED block_timestamp(0)
#define TIMESTAMP_NOW block_timestamp(now())

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define ACCOUNT_NAME_CSTR(account_name) eosio::name(account_name).to_string().c_str()
#define LOG(format, ...) print_f("%(%): " format "\n", __FUNCTION__, ACCOUNT_NAME_CSTR(_app), ##__VA_ARGS__);

namespace golos
{
class [[eosio::contract]] worker : public contract
{
private:
    static constexpr uint32_t voting_time_s = 7 * 24 * 3600;

    using comment_id_t = uint64_t;
    struct comment_data_t {
        string text;

        EOSLIB_SERIALIZE(comment_data_t, (text));
    };
    struct [[eosio::table]] comment_t {
        comment_id_t id;
        uint64_t foreign_id;
        eosio::name author;
        comment_data_t data;
        block_timestamp created;
        block_timestamp modified;

        EOSLIB_SERIALIZE(comment_t, (id)(foreign_id)(author)(data)(created)(modified));

        uint64_t primary_key() const { return id; }
        uint64_t get_secondary_1() const { return foreign_id; }
    };

    template <eosio::name::raw TableName>
    struct comments_module_t {
        multi_index<TableName, comment_t,
            indexed_by<"foreign"_n,
                const_mem_fun<comment_t, uint64_t, &comment_t::get_secondary_1>>> comments;

        comments_module_t(eosio::name code, uint64_t scope) : comments(code, scope) {}

        void add(comment_id_t id, uint64_t foreign_id, eosio::name author, const comment_data_t &data)
        {
            eosio_assert(comments.find(id) == comments.end(), "comment exists");
            comments.emplace(author, [&](auto &obj) {
                obj.id = id;
                obj.author = author;
                obj.data = data;
                obj.foreign_id = foreign_id;
                obj.created = TIMESTAMP_NOW;
                obj.modified = TIMESTAMP_UNDEFINED;
            });
        }

        void del(comment_id_t id)
        {
            const auto& comment = comments.get(id);
            require_auth(comment.author);
            comments.erase(comment);
        }

        void edit(comment_id_t id, const comment_data_t &data)
        {
            eosio_assert(!data.text.empty(), "nothing to change");
            const auto &comment = comments.get(id);
            require_auth(comment.author);

            comments.modify(comment, comment.author, [&](comment_t &obj) {
                obj.data.text = data.text;
            });
        }

        void erase_all(uint64_t foreign_id) {
            auto index = comments.template get_index<name("foreign")>();
            auto ptr = index.lower_bound(foreign_id);
            while (ptr != index.upper_bound(foreign_id)) {
                comment_id_t id = ptr->id;
                ptr++;
                comments.erase(comments.get(id));
            }
        }
    };

    struct [[eosio::table]] vote_t {
        uint64_t id;
        name voter;
        uint64_t foreign_id;
        bool positive;

        uint64_t primary_key() const { return id; }
        uint64_t get_secondary_1() const { return foreign_id; }

        EOSLIB_SERIALIZE(vote_t, (id)(foreign_id)(voter)(positive));
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
                    eosio_assert(vote_ptr->positive != vote.positive, "the vote already exists");
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
        string text;
        asset specification_cost;
        uint32_t specification_eta;
        asset development_cost;
        uint32_t development_eta;
        uint16_t payments_count;
        uint32_t payments_interval;

        EOSLIB_SERIALIZE(tspec_data_t, (text) \
            (specification_cost)(specification_eta) \
            (development_cost)(development_eta) \
            (payments_count)(payments_interval));

        void update(const tspec_data_t &that, bool limited) {
            bool modified = false;

            if (!that.text.empty()) {
                text = that.text;
                modified = true;
            }

            if (that.specification_cost.amount != 0) {
                eosio_assert(!limited, "cost can't be modified");
                specification_cost = that.specification_cost;
                modified = true;
            }

            if (that.specification_eta != 0) {
                specification_eta = that.specification_eta;
                modified = true;
            }

            if (that.development_cost.amount != 0) {
                eosio_assert(!limited, "cost can be modified");
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

            eosio_assert(modified, "nothing to modify");
        }
    };

    struct [[eosio::table]] tspec_app_t {
        tspec_id_t id;
        tspec_id_t foreign_id;
        eosio::name author;
        tspec_data_t data;
        block_timestamp created;
        block_timestamp modified;

        EOSLIB_SERIALIZE(tspec_app_t, (id)(foreign_id)(author)(data)(created)(modified));

        void modify(const tspec_data_t &that, bool limited = false) {
            data.update(that, limited);
            modified = TIMESTAMP_NOW;
        }

        uint64_t primary_key() const { return id; }
        uint64_t foreign_key() const { return foreign_id; }

    };
    multi_index<"tspecs"_n, tspec_app_t, indexed_by<"foreign"_n, const_mem_fun<tspec_app_t, uint64_t, &tspec_app_t::foreign_key>>> _proposal_tspecs;

    using proposal_id_t = uint64_t;
    struct [[eosio::table]] proposal_t {
        enum state_t {
            STATE_TSPEC_APP = 1,
            STATE_TSPEC_CREATE,
            STATE_WORK,
            STATE_DELEGATES_REVIEW,
            STATE_PAYMENT,
            STATE_CLOSED
        };

        enum review_status_t {
            STATUS_REJECT = 0,
            STATUS_ACCEPT = 1
        };

        enum type_t {
            TYPE_1,
            TYPE_2
        };

        proposal_id_t id;
        eosio::name author;
        uint8_t type;
        uint8_t state;
        string title;
        string description;
        eosio::name fund_name;
        asset deposit;
        tspec_id_t tspec_id;
        eosio::name worker;
        block_timestamp work_begining_time;
        uint8_t worker_payments_count;
        block_timestamp payment_begining_time;
        block_timestamp created;
        block_timestamp modified;

        EOSLIB_SERIALIZE(proposal_t, (id)(author)(type)(state)(title)(description)\
            (fund_name)(deposit)(tspec_id)\
            (worker)(work_begining_time)(worker_payments_count)\
            (payment_begining_time)(created)(modified));

        uint64_t primary_key() const { return id; }
        void set_state(state_t new_state) { state = new_state; }
    };
    multi_index<"proposals"_n, proposal_t> _proposals;

    struct [[eosio::table("state")]] state_t {
        eosio::symbol token_symbol;
        EOSLIB_SERIALIZE(state_t, (token_symbol));
    };
    singleton<"state"_n, state_t> _state;

    struct [[eosio::table]] fund_t {
        eosio::name owner;
        asset quantity;

        EOSLIB_SERIALIZE(fund_t, (owner)(quantity));

        uint64_t primary_key() const { return owner.value; }
    };
    multi_index<"funds"_n, fund_t> _funds;

    comments_module_t<"proposalsc"_n> _proposal_comments;
    voting_module_t<"proposalsv"_n> _proposal_votes;
    approve_module_t<"proposalstsv"_n> _proposal_tspec_votes;
    comments_module_t<"tspecappc"_n> _proposal_tspec_comments;
    comments_module_t<"statusc"_n> _proposal_status_comments;
    comments_module_t<"reviewc"_n> _proposal_review_comments;
    voting_module_t<"proposalsrv"_n> _proposal_review_votes;

    eosio::name _app;

protected:
    auto get_state()
    {
        return _state.get();
    }

    void require_app_member(eosio::name account)
    {
        require_auth(account);
        //TODO: eosio_assert(golos.vest::get_balance(account, _app).amount > 0, "app domain member authority is required to do this action");
    }

    void require_app_delegate(eosio::name account)
    {
        require_auth(account);
        //TODO: eosio_assert(golos.ctrl::is_witness(account, _app), "app domain delegate authority is required to do this action");
    }

    const auto get_proposal(proposal_id_t proposal_id)
    {
        auto proposal = _proposals.find(proposal_id);
        eosio_assert(proposal != _proposals.end(), "proposal has not been found");
        return proposal;
    }

    void deposit(proposal_t &proposal) {
        const tspec_data_t &tspec = _proposal_tspecs.get(proposal.tspec_id).data;
        const asset budget = tspec.development_cost + tspec.specification_cost;
        const auto &fund = _funds.get(proposal.fund_name.value);
        LOG("proposal.id: %, budget: %, fund: %", proposal.id, budget, proposal.fund_name);
        eosio_assert(budget <= fund.quantity, "insufficient funds");

        proposal.deposit = budget;
        _funds.modify(fund, name(), [&](auto &obj) {
            obj.quantity -= budget;
        });
    }

    void choose_proposal_tspec(proposal_t & proposal, const tspec_app_t &tspec_app)
    {
        eosio_assert(proposal.type == proposal_t::TYPE_1, "invalid state for choose_proposal_tspec");
        proposal.tspec_id = tspec_app.id;
        proposal.set_state(proposal_t::STATE_TSPEC_CREATE);

        // funds can be deposited in setfund(), if not, it will be deposited here
        if (proposal.deposit.amount == 0)
        {
            deposit(proposal);
        }
    }

    void pay_tspec_author(proposal_t & proposal)
    {
        const tspec_app_t& tspec_app = _proposal_tspecs.get(proposal.tspec_id);
        const tspec_data_t& tspec = tspec_app.data;

        LOG("paying % to %", tspec.specification_cost, ACCOUNT_NAME_CSTR(tspec_app.author));
        proposal.deposit -= tspec.specification_cost;

        action(permission_level{_self, "active"_n},
               TOKEN_ACCOUNT,
               "transfer"_n,
               std::make_tuple(_self, tspec_app.author,
                               tspec.specification_cost,
                               std::string("technical specification reward")))
                .send();
    }

    void enable_worker_reward(proposal_t & proposal)
    {
        proposal.payment_begining_time = TIMESTAMP_NOW;
        proposal.set_state(proposal_t::STATE_PAYMENT);
    }

    void refund(proposal_t & proposal, eosio::name modifier)
    {
        eosio_assert(proposal.deposit.amount > 0, "no funds were deposited");

        const auto &fund = _funds.get(proposal.fund_name.value);
        LOG("% to % fund", proposal.deposit, ACCOUNT_NAME_CSTR(fund.owner));
        _funds.modify(fund, modifier, [&](auto &obj) {
            obj.quantity += proposal.deposit;
        });

        proposal.deposit = ZERO_ASSET;
    }

    void close(proposal_t & proposal)
    {
        proposal.set_state(proposal_t::STATE_CLOSED);
    }

    void del_tspec(const tspec_app_t &tspec_app) {
        _proposal_tspec_votes.erase_all(tspec_app.id);
        _proposal_tspec_comments.erase_all(tspec_app.id);
        _proposal_tspecs.erase(tspec_app);
    }

public:
    worker(eosio::name receiver, eosio::name code, eosio::name app) : contract(receiver, code, eosio::datastream<const char *>(nullptr, 0)),
        _app(app),
        _state(_self, app.value),
        _proposals(_self, app.value),
        _funds(_self, app.value),
        _proposal_comments(_self, app.value),
        _proposal_votes(_self, app.value),
        _proposal_status_comments(_self, app.value),
        _proposal_review_comments(_self, app.value),
        _proposal_review_votes(_self, app.value),
        _proposal_tspecs(_self, app.value),
        _proposal_tspec_comments(_self, app.value),
        _proposal_tspec_votes(_self, app.value){}

    /**
   * @brief createpool creates workers pool in the application domain
   * @param token_symbol application domain name
   */
    [[eosio::action]]
    void createpool(eosio::symbol token_symbol)
    {
        LOG("creating worker's pool: code=\"%\" app=\"%\", token_symbol=\"%\"", name(_self).to_string().c_str(), name{_app}.to_string().c_str(), token_symbol);
        eosio_assert(!_state.exists(), "workers pool is already initialized for the specified app domain");
        require_auth(_app);

        state_t state{.token_symbol = token_symbol};
        _state.set(state, _app);
    }

    /**
   * @brief addpropos publishs a new proposal
   * @param proposal_id a proposal ID
   * @param author author of the new proposal
   * @param title proposal title
   * @param description proposal description
   */
    [[eosio::action]]
    void addpropos(proposal_id_t proposal_id, eosio::name author, string title, string description) {
        require_app_member(author);

        LOG("adding propos % \"%\" by %", proposal_id, title.c_str(), ACCOUNT_NAME_CSTR(author));

        _proposals.emplace(author, [&](auto &o) {
            o.id = proposal_id;
            o.type = proposal_t::TYPE_1;
            o.author = author;
            o.title = title;
            o.description = description;
            o.fund_name = _app;

            o.state = (uint8_t)proposal_t::STATE_TSPEC_APP;
            o.created = TIMESTAMP_NOW;
            o.modified = TIMESTAMP_UNDEFINED;
        });
        LOG("added");
    }

    /**
   * @brief addpropos2 publishs a new proposal for the done work
   * @param proposal_id proposal ID
   * @param author author of the proposal
   * @param title proposal title
   * @param description proposal description
   * @param specification proposal technical specification
   * @param worker the party that did work
   */
    [[eosio::action]]
    void addpropos2(proposal_id_t proposal_id,
               const eosio::name &author,
               const eosio::name &worker,
               const string &title,
               const string &description,
               const tspec_data_t &tspec,
               const comment_id_t comment_id,
               const comment_data_t &comment)
    {
        require_app_member(author);

        LOG("adding propos % \"%\" by %, worker: %", proposal_id, title.c_str(), ACCOUNT_NAME_CSTR(author), ACCOUNT_NAME_CSTR(worker));

        tspec_id_t tspec_id = _proposal_tspecs.available_primary_key();

        _proposals.emplace(author, [&](proposal_t &o) {
            o.id = proposal_id;
            o.type = proposal_t::TYPE_2;
            o.author = author;
            o.title = title;
            o.description = description;
            o.fund_name = _app;
            o.tspec_id = tspec_id;
            o.worker = worker;
            o.work_begining_time = TIMESTAMP_NOW;
            o.worker_payments_count = 0;

            o.created = TIMESTAMP_NOW;
            o.modified = TIMESTAMP_UNDEFINED;

           o.set_state(proposal_t::STATE_DELEGATES_REVIEW);
        });

        _proposal_tspecs.emplace(author, [&](tspec_app_t &obj) {
            obj.id = tspec_id;
            obj.foreign_id = proposal_id;
            obj.author = author;
            obj.data = tspec;
            obj.created = TIMESTAMP_NOW;
            obj.modified = TIMESTAMP_UNDEFINED;
        });

        _proposal_status_comments.add(comment_id, proposal_id, author, comment);
    }

    /**
   * @brief setfund sets a proposal fund
   * @param proposal_id proposal ID
   * @param fund_name the name of the fund: application domain fund (applicatoin domain name) or sponsored fund (account name)
   * @param quantity amount of the tokens that will be deposited
   */
    [[eosio::action]]
    void setfund(proposal_id_t proposal_id, eosio::name fund_name, asset quantity) {
        auto proposal_ptr = _proposals.find(proposal_id);
        eosio_assert(proposal_ptr != _proposals.end(), "proposal has not been found");
        require_app_member(fund_name);
        eosio_assert(get_state().token_symbol == quantity.symbol, "invalid symbol for setfund");
        eosio_assert(proposal_ptr->deposit.amount == 0, "fund is already deposited");
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for setfund");

        const auto &fund = _funds.get(fund_name.value);
        eosio_assert(fund.quantity >= quantity, "insufficient funds");

        _proposals.modify(proposal_ptr, fund_name, [&](auto &o) {
            o.fund_name = fund_name;
            o.deposit = quantity;
        });

        _funds.modify(fund, fund_name, [&](auto &obj) {
            obj.quantity -= quantity;
        });
    }

    /**
   * @brief editpropos modifies proposal
   * @param proposal_id ID of the modified proposal
   * @param title new title, live empty if no changes are needed
   * @param description a new description, live empty if no changes are required
   */
    [[eosio::action]]
    void editpropos(proposal_id_t proposal_id, string title, string description)
    {
        auto proposal_ptr = get_proposal(proposal_id);
        require_app_member(proposal_ptr->author);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for editpropos");
        eosio_assert(!(description.empty() && title.empty()), "invalid arguments");

        _proposals.modify(proposal_ptr, proposal_ptr->author, [&](auto &o) {
            if (!description.empty()) {
                o.description = description;
                o.modified = block_timestamp(now());
            }
            if (!title.empty()) {
                o.title = title;
                o.modified = block_timestamp(now());
            }
        });
    }

    /**
      * @brief delpropos deletes proposal
      * @param proposal_id proposal ID to delete
      */
    [[eosio::action]]
    void delpropos(proposal_id_t proposal_id) {
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for delpropos");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");
        require_app_member(proposal_ptr->author);

        auto tspec_index = _proposal_tspecs.get_index<"foreign"_n>();
        auto tspec_lower_bound = tspec_index.lower_bound(proposal_id);


        for (auto tspec_ptr = tspec_lower_bound; tspec_ptr != tspec_index.upper_bound(proposal_id); tspec_ptr++) {
            eosio_assert(_proposal_tspec_votes.count_positive(tspec_ptr->id) == 0, "proposal contains partly-approved technical specification applications");
        }

        _proposal_comments.erase_all(proposal_id);
        _proposal_review_comments.erase_all(proposal_id);
        _proposal_status_comments.erase_all(proposal_id);
        _proposal_votes.erase_all(proposal_id);

        for (auto tspec_ptr = tspec_lower_bound; tspec_ptr != tspec_index.upper_bound(proposal_id); ) {
            del_tspec(*(tspec_ptr++));
        }

        _proposals.erase(proposal_ptr);
    }

    /**
       * @brief votepropos places a vote for the proposal
       * @param proposal_id proposal ID
       * @param author name of the voting account
       * @param vote 1 for positive vote, 0 for negative vote. Look at the voting_module_t::vote_t
       */
    [[eosio::action]]
    void votepropos(proposal_id_t proposal_id, eosio::name voter, uint8_t positive)
    {
        auto proposal_ptr = _proposals.find(proposal_id);
        eosio_assert(proposal_ptr != _proposals.end(), "proposal has not been found");
        eosio_assert(voting_time_s + proposal_ptr->created.to_time_point().sec_since_epoch() >= now(), "voting time is over");
        require_app_member(voter);

        vote_t vote{
            .foreign_id = proposal_id,
            .voter = voter,
            .positive = positive != 0
        };
        _proposal_votes.vote(vote);
    }

    /**
     * @brief addcomment publish a new comment to the proposal
     * @param proposal_id proposal ID
     * @param comment_id comment ID
     * @param author author of the comment
     * @param data comment data
     */
    [[eosio::action]]
    void addcomment(proposal_id_t proposal_id, comment_id_t comment_id, eosio::name author, const comment_data_t &data) {
        const proposal_t &proposal = _proposals.get(proposal_id);
        eosio_assert(proposal.state != proposal_t::STATE_CLOSED, "invalid state for addcomment");

        LOG("proposal_id: %, comment_id: %, author: %", proposal_id, comment_id, ACCOUNT_NAME_CSTR(author));
        _proposal_comments.add(comment_id, proposal_id, author, data);
    }

    /**
   * @brief editcomment modifies existing comment
   * @param proposal_id proposal ID
   * @param comment_id comment ID
   * @param data comment's data, live empty fileds that shouldn't be modified
   */
    [[eosio::action]]
    void editcomment(comment_id_t comment_id, const comment_data_t &data)
    {
        LOG("comment_id: %", comment_id);

        const proposal_id_t proposal_id = _proposal_comments.comments.get(comment_id).foreign_id;
        const proposal_t& proposal = _proposals.get(proposal_id);
        eosio_assert(proposal.state != proposal_t::STATE_CLOSED, "invalid state for addcomment");

        _proposal_comments.edit(comment_id, data);
    }

    /**
   * @brief delcomment deletes comment
   * @param proposal_id proposal ID
   * @param comment_id comment ID to delete
   */
    [[eosio::action]]
    void delcomment(comment_id_t comment_id) {
        LOG("comment_id: %", comment_id);

        const proposal_id_t proposal_id = _proposal_comments.comments.get(comment_id).foreign_id;
        const proposal_t &proposal = _proposals.get(proposal_id);
        eosio_assert(proposal.state != proposal_t::STATE_CLOSED, "invalid state for addcomment");

        _proposal_comments.del(comment_id);
    }

    /**
   * @brief addtspec publish a new technical specification application
   * @param proposal_id proposal ID
   * @param tspec_id technical speification aplication ID
   * @param author author of the technical specification application
   * @param tspec technical specification details
   */
    [[eosio::action]]
    void addtspec(proposal_id_t proposal_id, tspec_id_t tspec_app_id, eosio::name author, const tspec_data_t &tspec)
    {
        LOG("proposal_id: %, tspec_id: %, author: %", proposal_id, tspec_app_id, ACCOUNT_NAME_CSTR(author));
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for addtspec");

        eosio_assert(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
        eosio_assert(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

        _proposal_tspecs.emplace(author, [&](tspec_app_t &spec) {
            spec.id = tspec_app_id;
            spec.author = author;
            spec.data = tspec;
            spec.foreign_id = proposal_id;
            spec.created = TIMESTAMP_NOW;
            spec.modified = TIMESTAMP_UNDEFINED;
        });
    }

    /**
   * @brief edittspec modifies technical specification application
   * @param proposal_id proposal ID
   * @param tspec_app_id technical specification application ID
   * @param author author of the technical specification
   * @param tspec technical specification details
   */
    [[eosio::action]]
    void edittspec(tspec_id_t tspec_app_id, const tspec_data_t &tspec) {
        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        const proposal_t &proposal = _proposals.get(tspec_app.foreign_id);
        LOG("proposal_id: %, tspec_id: %", proposal.id, tspec_app.id);

        eosio_assert(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state for edittspec");
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");

        eosio_assert(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
        eosio_assert(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

        require_app_member(tspec_app.author);

        _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t &obj) {
            obj.modify(tspec);
        });
    }

    /**
   * @brief deltspec deletes technical specification application
   * @param proposal_id proposal ID
   * @param tspec_app_id technical specification application ID
   */
    [[eosio::action]]
    void deltspec(tspec_id_t tspec_app_id)
    {
        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        const proposal_t &proposal = _proposals.get(tspec_app.foreign_id);
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");
        eosio_assert(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state for deltspec");
        eosio_assert(_proposal_tspec_votes.count_positive(tspec_app_id) == 0, "upvoted technical specification application can be removed");

        require_app_member(tspec_app.author);

        eosio_assert(_proposal_tspec_votes.count_positive(tspec_app.foreign_id) == 0,
                     "technical specification application can't be deleted because it already has been upvoted"); //Technical Specification 1.e

        del_tspec(tspec_app);
    }

    /**
   * @brief approvetspec votes for the technical specification application
   * @param proposal_id proposal ID
   * @param tspec_app_id technical specification application
   * @param author voting account name
   * @param comment_id comment ID of the comment that will be attached as a description to the vote
   * @param comment attached comment data
   */
    [[eosio::action]]
    void approvetspec(tspec_id_t tspec_app_id, eosio::name author, comment_id_t comment_id, const comment_data_t &comment) {
        LOG("tpsec.id: %, author: %, comment.id: % comment.text: %", tspec_app_id, ACCOUNT_NAME_CSTR(author), comment_id, comment.text.c_str());

        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        proposal_id_t proposal_id = tspec_app.foreign_id;
        const proposal_t &proposal = _proposals.get(proposal_id);

        eosio_assert(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state for approvetspec");
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");

        require_app_delegate(author);
        eosio_assert(voting_time_s + tspec_app.created.to_time_point().sec_since_epoch() >= now(), "approve time is over");

        if (!comment.text.empty())
        {
            _proposal_tspec_comments.add(comment_id, tspec_app_id, author, comment);
        }

        _proposal_tspec_votes.approve(tspec_app_id, author);

        const size_t positive_votes_count = _proposal_tspec_votes.count_positive(tspec_app_id);
        if (positive_votes_count >= witness_count_51)
        {
            //TODO: check that all voters are delegates in this moment
            LOG("technical specification % got % positive votes", tspec_app_id, positive_votes_count);
            _proposals.modify(proposal, author, [&](proposal_t &obj) {
                choose_proposal_tspec(obj, tspec_app);
            });
        }
    }

    /**
     * @brief approvetspec unapprove technical specification application
     **/
    [[eosio::action]]
    void dapprovetspec(tspec_id_t tspec_app_id, eosio::name author) {
        LOG("tpsec.id: %, author: %", tspec_app_id, ACCOUNT_NAME_CSTR(author));

        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        proposal_id_t proposal_id = tspec_app.foreign_id;
        const proposal_t &proposal = _proposals.get(proposal_id);

        eosio_assert(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state for dapprovetspec");
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");

        require_auth(author);
        eosio_assert(voting_time_s + tspec_app.created.to_time_point().sec_since_epoch() >= now(), "approve time is over");

        _proposal_tspec_votes.unapprove(tspec_app_id, author);
    }

    /**
   * @brief publishtspec publish a final tehcnical specification
   * @param proposal_id proposal ID
   * @param data technical specification details
   */
    [[eosio::action]]
    void publishtspec(proposal_id_t proposal_id, const tspec_data_t &data)
    {
        LOG("proposal_id: %", proposal_id);
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_CREATE, "invalid state for publishtspec");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        const tspec_app_t& tspec_app = _proposal_tspecs.get(proposal_ptr->tspec_id);
        LOG("tspec_app.id: %, tspec_app.author: %", tspec_app.id, tspec_app.author);
        require_auth(tspec_app.author);

        _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t &obj) {
            obj.data.update(data, true /* limited */);
        });
    }

    /**
   * @brief startwork chooses worker account and allows the worker to start work on the proposal
   * @param proposal_id proposal ID
   * @param worker worker account name
   */
    [[eosio::action]]
    void startwork(proposal_id_t proposal_id, eosio::name worker) {
        LOG("proposal_id: %, worker: %", proposal_id, ACCOUNT_NAME_CSTR(worker));
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_CREATE, "invalid state for startwork");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        const tspec_app_t& tspec_app = _proposal_tspecs.get(proposal_ptr->tspec_id);
        require_auth(tspec_app.author);

        _proposals.modify(proposal_ptr, tspec_app.author, [&](proposal_t &proposal) {
            proposal.worker = worker;
            proposal.work_begining_time = TIMESTAMP_NOW;
            proposal.set_state(proposal_t::STATE_WORK);
        });
    }

    /**
   * @brief cancelwork cancels work. Can be called by the worker or technical specification author
   * @param proposal_id propsal ID
   * @param initiator a cancel initiator's account name
   */
    [[eosio::action]]
    void cancelwork(proposal_id_t proposal_id, eosio::name initiator)
    {
        LOG("proposal_id: %, initiator: %", proposal_id, ACCOUNT_NAME_CSTR(initiator));
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_WORK, "invalid state for cancelwork");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        if (initiator == proposal_ptr->worker)
        {
            require_auth(proposal_ptr->worker);
        }
        else
        {
            const tspec_app_t& tspec_app = _proposal_tspecs.get(proposal_ptr->tspec_id);
            require_auth(tspec_app.author);
        }

        _proposals.modify(proposal_ptr, initiator, [&](proposal_t &proposal) {
            refund(proposal, initiator);
            close(proposal);
        });
    }

    /**
   * @brief poststatus post status for the work done
   * @param proposal_id proposal ID
   * @param comment_id comment ID
   * @param comment comment data
   */
    [[eosio::action]]
    void poststatus(proposal_id_t proposal_id, comment_id_t comment_id, const comment_data_t &comment) {
        LOG("proposal_id: %, comment: %", proposal_id, comment.text.c_str());
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_WORK, "invalid state for poststatus");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");
        require_auth(proposal_ptr->worker);
        _proposal_status_comments.add(comment_id, proposal_ptr->id, proposal_ptr->worker, comment);
    }

    /**
   * @brief acceptwork accepts a work that was done by the worker. Can be called only by the technical specification author
   * @param proposal_id proposal ID
   * @param comment_id comment ID
   * @param comment
   */
    [[eosio::action]]
    void acceptwork(proposal_id_t proposal_id, comment_id_t comment_id, const comment_data_t &comment)
    {
        LOG("proposal_id: %, comment: %", proposal_id, comment.text.c_str());
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_WORK, "invalid state for acceptwork");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        const tspec_app_t& tspec_app = _proposal_tspecs.get(proposal_ptr->tspec_id);
        require_auth(tspec_app.author);

        _proposals.modify(proposal_ptr, tspec_app.author, [&](auto &proposal) {
            proposal.set_state(proposal_t::STATE_DELEGATES_REVIEW);
        });

        _proposal_status_comments.add(comment_id, proposal_ptr->id, tspec_app.author, comment);
    }

    /**
   * @brief reviewwork posts delegate's review
   * @param proposal_id proposal ID
   * @param reviewer delegate's account name
   * @param status 0 - reject, 1 - approve. Look at the proposal_t::review_status_t
   * @param comment_id comemnt id
   * @param comment comment data, live empty if it isn't required
   */
    [[eosio::action]]
    void reviewwork(proposal_id_t proposal_id, eosio::name reviewer, uint8_t status, comment_id_t comment_id, const comment_data_t &comment) {
        LOG("proposal_id: %, comment: %, status: %, reviewer: %", proposal_id, comment.text.c_str(), (int)status, ACCOUNT_NAME_CSTR(reviewer));
        require_app_delegate(reviewer);
        auto proposal_ptr = get_proposal(proposal_id);
        require_app_delegate(reviewer);

        vote_t vote {
            .voter = reviewer,
            .positive = status == proposal_t::STATUS_ACCEPT,
            .foreign_id = proposal_id
        };

        _proposal_review_votes.vote(vote);

        _proposals.modify(proposal_ptr, reviewer, [&](proposal_t &proposal) {
            switch (static_cast<proposal_t::review_status_t>(status))
            {
            case proposal_t::STATUS_REJECT:
            {
                eosio_assert(proposal.state == proposal_t::STATE_DELEGATES_REVIEW ||
                             proposal.state == proposal_t::STATE_WORK,
                             "invalid state for negative review");

                size_t negative_votes_count = _proposal_review_votes.count_negative(proposal_id);
                if (negative_votes_count >= wintess_count_75)
                {
                    //TODO: check that all voters are delegates in this moment
                    LOG("work has been rejected by the delegates voting, got % negative votes", negative_votes_count);
                    refund(proposal, reviewer);
                    close(proposal);
                }
            }
                break;

            case proposal_t::STATUS_ACCEPT:
            {
                eosio_assert(proposal_ptr->state == proposal_t::STATE_DELEGATES_REVIEW, "invalid state for positive review");

                size_t positive_votes_count = _proposal_review_votes.count_positive(proposal_id);
                if (positive_votes_count >= witness_count_51)
                {
                    //TODO: check that all voters are delegates in this moment
                    LOG("work has been accepted by the delegates voting, got % positive votes", positive_votes_count);

                    if (proposal.deposit.amount == 0 && proposal.type == proposal_t::TYPE_2) {
                        deposit(proposal);
                    }

                    pay_tspec_author(proposal);
                    enable_worker_reward(proposal);
                }

                break;
            }
            }
        });
    }

    /**
   * @brief withdraw withdraws scheduled payment to the worker account
   * @param proposal_id proposal id
   */
    [[eosio::action]]
    void withdraw(proposal_id_t proposal_id)
    {
        LOG("proposal_id: %", proposal_id);
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_PAYMENT, "invalid state for withdraw");

        const tspec_app_t& tspec_app = _proposal_tspecs.get(proposal_ptr->tspec_id);
        const tspec_data_t& tspec = tspec_app.data;

        require_auth(proposal_ptr->worker);

        LOG("proposal.worker: %, payments_count: %, payments_interval: %", proposal_ptr->worker,
            static_cast<int>(tspec.payments_count),
            static_cast<int>(tspec.payments_interval));

        require_auth(proposal_ptr->worker);

        asset quantity;

        if (tspec.payments_count == 1)
        {
            quantity = tspec.development_cost;
        }
        else
        {
            const uint32_t payment_epoch = (now() - proposal_ptr->payment_begining_time.to_time_point().sec_since_epoch())
                / tspec.payments_interval;

            LOG("payment epoch: %, interval: %s, worker payments: %",
                payment_epoch, tspec.payments_interval,
                int(proposal_ptr->worker_payments_count));

            eosio_assert(payment_epoch > proposal_ptr->worker_payments_count, "can't withdraw right now");

            quantity = tspec.development_cost / tspec.payments_count;

            if (proposal_ptr->worker_payments_count + 1 == tspec.payments_count)
            {
                quantity += asset(tspec.development_cost.amount % tspec.payments_count, quantity.symbol);
            }
        }

        _proposals.modify(proposal_ptr, proposal_ptr->worker, [&](proposal_t &proposal) {
            proposal.deposit -= quantity;
            proposal.worker_payments_count += 1;

            if (proposal.worker_payments_count == tspec.payments_count)
            {
                close(proposal);
            }
        });

        action(permission_level{_self, "active"_n},
               TOKEN_ACCOUNT, "transfer"_n,
               std::make_tuple(_self, proposal_ptr->worker,
                               quantity, std::string("worker reward")))
                .send();
    }

    // https://tbfleming.github.io/cib/eos.html#gist=d230f3ab2998e8858d3e51af7e4d9aeb
    static void transfer(eosio::name code, transfer_args & t)
    {
        print_f("%(%): transfer % from \"%\" to \"%\"\n", __FUNCTION__, t.memo.c_str(), t.quantity, ACCOUNT_NAME_CSTR(t.from), ACCOUNT_NAME_CSTR(t.to));

        if (t.memo.size() > 13 || t.to.value != current_receiver()) {
            print_f("%(%): skiping transfer\n", __FUNCTION__, t.memo.c_str());
            return;
        }

        worker self(eosio::name(current_receiver()), code, eosio::name(t.memo.c_str()));

        if (t.quantity.symbol != self.get_state().token_symbol) {
            print_f("%(%): invalid symbol code: %, expected: %\n", __FUNCTION__, t.memo.c_str(), t.quantity.symbol, self.get_state().token_symbol);
            return;
        }

        if (t.to != self._self || code != TOKEN_ACCOUNT)
        {
            print_f("%(%): invalid beneficiary or contract code\n", __FUNCTION__, t.memo.c_str());
            return;
        }

        const eosio::name &ram_payer = t.to;

        auto fund = self._funds.find(t.from.value);
        if (fund == self._funds.end())
        {
            self._funds.emplace(ram_payer, [&](auto &fund) {
                fund.owner = t.from;
                fund.quantity = t.quantity;
            });
        }
        else
        {
            self._funds.modify(fund, ram_payer, [&](auto &fund) {
                fund.quantity += t.quantity;
            });
        }

        print_f("%(%): added % credits to % fund", __FUNCTION__, t.memo.c_str(), t.quantity, t.from.to_string().c_str());
    }
};
} // namespace golos

APP_DOMAIN_ABI(golos::worker, (createpool)(addpropos2)(addpropos)(setfund)(editpropos)(delpropos)(votepropos)(addcomment)(editcomment)(delcomment)(addtspec)(edittspec)(deltspec)(approvetspec)(dapprovetspec)(publishtspec)(startwork)(poststatus)(acceptwork)(reviewwork)(cancelwork)(withdraw),
               (transfer))
