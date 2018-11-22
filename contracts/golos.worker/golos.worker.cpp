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
#include <algorithm>

#include "external.hpp"
#include "structs.hpp"

#include "app_dispatcher.hpp"

using namespace eosio;

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
using std::string;

class worker : public contract
{
private:
    static constexpr uint32_t voting_time_s = 7 * 24 * 3600;

    using comment_id_t = uint64_t;
    struct comment_data_t
    {
        string text;

        EOSLIB_SERIALIZE(comment_data_t, (text));
    };
    struct [[eosio::table]] comment_t
    {
        comment_id_t id;
        uint64_t foreign_id;
        eosio::name author;
        comment_data_t data;
        block_timestamp created;
        block_timestamp modified;

        uint64_t primary_key() const { return id; }
        uint64_t get_secondary_1() const { return foreign_id; }

        EOSLIB_SERIALIZE(comment_t, (id)(foreign_id)(author)(data)(created)(modified));
    };

    template <eosio::name::raw TableName>
    struct comments_module_t
    {
        multi_index<TableName, comment_t> comments;

        comments_module_t(eosio::name code, uint64_t scope): comments(code, scope) {}

        void add(comment_id_t id, uint64_t foreign_id, eosio::name author, const comment_data_t &data)
        {
            eosio_assert(comments.find(id) == comments.end(), "comment exists");
            comments.emplace(author, [&](auto &obj) {
                obj = comment_t{ .id = id,
                        .author = author,
                        .data = data,
                        .foreign_id = foreign_id,
                        .created = TIMESTAMP_NOW,
                        .modified = TIMESTAMP_UNDEFINED
            };
            });
        }

        void del(comment_id_t id)
        {
            const auto comment_ptr = comments.find(id);
            eosio_assert(comment_ptr != comments.end(), "comment doens't exist");
            require_auth(comment_ptr->author);
            comments.erase(comment_ptr);
        }

        void edit(comment_id_t id, const comment_data_t &data)
        {
            eosio_assert(!data.text.empty(), "nothing to change");
            auto comment_ptr = comments.find(id);
            eosio_assert(comment_ptr != comments.end(), "comment doesn't exist");
            require_auth(comment_ptr->author);

            comments.modify(comment_ptr, comment_ptr->author, [&](comment_t &obj) {
                obj.data.text = data.text;
            });
        }
    };

    struct [[eosio::table]] vote_t {
        name voter;
        uint64_t foreign_id;
        bool positive;

        uint64_t primary_key() const { return (uint64_t) voter.value; }
        uint64_t get_secondary_1() const { return foreign_id; }

        EOSLIB_SERIALIZE(vote_t, (foreign_id)(voter)(positive));
    };
    template <eosio::name::raw TableName, eosio::name::raw IndexName>
    struct voting_module_t
    {
        multi_index<TableName, vote_t, indexed_by<IndexName, const_mem_fun<vote_t, uint64_t, &vote_t::get_secondary_1>>> votes;

        voting_module_t(eosio::name code, uint64_t scope): votes(code, scope) {}

        void vote(vote_t vote) {
            auto vote_ptr = votes.find(vote.voter.value);
            if (vote_ptr == votes.end()) { //first vote
                votes.emplace(vote.voter, [&](auto &obj) {
                    obj = vote;
                });
            } else {
                votes.modify(vote_ptr, vote.voter, [&](auto &obj) {
                    obj = vote;
                });
            }
        }

        size_t count_positive(uint64_t foreign_id) const {
            auto index = votes.get_index<IndexName>();
            return std::count_if(index.lower_bound(foreign_id), index.upper_bound(foreign_id), [&](const vote_t &vote) {
                return vote.positive;
            });
        }

        size_t count_negative(uint64_t foreign_id) const {
            auto index = votes.get_index<IndexName>();
            return std::count_if(index.lower_bound(foreign_id), index.upper_bound(foreign_id), [&](const vote_t &vote) {
                return !vote.positive;
            });
        }

        size_t count(uint64_t foreign_id) const {
            auto index = votes.get_index<IndexName>();
            return (size_t) index.upper_bound(foreign_id) - index.lower_bound(foreign_id) + 1;
        }
    };

    typedef uint64_t tspec_id_t;
    struct tspec_data_t
    {
        string text;
        asset specification_cost;
        block_timestamp specification_eta;
        asset development_cost;
        block_timestamp development_eta;
        uint8_t payments_count;
        uint32_t payment_interval;

        EOSLIB_SERIALIZE(tspec_data_t, (text)(specification_cost)(specification_eta)(development_cost)(development_eta)(payments_count));

        void update(const tspec_data_t &that)
        {
            if (!that.text.empty())
            {
                text = that.text;
            }
            if (that.specification_cost.amount != 0)
            {
                specification_cost = that.specification_cost;
            }

            if (that.specification_eta != TIMESTAMP_UNDEFINED)
            {
                specification_eta = that.specification_eta;
            }

            if (that.development_cost.amount != 0)
            {
                development_cost = that.development_cost;
            }

            if (that.development_eta != TIMESTAMP_UNDEFINED)
            {
                development_eta = that.development_eta;
            }

            if (that.payments_count != 0)
            {
                payments_count = that.payments_count;
            }
        }
    };
    struct [[eosio::table]] tspec_app_t
    {
        tspec_id_t id;
        tspec_id_t foreign_id;

        eosio::name author;
        tspec_data_t data;

        block_timestamp created;
        block_timestamp modified;

        void modify(const tspec_data_t &that)
        {
            data.update(that);
            modified = TIMESTAMP_NOW;
        }

        uint64_t primary_key() const { return id; }
        uint64_t get_secondary_1() const { return foreign_id; }

        EOSLIB_SERIALIZE(tspec_app_t, (id)(foreign_id)(author)(data)(created)(modified));
    };
    multi_index<"tspecs"_n, tspec_app_t, indexed_by<"index"_n, const_mem_fun<tspec_app_t, uint64_t, &tspec_app_t::get_secondary_1>>> _proposal_tspecs;

    using proposal_id_t = uint64_t;
    struct [[eosio::table]] proposal_t
    {
        enum state_t
        {
            STATE_TSPEC_APP = 1,
            STATE_TSPEC_CREATE,
            STATE_WORK,
            STATE_DELEGATES_REVIEW,
            STATE_PAYMENT,
            STATE_CLOSED
        };

        enum review_status_t
        {
            STATUS_REJECT = 0,
            STATUS_ACCEPT = 1
        };

        enum type_t
        {
            TYPE_1,
            TYPE_2
        };

        proposal_id_t id;
        eosio::name author;
        uint8_t type;
        string title;
        string description;
        eosio::name fund_name;
        asset deposit;
        ///< technical specification author
        eosio::name tspec_author;
        ///< technical specification data
        tspec_data_t tspec;
        ///< perpetrator account name
        eosio::name worker;
        block_timestamp work_begining_time;
        block_timestamp payment_begining_time;
        uint8_t worker_payments_count;
        block_timestamp created;
        block_timestamp modified;
        uint8_t state;

        EOSLIB_SERIALIZE(proposal_t, (id)(author)(type)(title)(description)(fund_name)(deposit)(tspec_author)(tspec)(worker)(work_begining_time)(worker_payments_count)(created)(modified)(state));

        uint64_t primary_key() const { return id; }
        void set_state(state_t new_state) { state = new_state; }
    };
    multi_index<"proposals"_n, proposal_t> _proposals;

    struct [[eosio::table]] state_t
    {
        eosio::symbol token_symbol;
        EOSLIB_SERIALIZE(state_t, (token_symbol));
    };
    singleton<"state"_n, state_t> _state;

    struct [[eosio::table]] fund_t
    {
        eosio::name owner;
        asset quantity;

        EOSLIB_SERIALIZE(fund_t, (owner)(quantity));

        uint64_t primary_key() const { return owner.value; }
    };
    multi_index<"funds"_n, fund_t> _funds;

    comments_module_t<"proposalsc"_n> _proposal_comments;
    voting_module_t<"proposalsv"_n, "proposalsvi"_n> _proposal_votes;
    voting_module_t<"tspecappv"_n, "tspecappvi"_n> _proposal_tspec_votes;
    comments_module_t<"tspecappc"_n> _proposal_tspec_comments;
    comments_module_t<"statusc"_n> _proposal_status_comments;
    comments_module_t<"reviewc"_n> _proposal_reivew_comments;
    voting_module_t<"reviewv"_n, "reviewvi"_n> _proposal_review_votes;

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

    auto get_fund(eosio::name fund_name)
    {
        auto fund_ptr = _funds.find(fund_name.value);
        eosio_assert(fund_ptr != _funds.end(), "fund doesn't exists");
        return fund_ptr;
    }

    void choose_proposal_tspec(proposal_t &proposal, const tspec_app_t &tspec_app, eosio::name modifier)
    {
        if (proposal.deposit.amount == 0)
        {
            const asset budget = tspec_app.data.development_cost + tspec_app.data.specification_cost;
            auto fund = _funds.find(proposal.fund_name.value);
            eosio_assert(fund != _funds.end(), "fund doens't exist");
            LOG("tspec_app: % budget: %, fund: %", tspec_app.id, budget, fund->quantity);
            eosio_assert(budget <= fund->quantity, "insufficient funds");

            proposal.deposit = budget;
            _funds.modify(fund, modifier, [&](auto &fund) {
                fund.quantity -= budget;
            });
        }

        proposal.tspec_author = tspec_app.author;
        proposal.tspec = tspec_app.data;

        if (proposal.type == proposal_t::TYPE_1)
        {
            proposal.set_state(proposal_t::STATE_TSPEC_CREATE);
        }
        else
        {
            proposal.set_state(proposal_t::STATE_DELEGATES_REVIEW);
            proposal.worker = proposal.tspec_author;
        }
    }

    void pay_tspec_author(proposal_t &proposal)
    {

        LOG("paying % to %", proposal.tspec.specification_cost, ACCOUNT_NAME_CSTR(proposal.tspec_author));
        proposal.deposit -= proposal.tspec.specification_cost;

        action(permission_level{_self, "active"_n},
               TOKEN_ACCOUNT,
               "transfer"_n,
               std::make_tuple(_self, proposal.tspec_author,
                               proposal.tspec.specification_cost,
                               std::string("technical specification reward")))
                .send();
    }

    void enable_worker_reward(proposal_t &proposal)
    {
        proposal.payment_begining_time = TIMESTAMP_NOW;
        proposal.set_state(proposal_t::STATE_PAYMENT);
    }

    void refund(proposal_t &proposal, eosio::name modifier)
    {
        eosio_assert(proposal.deposit.amount > 0, "no funds were deposited");

        auto fund_ptr = get_fund(proposal.fund_name);
        LOG("% to % fund", proposal.deposit, ACCOUNT_NAME_CSTR(fund_ptr->owner));
        _funds.modify(fund_ptr, modifier, [&](auto &fund) {
            fund.quantity += proposal.deposit;
        });

        proposal.deposit = ZERO_ASSET;
    }

    void close(proposal_t &proposal)
    {
        proposal.set_state(proposal_t::STATE_CLOSED);
    }

public:
    worker(eosio::name receiver, eosio::name code, eosio::name app) :
        contract(receiver, code, eosio::datastream<const char *>(nullptr, 0)),
        _app(app),
        _state(_self, app.value),
        _proposals(_self, app.value),
        _funds(_self, app.value),
        _proposal_comments(_self, app.value),
        _proposal_votes(_self, app.value),
        _proposal_status_comments(_self, app.value),
        _proposal_reivew_comments(_self, app.value),
        _proposal_review_votes(_self, app.value),
        _proposal_tspecs(_self, app.value),
        _proposal_tspec_comments(_self, app.value),
        _proposal_tspec_votes(_self, app.value)
    {
    }

    /**
   * @brief createpool creates workers pool in the application domain
   * @param token_symbol application domain name
   */
    [[eosio::action]]
    void createpool(eosio::symbol token_symbol)
    {
        LOG("creating worker's pool: code=\"%\" app=\"%\"", name(_self).to_string().c_str(), name{_app}.to_string().c_str());
        eosio_assert(!_state.exists(), "workers pool is already initialized for the specified app domain");
        require_auth(_app);

        _state.set(state_t{.token_symbol = token_symbol}, _app);
    }

    /**
   * @brief addpropos publishs a new proposal
   * @param proposal_id a proposal ID
   * @param author author of the new proposal
   * @param title proposal title
   * @param description proposal description
   */
    [[eosio::action]]
    void addpropos(proposal_id_t proposal_id, eosio::name author, string title, string description)
    {
        require_app_member(author);

        LOG("adding propos % \"%\" by %", proposal_id, title.c_str(), ACCOUNT_NAME_CSTR(author));

        _proposals.emplace(author, [&](auto &o) {
            o.id = proposal_id;
            o.type = proposal_t::TYPE_1;
            o.author = author;
            o.title = title;
            o.description = description;
            o.created = TIMESTAMP_NOW;
            o.modified = TIMESTAMP_UNDEFINED;
            o.state = (uint8_t)proposal_t::STATE_TSPEC_APP;
            o.fund_name = _app;
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
    void addpropos2(proposal_id_t proposal_id, eosio::name author,
                    const string &title, const string &description,
                    tspec_id_t tspec_id, const tspec_data_t &specification, eosio::name worker)
    {
        require_app_member(author);

        LOG("adding propos % \"%\" by %", proposal_id, title.c_str(), name{author}.to_string().c_str());

        _proposals.emplace(author, [&](proposal_t &o) {
            o.id = proposal_id;
            o.type = proposal_t::TYPE_2;
            o.author = author;
            o.title = title;
            o.description = description;
            o.created = TIMESTAMP_NOW;
            o.modified = TIMESTAMP_UNDEFINED;
            o.state = (uint8_t)proposal_t::STATE_TSPEC_APP;
            o.tspec = specification;
            o.fund_name = _app;

            _proposal_tspecs.emplace(author, [&](tspec_app_t &tspec) {
                tspec = {
                    .id = tspec_id,
                    .foreign_id = proposal_id,
                    .author = author,
                    .data = specification,
                    .created = TIMESTAMP_NOW,
                    .modified = TIMESTAMP_UNDEFINED
                };
            });
        });
    }

    /**
   * @brief setfund sets a proposal fund
   * @param proposal_id proposal ID
   * @param fund_name the name of the fund: application domain fund (applicatoin domain name) or sponsored fund (account name)
   * @param quantity amount of the tokens that will be deposited
   */
    [[eosio::action]]
    void setfund(proposal_id_t proposal_id, eosio::name fund_name, asset quantity)
    {
        auto proposal_ptr = _proposals.find(proposal_id);
        eosio_assert(proposal_ptr != _proposals.end(), "proposal has not been found");
        require_app_member(fund_name);

        eosio_assert(proposal_ptr->deposit.amount == 0, "fund is already deposited");
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state " __FILE__ ":" TOSTRING(__LINE__));

        auto fund_ptr = get_fund(fund_name);
        eosio_assert(fund_ptr->quantity >= quantity, "insufficient funds");

        _proposals.modify(proposal_ptr, fund_name, [&](auto &o) {
            o.fund_name = fund_name;
            o.deposit = quantity;
        });

        _funds.modify(fund_ptr, fund_name, [&](auto &fund) {
            fund.quantity -= quantity;
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
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state " __FILE__ ":" TOSTRING(__LINE__));

        _proposals.modify(proposal_ptr, proposal_ptr->author, [&](auto &o) {
            bool modified = false;
            if (!description.empty())
            {
                o.description = description;
                modified = true;
            }
            if (!title.empty())
            {
                o.title = title;
                modified = true;
            }

            if (modified)
            {
                o.modified = block_timestamp(now());
            }
        });
    }

    /**
  * @brief delpropos deletes proposal
  * @param proposal_id proposal ID to delete
  */
    [[eosio::action]]
    void delpropos(proposal_id_t proposal_id)
    {
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state " __FILE__ ":" TOSTRING(__LINE__));
        eosio_assert(_proposal_votes.count_positive(proposal_id) == 0, "proposal has been already upvoted");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        require_app_member(proposal_ptr->author);
        _proposals.erase(proposal_ptr);
    }

    /**
   * @brief votepropos places a vote for the proposal
   * @param proposal_id proposal ID
   * @param author name of the voting account
   * @param vote 1 for positive vote, 0 for negative vote. Look at the voting_module_t::vote_t
   */
    [[eosio::action]]
    void votepropos(proposal_id_t proposal_id, eosio::name author, uint8_t positive)
    {
        auto proposal_ptr = _proposals.find(proposal_id);
        eosio_assert(proposal_ptr != _proposals.end(), "proposal has not been found");
        eosio_assert(voting_time_s + proposal_ptr->created.to_time_point().sec_since_epoch() >= now(), "voting time is over");
        require_app_member(author);

        _proposal_votes.vote(vote_t {
                                 .foreign_id = proposal_id,
                                 .voter = author,
                                 .positive = positive != 0
                             });
    }

    /**
   * @brief addcomment publish a new comment to the proposal
   * @param proposal_id proposal ID
   * @param comment_id comment ID
   * @param author author of the comment
   * @param data comment data
   */
    [[eosio::action]]
    void addcomment(proposal_id_t proposal_id, comment_id_t comment_id, eosio::name author, const comment_data_t &data)
    {
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
        _proposal_comments.edit(comment_id, data);
    }

    /**
   * @brief delcomment deletes comment
   * @param proposal_id proposal ID
   * @param comment_id comment ID to delete
   */
    [[eosio::action]]
    void delcomment(comment_id_t comment_id)
    {
        LOG("comment_id: %", comment_id);
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
    void addtspec(proposal_id_t proposal_id, tspec_id_t tspec_id, eosio::name author, const tspec_data_t &tspec)
    {
        LOG("proposal_id: %, tspec_id: %, author: %", proposal_id, tspec_id, ACCOUNT_NAME_CSTR(author));
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        _proposal_tspecs.emplace(author, [&](tspec_app_t &spec) {
            spec = tspec_app_t {
                .id = tspec_id,
                .author = author,
                .created = TIMESTAMP_NOW,
                .modified = TIMESTAMP_UNDEFINED,
                .data = tspec,
                .foreign_id = proposal_id
            };
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
    void edittspec(tspec_id_t tspec_app_id, const tspec_data_t &tspec_data)
    {
        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        const proposal_t &proposal = _proposals.get(tspec_app.foreign_id);
        LOG("proposal_id: %, tspec_id: %", proposal.id, tspec_app.id);

        eosio_assert(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state " __FILE__ ":" TOSTRING(__LINE__));
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");
        eosio_assert(tspec_data.specification_cost.symbol == get_state().token_symbol, "invalid token symbol");
        eosio_assert(tspec_data.development_cost.symbol == get_state().token_symbol, "invalid token symbol");

        require_app_member(tspec_app.author);
        _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t &obj) {
            obj.data = tspec_data;
        });
    }

    /**
   * @brief deltspec deletes technical specification application
   * @param proposal_id proposal ID
   * @param tspec_app_id technical specification application ID
   */
    [[eosio::action]]
    void deltspec(proposal_id_t proposal_id, tspec_id_t tspec_app_id)
    {
        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        const proposal_t &proposal = _proposals.get(tspec_app.foreign_id);
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");
        require_app_member(tspec_app.author);

        eosio_assert(_proposal_tspec_votes.count_positive(tspec_app.foreign_id) == 0,
                     "technical specification application can't be deleted because it already has been upvoted"); //Technical Specification 1.e

        _proposal_tspecs.erase(tspec_app);
    }

    /**
   * @brief votetspec votes for the technical specification application
   * @param proposal_id proposal ID
   * @param tspec_app_id technical specification application
   * @param author voting account name
   * @param vote 1 - for the positive vote, 0 - for the negative vote. Look at a voting_module_t::vote_t
   * @param comment_id comment ID of the comment that will be attached as a description to the vote
   * @param comment attached comment data
   */
    [[eosio::action]]
    void votetspec(tspec_id_t tspec_app_id, eosio::name author, uint8_t vote, comment_id_t comment_id, const comment_data_t &comment)
    {
        const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
        proposal_id_t proposal_id = tspec_app.foreign_id;

        LOG("proposal_id: %, tpsec_id: %, author: %, vote: %", proposal_id, tspec_app_id, ACCOUNT_NAME_CSTR(author), (int)vote);

        const proposal_t &proposal = _proposals.get(proposal_id);

        eosio_assert(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state " __FILE__ ":" TOSTRING(__LINE__));
        eosio_assert(proposal.type == proposal_t::TYPE_1, "unsupported action");

        require_app_delegate(author);
        eosio_assert(voting_time_s + tspec_app.created.to_time_point().sec_since_epoch() >= now(), "voting time is over");

        if (!comment.text.empty())
        {
            _proposal_tspec_comments.add(comment_id, tspec_app_id, author, comment);
        }

        _proposal_tspec_votes.vote(vote_t {
                                        .voter = author,
                                        .positive = vote != 0,
                                        .foreign_id = tspec_app_id
                                    });

        if (vote != 0 && _proposal_tspec_votes.count_positive(tspec_app_id) >= witness_count_51)
        {
            //TODO: check that all voters are delegates in this moment
            _proposals.modify(proposal, author, [&] (proposal_t &obj) {
                choose_proposal_tspec(obj, tspec_app, author);
            });
        }
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
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_CREATE, "invalid proposal state");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");
        require_auth(proposal_ptr->tspec_author);

        _proposals.modify(proposal_ptr, proposal_ptr->tspec_author, [&](proposal_t &proposal) {
            proposal.tspec.update(data);
        });
    }

    /**
   * @brief startwork chooses worker account and allows the worker to start work on the proposal
   * @param proposal_id proposal ID
   * @param worker worker account name
   */
    [[eosio::action]]
    void startwork(proposal_id_t proposal_id, eosio::name worker)
    {
        LOG("proposal_id: %, worker: %", proposal_id, ACCOUNT_NAME_CSTR(worker));
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_TSPEC_CREATE, "invalid proposal state");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");
        require_auth(proposal_ptr->tspec_author);

        _proposals.modify(proposal_ptr, proposal_ptr->tspec_author, [&](proposal_t &proposal) {
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
        eosio_assert(proposal_ptr->state == proposal_t::STATE_WORK, "invalid proposal state");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");

        if (initiator == proposal_ptr->worker)
        {
            require_auth(proposal_ptr->worker);
        }
        else
        {
            require_auth(proposal_ptr->tspec_author);
        }

        _proposals.modify(proposal_ptr, initiator, [&](proposal_t &proposal) {
            refund(proposal, initiator);
        });
    }

    /**
   * @brief poststatus post status for the work done
   * @param proposal_id proposal ID
   * @param comment_id comment ID
   * @param comment comment data
   */
    [[eosio::action]]
    void poststatus(proposal_id_t proposal_id, comment_id_t comment_id, const comment_data_t &comment)
    {
        LOG("proposal_id: %, comment: %", proposal_id, comment.text.c_str());
        auto proposal_ptr = get_proposal(proposal_id);
        eosio_assert(proposal_ptr->state == proposal_t::STATE_WORK, "invalid proposal state");
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
        eosio_assert(proposal_ptr->state == proposal_t::STATE_DELEGATES_REVIEW, "invalid proposal state");
        eosio_assert(proposal_ptr->type == proposal_t::TYPE_1, "unsupported action");
        require_auth(proposal_ptr->tspec_author);

        _proposals.modify(proposal_ptr, proposal_ptr->tspec_author, [&](auto &proposal) {
            proposal.set_state(proposal_t::STATE_DELEGATES_REVIEW);
        });

        _proposal_status_comments.add(comment_id, proposal_ptr->id, proposal_ptr->tspec_author, comment);
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
    void reviewwork(proposal_id_t proposal_id, eosio::name reviewer, uint8_t status, comment_id_t comment_id, const comment_data_t &comment)
    {
        LOG("proposal_id: %, comment: %, status: %, reviewer: %", proposal_id, comment.text.c_str(), (int) status, ACCOUNT_NAME_CSTR(reviewer));
        require_app_delegate(reviewer);
        auto proposal_ptr = get_proposal(proposal_id);
        require_app_delegate(reviewer);

        _proposal_review_votes.vote(vote_t {
                                        .voter = reviewer,
                                        .positive = status == proposal_t::STATUS_ACCEPT,
                                        .foreign_id = proposal_id
                                    });

        _proposals.modify(proposal_ptr, reviewer, [&](proposal_t &proposal) {
            switch (static_cast<proposal_t::review_status_t>(status))
            {
            case proposal_t::STATUS_REJECT:
            {
                eosio_assert(proposal.state == proposal_t::STATE_DELEGATES_REVIEW ||
                             proposal.state == proposal_t::STATE_WORK,
                             "invalid state " __FILE__ ":" TOSTRING(__LINE__));

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
                eosio_assert(proposal_ptr->state == proposal_t::STATE_DELEGATES_REVIEW, "invalid state " __FILE__ ":" TOSTRING(__LINE__));

                size_t positive_votes_count = _proposal_review_votes.count_positive(proposal_id);
                if (positive_votes_count >= witness_count_51)
                {
                    //TODO: check that all voters are delegates in this moment
                    LOG("work has been accepted by the delegates voting, got % positive votes", positive_votes_count);
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
        eosio_assert(proposal_ptr->state == proposal_t::STATE_PAYMENT, "invalid state " __FILE__ ":" TOSTRING(__LINE__));
        require_auth(proposal_ptr->worker);

        asset quantity;

        if (proposal_ptr->tspec.payments_count == 1)
        {
            quantity = proposal_ptr->tspec.development_cost;
        }
        else
        {
            const uint32_t payment_epoch = (now() - proposal_ptr->payment_begining_time.to_time_point().sec_since_epoch()) / proposal_ptr->tspec.payment_interval;
            LOG("payment epoch: %, interval: %s, worker payments: %", payment_epoch, proposal_ptr->tspec.payment_interval, int(proposal_ptr->worker_payments_count));
            eosio_assert(payment_epoch > proposal_ptr->worker_payments_count, "can't withdraw right now");

            quantity = proposal_ptr->tspec.development_cost / proposal_ptr->tspec.payments_count;

            if (proposal_ptr->worker_payments_count + 1 == proposal_ptr->tspec.payments_count)
            {
                quantity += asset(proposal_ptr->tspec.development_cost.amount % proposal_ptr->tspec.payments_count, quantity.symbol);
            }
        }

        _proposals.modify(proposal_ptr, proposal_ptr->worker, [&](proposal_t &proposal) {
            proposal.deposit -= quantity;
            proposal.worker_payments_count += 1;

            if (proposal.worker_payments_count == proposal.tspec.payments_count)
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
    static void transfer(eosio::name code, transfer_args &t)
    {
        print_f("%: transfer % from \"%\" to \"%\"", __FUNCTION__, t.quantity, ACCOUNT_NAME_CSTR(t.from), ACCOUNT_NAME_CSTR(t.to));

        worker self(eosio::name(current_receiver()), code, eosio::name(t.memo.c_str()));
        if (t.to != self._self || t.quantity.symbol == self.get_state().token_symbol || code != TOKEN_ACCOUNT)
        {
            return;
        }

        const eosio::name &payer = t.from;

        auto fund = self._funds.find(t.from.value);
        if (fund == self._funds.end())
        {
            self._funds.emplace(payer, [&](auto &fund) {
                fund.owner = t.from;
                fund.quantity = t.quantity;
            });
        }
        else
        {
            self._funds.modify(fund, payer, [&](auto &fund) {
                fund.quantity += t.quantity;
            });
        }
    }
};
} // namespace golos

APP_DOMAIN_ABI(golos::worker, (createpool)(addpropos2)(addpropos)(setfund)(editpropos)(delpropos)(votepropos)(addcomment)(editcomment)(delcomment)(addtspec)(edittspec)(deltspec)(votetspec)(publishtspec)(startwork)(poststatus)(acceptwork)(reviewwork)(cancelwork)(withdraw),
               (transfer))
