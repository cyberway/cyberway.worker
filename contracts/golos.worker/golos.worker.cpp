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

using namespace eosio;
using namespace std;

#define TOKEN_ACCOUNT "eosio.token"_n
#define ZERO_ASSET eosio::asset(0, get_state().token_symbol)
#define TIMESTAMP_UNDEFINED block_timestamp(0)
#define TIMESTAMP_NOW block_timestamp(now())

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define ACCOUNT_NAME_CSTR(account_name) eosio::name(account_name).to_string().c_str()
#define LOG(format, ...) print_f("%(%): " format "\n", __FUNCTION__, ACCOUNT_NAME_CSTR(_self), ##__VA_ARGS__);

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
        asset specification_cost;
        uint32_t specification_eta;
        asset development_cost;
        uint32_t development_eta;
        uint16_t payments_count;
        uint32_t payments_interval;

        EOSLIB_SERIALIZE(tspec_data_t,
            (specification_cost)(specification_eta) \
            (development_cost)(development_eta) \
            (payments_count)(payments_interval));

        void update(const tspec_data_t &that, bool limited) {
            bool modified = false;

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
        enum state_t {
            STATE_CREATED = 1,
            STATE_APPROVED,
            STATE_WORK,
            STATE_DELEGATES_REVIEW,
            STATE_PAYMENT,
            STATE_CLOSED
        };

        tspec_id_t id;
        tspec_id_t foreign_id;
        eosio::name author;
        uint8_t state;
        tspec_data_t data;
        block_timestamp created;
        block_timestamp modified;

        EOSLIB_SERIALIZE(tspec_app_t, (id)(foreign_id)(author)(state)(data)(created)(modified));

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
            STATE_TSPEC_CREATE
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
        eosio::name fund_name;
        asset deposit;
        tspec_id_t tspec_id;
        eosio::name worker;
        block_timestamp work_begining_time;
        uint8_t worker_payments_count;
        block_timestamp payment_begining_time;
        block_timestamp created;
        block_timestamp modified;

        EOSLIB_SERIALIZE(proposal_t, (id)(author)(type)(state)\
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
    voting_module_t<"tspecrv"_n> _tspec_review_votes;

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
        deposit(proposal);
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

    void close_tspec(name payer, const tspec_app_t& tspec_app, const proposal_t& proposal) {
        if (proposal.state > proposal_t::STATE_TSPEC_APP) {    
            _proposals.modify(proposal, payer, [&](proposal_t& proposal) {
                proposal.set_state(proposal_t::STATE_TSPEC_APP);
            });
        }
        _proposal_tspecs.modify(tspec_app, payer, [&](tspec_app_t& tspec) {
            tspec.set_state(tspec_app_t::STATE_CLOSED);
        });
    }

    void del_tspec(const tspec_app_t &tspec_app) {
        _proposal_tspec_votes.erase_all(tspec_app.id);
        _proposal_tspec_comments.erase_all(tspec_app.id);
        _proposal_tspecs.erase(tspec_app);
    }
public:
    worker(eosio::name receiver, eosio::name code, eosio::datastream<const char *>& ds) : contract(receiver, code, ds),
        _state(_self, _self.value),
        _proposals(_self, _self.value),
        _funds(_self, _self.value),
        _proposal_comments(_self, _self.value),
        _proposal_votes(_self, _self.value),
        _proposal_status_comments(_self, _self.value),
        _proposal_review_comments(_self, _self.value),
        _tspec_review_votes(_self, _self.value),
        _proposal_tspecs(_self, _self.value),
        _proposal_tspec_comments(_self, _self.value),
        _proposal_tspec_votes(_self, _self.value) {}

    /**
   * @brief createpool creates workers pool in the application domain
   * @param token_symbol application domain name
   */
    [[eosio::action]]
    void createpool(eosio::symbol token_symbol)
    {
        LOG("creating worker's pool: token_symbol=\"%\"", token_symbol);
        eosio_assert(!_state.exists(), "workers pool is already initialized for the specified app domain");
        require_auth(_self);

        state_t state{.token_symbol = token_symbol};
        _state.set(state, _self);
        LOG("created");
    }

    /**
   * @brief addpropos publishs a new proposal
   * @param proposal_id a proposal ID
   * @param author author of the new proposal
   * @param title proposal title
   * @param description proposal description
   */
    [[eosio::action]]
    void addpropos(proposal_id_t proposal_id, const eosio::name& author, const string& title, const string& description) {
        require_app_member(author);

        LOG("adding propos % \"%\" by %", proposal_id, title.c_str(), ACCOUNT_NAME_CSTR(author));

        _proposals.emplace(author, [&](auto &o) {
            o.id = proposal_id;
            o.type = proposal_t::TYPE_1;
            o.author = author;
            o.fund_name = _self;

            o.state = (uint8_t)proposal_t::STATE_TSPEC_APP;
            o.created = TIMESTAMP_NOW;
            o.modified = TIMESTAMP_UNDEFINED;
        });
        LOG("added % % % %", ACCOUNT_NAME_CSTR(_self), ACCOUNT_NAME_CSTR(_code), _proposals.get(proposal_id).id);
    }

    /**
   * @brief addpropos2 publishs a new proposal for the done work
   * @param proposal_id proposal ID
   * @param author author of the proposal
   * @param worker the party that did work
   * @param title proposal title
   * @param description proposal description
   * @param tspec proposal technical specification
   * @param tspec_text technical specification text
   */
    [[eosio::action]]
    void addpropos2(proposal_id_t proposal_id,
               const eosio::name &author,
               const eosio::name &worker,
               const string &title,
               const string &description,
               const tspec_data_t &tspec,
               const string& tspec_text,
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
            o.fund_name = _self;
            o.tspec_id = tspec_id;
            o.worker = worker;
            o.work_begining_time = TIMESTAMP_NOW;
            o.worker_payments_count = 0;

            o.created = TIMESTAMP_NOW;
            o.modified = TIMESTAMP_UNDEFINED;
        });

        _proposal_tspecs.emplace(author, [&](tspec_app_t &obj) {
            obj.id = tspec_id;
            obj.foreign_id = proposal_id;
            obj.author = author;
            obj.data = tspec;
            obj.created = TIMESTAMP_NOW;
            obj.modified = TIMESTAMP_UNDEFINED;

            obj.set_state(tspec_app_t::STATE_DELEGATES_REVIEW);
        });

        _proposal_status_comments.add(comment_id, proposal_id, author, comment);
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
            o.modified = TIMESTAMP_NOW;
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

    // TODO: refactor comments in special issue
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

        LOG("proposal_id: %, comment_id: %, author: %", proposal_id, comment_id, ACCOUNT_NAME_CSTR(author));
        _proposal_comments.add(comment_id, proposal_id, author, data);
    }

   // TODO: refactor comments in special issue
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

        _proposal_comments.edit(comment_id, data);
    }

   // TODO: refactor comments in special issue
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

        _proposal_comments.del(comment_id);
    }

    /**
   * @brief addtspec publish a new technical specification application
   * @param proposal_id proposal ID
   * @param tspec_id technical speification aplication ID
   * @param author author of the technical specification application
   * @param tspec technical specification details
   * @param tspec_text technical specification text
   */
    [[eosio::action]]
    void addtspec(proposal_id_t proposal_id, tspec_id_t tspec_app_id, eosio::name author, const tspec_data_t &tspec, const string& tspec_text)
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
   * @param tspec_text technical specification text
   */
    [[eosio::action]]
    void edittspec(tspec_id_t tspec_app_id, const tspec_data_t &tspec, const string& tspec_text) {
        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        const proposal_t &proposal = _proposals.get(tspec_app.foreign_id);
        LOG("proposal_id: %, tspec_id: %", proposal.id, tspec_app.id);

        eosio_assert(proposal.state == proposal_t::STATE_TSPEC_APP || 
                     proposal.state == proposal_t::STATE_TSPEC_CREATE, "invalid state for edittspec");
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");

        eosio_assert(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
        eosio_assert(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

        require_app_member(tspec_app.author);

        _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t &obj) {
            obj.modify(tspec, proposal.state == proposal_t::STATE_TSPEC_CREATE /* limited */);
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
            _proposal_tspecs.modify(tspec_app, author, [&](tspec_app_t& tspec) {
                tspec.set_state(tspec_app_t::STATE_APPROVED);
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
   * @brief startwork chooses worker account and allows the worker to start work on the tspec
   * @param tspec_app_id techspec application
   * @param worker worker account name
   */
    [[eosio::action]]
    void startwork(tspec_id_t tspec_app_id, eosio::name worker) {
        LOG("tspec_app_id: %, worker: %", tspec_app_id, ACCOUNT_NAME_CSTR(worker));
        const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
        require_auth(tspec_app.author);
        eosio_assert(tspec_app.state == tspec_app_t::STATE_APPROVED, "invalid state for startwork");

        auto proposal_ptr = get_proposal(tspec_app.foreign_id);
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        _proposals.modify(proposal_ptr, tspec_app.author, [&](proposal_t &proposal) {
            proposal.worker = worker;
            proposal.work_begining_time = TIMESTAMP_NOW;
        });

        _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t& tspec) {
            tspec.set_state(tspec_app_t::STATE_WORK);
        });
    }

    /**
   * @brief cancelwork cancels work. Can be called by the worker or technical specification author
   * @param tspec_app_id techspec application
   * @param initiator a cancel initiator's account name
   */
    [[eosio::action]]
    void cancelwork(tspec_id_t tspec_app_id, eosio::name initiator)
    {
        LOG("tspec_app_id: %, initiator: %", tspec_app_id, ACCOUNT_NAME_CSTR(initiator));
        const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
        eosio_assert(tspec_app.state == tspec_app_t::STATE_WORK, "invalid state for cancelwork");
        auto proposal_ptr = get_proposal(tspec_app.foreign_id);
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        if (initiator == proposal_ptr->worker)
        {
            require_auth(proposal_ptr->worker);
        }
        else
        {
            require_auth(tspec_app.author);
        }

        _proposals.modify(proposal_ptr, initiator, [&](proposal_t &proposal) {
            refund(proposal, initiator);
        });

        close_tspec(initiator, tspec_app, *proposal_ptr);
    }

   // TODO: refactor comments in special issue
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
        const auto& tspec_app = _proposal_tspecs.get(proposal_ptr->tspec_id);
        eosio_assert(tspec_app.state == tspec_app_t::STATE_WORK, "invalid state for poststatus");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");
        require_auth(proposal_ptr->worker);
        _proposal_status_comments.add(comment_id, proposal_ptr->id, proposal_ptr->worker, comment);
    }

    /**
   * @brief acceptwork accepts a work that was done by the worker. Can be called only by the technical specification author
   * @param tspec_app_id techspec application
   * @param comment_id comment ID
   * @param comment
   */
    [[eosio::action]]
    void acceptwork(tspec_id_t tspec_app_id, comment_id_t comment_id, const comment_data_t &comment)
    {
        LOG("tspec_app_id: %, comment: %", tspec_app_id, comment.text.c_str());

        const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
        require_auth(tspec_app.author);
        eosio_assert(tspec_app.state == tspec_app_t::STATE_WORK, "invalid state for acceptwork");

        auto proposal_ptr = get_proposal(tspec_app.foreign_id);
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](auto& tspec) {
            tspec.set_state(tspec_app_t::STATE_DELEGATES_REVIEW);
        });

        _proposal_status_comments.add(comment_id, proposal_ptr->id, tspec_app.author, comment);
    }

    /**
   * @brief reviewwork posts delegate's review
   * @param tspec_app_id techspec application
   * @param reviewer delegate's account name
   * @param status 0 - reject, 1 - approve. Look at the proposal_t::review_status_t
   * @param comment_id comemnt id
   * @param comment comment data, live empty if it isn't required
   */
    [[eosio::action]]
    void reviewwork(tspec_id_t tspec_app_id, eosio::name reviewer, uint8_t status, comment_id_t comment_id, const comment_data_t &comment) {
        LOG("tspec_app_id: %, comment: %, status: %, reviewer: %", tspec_app_id, comment.text.c_str(), (int)status, ACCOUNT_NAME_CSTR(reviewer));
        require_app_delegate(reviewer);

        const auto& tspec = _proposal_tspecs.get(tspec_app_id);
        const auto& proposal = _proposals.get(tspec.foreign_id);

        vote_t vote {
            .voter = reviewer,
            .positive = status == proposal_t::STATUS_ACCEPT,
            .foreign_id = tspec_app_id
        };

        _tspec_review_votes.vote(vote);

        if (static_cast<proposal_t::review_status_t>(status) == proposal_t::STATUS_REJECT) {
            eosio_assert(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW ||
                         tspec.state == tspec_app_t::STATE_WORK,
                         "invalid state for negative review");

            size_t negative_votes_count = _tspec_review_votes.count_negative(tspec_app_id);
            if (negative_votes_count >= wintess_count_75)
            {
                //TODO: check that all voters are delegates in this moment
                LOG("work has been rejected by the delegates voting, got % negative votes", negative_votes_count);
                _proposals.modify(proposal, reviewer, [&](proposal_t& proposal) {
                    refund(proposal, reviewer);
                });

                close_tspec(reviewer, tspec, proposal);
            }
        } else {
            eosio_assert(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW, "invalid state for positive review");

            size_t positive_votes_count = _tspec_review_votes.count_positive(tspec_app_id);
            if (positive_votes_count >= witness_count_51)
            {
                //TODO: check that all voters are delegates in this moment
                LOG("work has been accepted by the delegates voting, got % positive votes", positive_votes_count);

                _proposals.modify(proposal, reviewer, [&](proposal_t& proposal) {
                    if (proposal.deposit.amount == 0 && proposal.type == proposal_t::TYPE_2) {
                        deposit(proposal);
                    }

                    pay_tspec_author(proposal);
                    proposal.payment_begining_time = TIMESTAMP_NOW;
                });

                _proposal_tspecs.modify(tspec, reviewer, [&](tspec_app_t& tspec) {
                    tspec.set_state(tspec_app_t::STATE_PAYMENT);
                });
            }
        }
    }

    /**
   * @brief withdraw withdraws scheduled payment to the worker account
   * @param tspec_app_id techspec application
   */
    [[eosio::action]]
    void withdraw(tspec_id_t tspec_app_id)
    {
        LOG("tspec_app_id: %", tspec_app_id);
        const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
        const auto& tspec = tspec_app.data;
        eosio_assert(tspec_app.state == tspec_app_t::STATE_PAYMENT, "invalid state for withdraw");

        auto proposal_ptr = get_proposal(tspec_app.foreign_id);

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

        if (proposal_ptr->worker_payments_count+1 == tspec.payments_count) {
            close_tspec(proposal_ptr->worker, tspec_app, *proposal_ptr);
        }

        _proposals.modify(proposal_ptr, proposal_ptr->worker, [&](proposal_t &proposal) {
            proposal.deposit -= quantity;
            proposal.worker_payments_count += 1;
        });

        action(permission_level{_self, "active"_n},
               TOKEN_ACCOUNT, "transfer"_n,
               std::make_tuple(_self, proposal_ptr->worker,
                               quantity, std::string("worker reward")))
                .send();
    }

    // https://tbfleming.github.io/cib/eos.html#gist=d230f3ab2998e8858d3e51af7e4d9aeb
    void transfer(const transfer_args& t)
    {
        LOG("transfer % from \"%\" to \"%\"\n", t.quantity, ACCOUNT_NAME_CSTR(t.from), ACCOUNT_NAME_CSTR(t.to));

        if (t.memo.size() > 13 || t.to.value != current_receiver()) {
            LOG("skiping transfer\n");
            return;
        }

        if (t.quantity.symbol != get_state().token_symbol) {
            LOG("invalid symbol code: %, expected: %\n", t.quantity.symbol, get_state().token_symbol);
            return;
        }

        if (t.to != _self || get_code() != TOKEN_ACCOUNT) {
            LOG("invalid beneficiary or contract code\n");
            return;
        }

        const eosio::name &ram_payer = t.to;
        const name fund_name = name(t.memo);

        auto fund_ptr = _funds.find(fund_name.value);
        if (fund_ptr == _funds.end()) {
            _funds.emplace(ram_payer, [&](auto &fund) {
                fund.owner = fund_name;
                fund.quantity = t.quantity;
            });
        } else {
            _funds.modify(fund_ptr, ram_payer, [&](auto &fund) {
                fund.quantity += t.quantity;
            });
        }

        LOG("added % credits to % fund", t.quantity, t.memo.c_str());
    }
};
} // namespace golos

extern "C" {
   void apply(uint64_t receiver, uint64_t code, uint64_t action) {
         switch(action) {
            EOSIO_DISPATCH_HELPER(golos::worker, (createpool)(addpropos2)(addpropos)(editpropos)(delpropos)(votepropos)(addcomment)(editcomment)(delcomment)(addtspec)(edittspec)(deltspec)(approvetspec)(dapprovetspec)(startwork)(poststatus)(acceptwork)(reviewwork)(cancelwork)(withdraw)(transfer))
        }
    }
}
