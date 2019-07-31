#include "golos.worker.hpp"

#define CHECK_POST(ID, AUTHOR) { \
    auto post = _comments.find(ID); \
    eosio::check(post != _comments.end(), "comment not exists"); \
    eosio::check(!post->parent_id, "comment is not root"); \
    eosio::check(post->author == AUTHOR, "comment not your"); \
}

namespace config = golos::config;

namespace golos
{

void worker::require_app_member(eosio::name account) {
    require_auth(account);
    //TODO: eosio::check(golos.vest::get_balance(account, _app).amount > 0, "app domain member authority is required to do this action");
}

void worker::require_app_delegate(eosio::name account) {
    require_auth(account);
    //TODO: eosio::check(golos.ctrl::is_witness(account, _app), "app domain delegate authority is required to do this action");
}

auto worker::get_state() {
    return _state.get();
}

const auto worker::get_proposal(proposal_id_t proposal_id) {
    auto proposal = _proposals.find(proposal_id);
    eosio::check(proposal != _proposals.end(), "proposal has not been found");
    return proposal;
}

void worker::deposit(tspec_app_t& tspec_app) {
    const asset budget = tspec_app.data.development_cost + tspec_app.data.specification_cost;
    const auto& fund = _funds.get(tspec_app.fund_name.value);
    eosio::check(budget <= fund.quantity, "insufficient funds");

    tspec_app.deposit = budget;
    _funds.modify(fund, name(), [&](auto &obj) {
        obj.quantity -= budget;
    });
}

void worker::choose_proposal_tspec(proposal_t& proposal, const tspec_app_t& tspec_app) {
    eosio::check(proposal.type == proposal_t::TYPE_TASK, "invalid state for choose_proposal_tspec");
    proposal.set_state(proposal_t::STATE_TSPEC_CHOSE);
}

void worker::pay_tspec_author(tspec_app_t& tspec_app) {
    const auto& tspec = tspec_app.data;

    tspec_app.deposit -= tspec.specification_cost;

    action(permission_level{_self, "active"_n},
           config::token_name,
           "transfer"_n,
           std::make_tuple(_self, tspec_app.author,
                           tspec.specification_cost,
                           std::string("technical specification reward")))
            .send();
}

void worker::refund(tspec_app_t& tspec_app, eosio::name modifier) {
    if (tspec_app.deposit.amount == 0) {
        return;
    }

    const auto& fund = _funds.get(tspec_app.fund_name.value);
    _funds.modify(fund, modifier, [&](auto &obj) {
        obj.quantity += tspec_app.deposit;
    });

    tspec_app.deposit = ZERO_ASSET;
}

void worker::close_tspec(name payer, const tspec_app_t& tspec_app, tspec_app_t::state_t state, const proposal_t& proposal) {
    if (state != tspec_app_t::STATE_PAYMENT_COMPLETE && proposal.state > proposal_t::STATE_TSPEC_APP) {
        _proposals.modify(proposal, payer, [&](proposal_t& proposal) {
            proposal.set_state(proposal_t::STATE_TSPEC_APP);
        });
    }
    _proposal_tspecs.modify(tspec_app, payer, [&](tspec_app_t& tspec) {
        tspec.set_state(state);
        refund(tspec, payer);
    });
    // TODO: do instead of modify
    if (state == tspec_app_t::STATE_CLOSED_BY_AUTHOR
            && _proposal_tspec_votes.empty(tspec_app.id) && _tspec_review_votes.empty(tspec_app.id)) {
        _proposal_tspecs.erase(tspec_app);
    }
}

void worker::createpool(eosio::symbol token_symbol) {
    eosio::check(!_state.exists(), "workers pool is already initialized for the specified app domain");
    require_auth(_self);

    state_t state{.token_symbol = token_symbol};
    _state.set(state, _self);
}

void worker::addpropos(proposal_id_t proposal_id, const eosio::name& author, comment_id_t comment_id) {
    require_app_member(author);

    CHECK_POST(comment_id, author);

    _proposals.emplace(author, [&](auto &o) {
        o.id = proposal_id;
        o.type = proposal_t::TYPE_TASK;
        o.author = author;
        o.comment_id = comment_id;

        o.state = (uint8_t)proposal_t::STATE_TSPEC_APP;
        o.created = TIMESTAMP_NOW;
        o.modified = TIMESTAMP_UNDEFINED;
    });
}

void worker::addproposdn(proposal_id_t proposal_id, const eosio::name& author, comment_id_t comment_id, const eosio::name& worker,
        const tspec_data_t& tspec) {
    require_app_member(author);

    CHECK_POST(comment_id, author);

    tspec_id_t tspec_id = _proposal_tspecs.available_primary_key();

    _proposals.emplace(author, [&](proposal_t &o) {
        o.id = proposal_id;
        o.type = proposal_t::TYPE_DONE;
        o.author = author;
        o.comment_id = comment_id;
        o.state = (uint8_t)proposal_t::STATE_TSPEC_CHOSE;
        o.created = TIMESTAMP_NOW;
        o.modified = TIMESTAMP_UNDEFINED;
    });

    _proposal_tspecs.emplace(author, [&](tspec_app_t &obj) {
        obj.id = tspec_id;
        obj.foreign_id = proposal_id;
        obj.author = author;
        obj.comment_id = comment_id;
        obj.data = tspec;
        obj.fund_name = _self;
        obj.worker = worker;
        obj.work_begining_time = TIMESTAMP_NOW;
        obj.worker_payments_count = 0;
        obj.created = TIMESTAMP_NOW;
        obj.modified = TIMESTAMP_UNDEFINED;

        obj.set_state(tspec_app_t::STATE_DELEGATES_REVIEW);
    });
}

void worker::editpropos(proposal_id_t proposal_id) // TODO: changing type
{
    auto proposal_ptr = get_proposal(proposal_id);
    require_app_member(proposal_ptr->author);
    eosio::check(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for editpropos");

    _proposals.modify(proposal_ptr, proposal_ptr->author, [&](auto &o) {
        o.modified = TIMESTAMP_NOW;
    });
}

void worker::delpropos(proposal_id_t proposal_id) {
    auto proposal_ptr = get_proposal(proposal_id);
    require_app_member(proposal_ptr->author);

    auto tspec_index = _proposal_tspecs.get_index<"foreign"_n>();
    eosio::check(tspec_index.find(proposal_id) == tspec_index.end(), "proposal has tspecs");

    _proposals.erase(proposal_ptr);
    _proposal_votes.erase_all(proposal_id);
}

void worker::votepropos(proposal_id_t proposal_id, eosio::name voter, uint8_t positive) {
    auto proposal_ptr = _proposals.find(proposal_id);
    eosio::check(proposal_ptr != _proposals.end(), "proposal has not been found");
    eosio::check(eosio::current_time_point().sec_since_epoch() <= proposal_ptr->created + voting_time_s, "voting time is over");
    require_app_member(voter);

    vote_t vote{
        .foreign_id = proposal_id,
        .voter = voter,
        .positive = positive != 0
    };
    _proposal_votes.vote(vote);
}

void worker::addcomment(comment_id_t comment_id, eosio::name author, std::optional<comment_id_t> parent_id, const string& text) {
    require_auth(author);
    eosio::check(!text.empty(), "comment cannot be empty");
    eosio::check(_comments.find(comment_id) == _comments.end(), "comment exists");
    eosio::check(!parent_id || _comments.find(*parent_id) != _comments.end(), "parent comment not exists");
    _comments.emplace(author, [&](auto &obj) {
        obj.id = comment_id;
        obj.author = author;
        obj.parent_id = parent_id;
    });
}

void worker::editcomment(comment_id_t comment_id, const string& text) {
    eosio::check(!text.empty(), "comment cannot be empty");
    const auto& comment = _comments.get(comment_id);
    require_auth(comment.author);
}

void worker::delcomment(comment_id_t comment_id) {
    const auto& comment = _comments.get(comment_id);
    require_auth(comment.author);

    auto index = _comments.get_index<name("parent")>();
    eosio::check(index.find(comment_id) == index.end(), "comment has child comments");

    auto prop_idx = _proposals.get_index<name("comment")>();
    eosio::check(prop_idx.find(comment_id) == prop_idx.end(), "comment has proposal");

    auto tspec_idx = _proposal_tspecs.get_index<name("comment")>();
    eosio::check(tspec_idx.find(comment_id) == tspec_idx.end(), "comment has tspec");

    auto tspec_res_idx = _proposal_tspecs.get_index<name("resultc")>();
    eosio::check(tspec_res_idx.find(comment_id) == tspec_res_idx.end(), "comment used as result for tspec");

    _comments.erase(comment);
}

void worker::addtspec(tspec_id_t tspec_app_id, eosio::name author, proposal_id_t proposal_id, comment_id_t comment_id, const tspec_data_t& tspec) {
    auto proposal_ptr = get_proposal(proposal_id);
    eosio::check(proposal_ptr->type == proposal_t::TYPE_TASK, "unsupported action");
    eosio::check(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for addtspec");

    CHECK_POST(comment_id, author);

    eosio::check(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
    eosio::check(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

    _proposal_tspecs.emplace(author, [&](tspec_app_t &spec) {
        spec.id = tspec_app_id;
        spec.author = author;
        spec.comment_id = comment_id;
        spec.fund_name = _self;
        spec.data = tspec;
        spec.foreign_id = proposal_id;
        spec.created = TIMESTAMP_NOW;
        spec.modified = TIMESTAMP_UNDEFINED;
    });
}

void worker::edittspec(tspec_id_t tspec_app_id, const tspec_data_t& tspec) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    const auto& proposal = _proposals.get(tspec_app.foreign_id);

    eosio::check(proposal.state == proposal_t::STATE_TSPEC_APP || 
                 proposal.state == proposal_t::STATE_TSPEC_CHOSE, "invalid state for edittspec");
    eosio::check(proposal.type == proposal_t::TYPE_TASK, "unsupported action");

    eosio::check(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
    eosio::check(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

    require_app_member(tspec_app.author);

    _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t &obj) {
        obj.modify(tspec, proposal.state == proposal_t::STATE_TSPEC_CHOSE /* limited */);
    });
}

void worker::deltspec(tspec_id_t tspec_app_id)
{
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    const auto& proposal = _proposals.get(tspec_app.foreign_id);
    eosio::check(tspec_app.state <= tspec_app_t::STATE_PAYMENT, "techspec already closed");
    eosio::check(tspec_app.state < tspec_app_t::STATE_PAYMENT, "techspec paying, cannot delete");

    require_app_member(tspec_app.author);

    close_tspec(tspec_app.author, tspec_app, tspec_app_t::STATE_CLOSED_BY_AUTHOR, proposal);
}

void worker::approvetspec(tspec_id_t tspec_app_id, eosio::name author) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    auto proposal_id = tspec_app.foreign_id;
    const auto& proposal = _proposals.get(proposal_id);

    eosio::check(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state for approvetspec");
    eosio::check(proposal.type == proposal_t::TYPE_TASK, "unsupported action");

    require_app_delegate(author);
    eosio::check(eosio::current_time_point().sec_since_epoch() <= tspec_app.created + voting_time_s, "approve time is over");

    _proposal_tspec_votes.approve(tspec_app_id, author);

    const size_t positive_votes_count = _proposal_tspec_votes.count_positive(tspec_app_id);
    if (positive_votes_count >= config::witness_count_51)
    {
        //TODO: check that all voters are delegates in this moment
        LOG("technical specification % got % positive votes", tspec_app_id, positive_votes_count);
        _proposals.modify(proposal, author, [&](proposal_t &obj) {
            choose_proposal_tspec(obj, tspec_app);
        });
        _proposal_tspecs.modify(tspec_app, author, [&](tspec_app_t& tspec) {
            tspec.set_state(tspec_app_t::STATE_APPROVED);
            deposit(tspec);
        });
    }
}

void worker::dapprovetspec(tspec_id_t tspec_app_id, eosio::name author) {
    const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
    proposal_id_t proposal_id = tspec_app.foreign_id;
    const proposal_t &proposal = _proposals.get(proposal_id);

    eosio::check(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state for dapprovetspec");
    eosio::check(proposal.type == proposal_t::TYPE_TASK, "unsupported action");

    require_auth(author);
    eosio::check(eosio::current_time_point().sec_since_epoch() <= tspec_app.created + voting_time_s, "approve time is over");

    _proposal_tspec_votes.unapprove(tspec_app_id, author);
}

void worker::startwork(tspec_id_t tspec_app_id, eosio::name worker) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    require_auth(tspec_app.author);
    eosio::check(tspec_app.state == tspec_app_t::STATE_APPROVED, "invalid state for startwork");

    auto proposal_ptr = get_proposal(tspec_app.foreign_id);
    eosio::check(proposal_ptr->type == proposal_t::TYPE_TASK, "unsupported action");

    _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t& tspec) {
        tspec.set_state(tspec_app_t::STATE_WORK);
        tspec.worker = worker;
        tspec.work_begining_time = TIMESTAMP_NOW;
    });
}

void worker::cancelwork(tspec_id_t tspec_app_id, eosio::name initiator) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    eosio::check(tspec_app.state == tspec_app_t::STATE_WORK || tspec_app.state == tspec_app_t::STATE_WIP, "invalid state for cancelwork");

    if (initiator == tspec_app.worker)
    {
        require_auth(tspec_app.worker);
    }
    else
    {
        require_auth(tspec_app.author);
    }

    // TODO: cancelwork fixed in #51
    _proposal_tspecs.modify(tspec_app, initiator, [&](auto& tspec) {
        refund(tspec, initiator);
    });

    auto proposal_ptr = get_proposal(tspec_app.foreign_id);
    close_tspec(initiator, tspec_app, tspec_app_t::STATE_CLOSED_BY_WITNESSES, *proposal_ptr);
}

void worker::acceptwork(tspec_id_t tspec_app_id, comment_id_t comment_id) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    require_auth(tspec_app.author);
    eosio::check(tspec_app.state == tspec_app_t::STATE_WORK || tspec_app.state == tspec_app_t::STATE_WIP, "invalid state");

    CHECK_POST(comment_id, tspec_app.author);

    _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](auto& tspec) {
        tspec.set_state(tspec_app_t::STATE_DELEGATES_REVIEW);
        tspec.result_comment_id = comment_id;
    });
}

void worker::unacceptwork(tspec_id_t tspec_app_id) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    require_auth(tspec_app.author);
    eosio::check(tspec_app.state == tspec_app_t::STATE_DELEGATES_REVIEW, "invalid state");

    _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](auto& tspec) {
        tspec.set_state(tspec_app_t::STATE_WIP);
        tspec.result_comment_id = 0;
    });
}

void worker::reviewwork(tspec_id_t tspec_app_id, eosio::name reviewer, uint8_t status) {
    require_app_delegate(reviewer);

    const auto& tspec = _proposal_tspecs.get(tspec_app_id);

    vote_t vote {
        .voter = reviewer,
        .positive = status == tspec_app_t::STATUS_ACCEPT,
        .foreign_id = tspec_app_id
    };

    _tspec_review_votes.vote(vote);

    const auto& proposal = _proposals.get(tspec.foreign_id);

    if (static_cast<tspec_app_t::review_status_t>(status) == tspec_app_t::STATUS_REJECT) {
        eosio::check(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW ||
                     tspec.state == tspec_app_t::STATE_WORK ||
                     tspec.state == tspec_app_t::STATE_WIP ||
                     tspec.state == tspec_app_t::STATE_PAYMENT,
                     "invalid state for negative review");

        size_t negative_votes_count = _tspec_review_votes.count_negative(tspec_app_id);
        if (negative_votes_count >= config::witness_count_75)
        {
            //TODO: check that all voters are delegates in this moment
            LOG("work has been rejected by the delegates voting, got % negative votes", negative_votes_count);

            if (tspec.state == tspec_app_t::STATE_PAYMENT) {
                close_tspec(reviewer, tspec, tspec_app_t::STATE_DISAPPROVED_BY_WITNESSES, proposal);
            } else {
                close_tspec(reviewer, tspec, tspec_app_t::STATE_CLOSED_BY_WITNESSES, proposal);
            }
        }
    } else if (static_cast<tspec_app_t::review_status_t>(status) == tspec_app_t::STATUS_ACCEPT) {
        eosio::check(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW, "invalid state for positive review");

        size_t positive_votes_count = _tspec_review_votes.count_positive(tspec_app_id);
        if (positive_votes_count >= config::witness_count_51)
        {
            //TODO: check that all voters are delegates in this moment
            LOG("work has been accepted by the delegates voting, got % positive votes", positive_votes_count);

            _proposal_tspecs.modify(tspec, reviewer, [&](auto& tspec) {
                if (tspec.deposit.amount == 0 && proposal.type == proposal_t::TYPE_DONE) {
                    deposit(tspec);
                }

                tspec.set_state(tspec_app_t::STATE_PAYMENT); 
                pay_tspec_author(tspec);
                tspec.payment_begining_time = TIMESTAMP_NOW;
            });
        }
    } else {
        eosio::check(false, "wrong status");
    }
}

void worker::withdraw(tspec_id_t tspec_app_id) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    const auto& tspec = tspec_app.data;
    eosio::check(tspec_app.state == tspec_app_t::STATE_PAYMENT, "invalid state for withdraw");

    require_auth(tspec_app.worker);

    LOG("tspec_app.worker: %, payments_count: %, payments_interval: %", tspec_app.worker,
        static_cast<int>(tspec.payments_count),
        static_cast<int>(tspec.payments_interval));

    asset quantity;

    if (tspec.payments_count == 1)
    {
        quantity = tspec.development_cost;
    }
    else
    {
        const uint32_t payment_epoch = (eosio::current_time_point().sec_since_epoch() - tspec_app.payment_begining_time)
            / tspec.payments_interval;

        LOG("payment epoch: %, interval: %s, worker payments: %",
            payment_epoch, tspec.payments_interval,
            int(tspec_app.worker_payments_count));

        eosio::check(payment_epoch > tspec_app.worker_payments_count, "can't withdraw right now");

        quantity = tspec.development_cost / tspec.payments_count;

        if (tspec_app.worker_payments_count + 1 == tspec.payments_count)
        {
            quantity += asset(tspec.development_cost.amount % tspec.payments_count, quantity.symbol);
        }
    }

    if (tspec_app.worker_payments_count+1 == tspec.payments_count) {
        auto proposal_ptr = get_proposal(tspec_app.foreign_id);
        close_tspec(tspec_app.worker, tspec_app, tspec_app_t::STATE_PAYMENT_COMPLETE, *proposal_ptr);
    }

    _proposal_tspecs.modify(tspec_app, tspec_app.worker, [&](auto& t) {
        t.deposit -= quantity;
        t.worker_payments_count += 1;
    });

    action(permission_level{_self, "active"_n},
           config::token_name, "transfer"_n,
           std::make_tuple(_self, tspec_app.worker,
                           quantity, std::string("worker reward")))
            .send();
}

void worker::on_transfer(name from, name to, eosio::asset quantity, std::string memo) {
    if (to != _self) {
        return;
    }
    // Can be filled by emission (memo == "emission") or someone's transfer

    if (memo.size() > 13) {
        LOG("skiping transfer\n");
        return;
    }

    if (quantity.symbol != get_state().token_symbol) {
        LOG("invalid symbol code: %, expected: %\n", quantity.symbol, get_state().token_symbol);
        return;
    }

    const auto fund_name = name(memo);

    auto fund_ptr = _funds.find(fund_name.value);
    if (fund_ptr == _funds.end()) {
        _funds.emplace(_self, [&](auto &fund) {
            fund.owner = fund_name;
            fund.quantity = quantity;
        });
    } else {
        _funds.modify(fund_ptr, eosio::same_payer, [&](auto &fund) {
            fund.quantity += quantity;
        });
    }

    LOG("added % credits to % fund", quantity, memo.c_str());
}

} // golos

DISPATCH_WITH_TRANSFER(golos::worker, config::token_name, on_transfer, (createpool)
    (addproposdn)(addpropos)(editpropos)(delpropos)(votepropos)
    (addcomment)(editcomment)(delcomment)
    (addtspec)(edittspec)(deltspec)(approvetspec)(dapprovetspec)(startwork)(acceptwork)(unacceptwork)(reviewwork)(cancelwork)(withdraw)
)
