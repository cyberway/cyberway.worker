#include "cyber.worker.hpp"
#include <eosio/event.hpp>
#include <eosio/transaction.hpp>

namespace config = cyber::config;

namespace cyber
{

void worker::require_app_member(eosio::name account) {
    require_auth(account);
}

void worker::require_app_delegate(eosio::name account) {
    require_auth(account);
    //TODO: remove
}

auto worker::get_state() {
    return _state.get();
}

const auto worker::get_proposal(comment_id_t proposal_id) {
    auto proposal = _proposals.find(proposal_id);
    eosio::check(proposal != _proposals.end(), "proposal has not been found");
    return proposal;
}

void worker::deposit(tspec_app_t& tspec_app) {
    const asset budget = tspec_app.data.development_cost + tspec_app.data.specification_cost;
    const auto& fund = _funds.get(tspec_app.fund_name.value);
    eosio::check(budget <= fund.quantity, "insufficient funds");

    tspec_app.deposit = budget;
    _funds.modify(fund, same_payer, [&](auto &obj) {
        obj.quantity -= budget;
    });
}

void worker::refund(tspec_app_t& tspec_app) {
    if (tspec_app.deposit.amount == 0) {
        return;
    }

    const auto& fund = _funds.get(tspec_app.fund_name.value);
    _funds.modify(fund, same_payer, [&](auto &obj) {
        obj.quantity += tspec_app.deposit;
    });

    tspec_app.deposit = ZERO_ASSET;
}

void worker::close_tspec(const tspec_app_t& tspec_app, tspec_app_t::state_t state, const proposal_t& proposal) {
    if (state != tspec_app_t::STATE_PAYMENT_COMPLETE && proposal.state > proposal_t::STATE_TSPEC_APP) {
        _proposals.modify(proposal, same_payer, [&](proposal_t& proposal) {
            proposal.set_state(proposal_t::STATE_TSPEC_APP);
        });
    }
    _tspecs.modify(tspec_app, same_payer, [&](tspec_app_t& tspec) {
        tspec.set_state(state);
        refund(tspec);
    });
    // TODO: do instead of modify
    if (state == tspec_app_t::STATE_CLOSED_BY_AUTHOR
            && _tspec_votes.empty(tspec_app.id) && _tspec_review_votes.empty(tspec_app.id)) {
        _tspecs.erase(tspec_app);
        send_tspecerase_event(tspec_app);
    } else {
        send_tspecstate_event(tspec_app, state);
    }
}

void worker::send_tspecstate_event(const tspec_app_t& tspec_app, tspec_app_t::state_t state) {
    eosio::event(_self, "tspecstate"_n, tspecstate_event{tspec_app.id, static_cast<uint8_t>(state)}).send();
}

void worker::send_tspecerase_event(const tspec_app_t& tspec_app) {
    eosio::event(_self, "tspecerase"_n, tspec_app.id).send();
}

void worker::createpool(eosio::symbol token_symbol) {
    eosio::check(!_state.exists(), "workers pool is already initialized for the specified app domain");
    require_auth(_self);

    state_t state{.token_symbol = token_symbol};
    _state.set(state, _self);
}

void worker::addpropos(comment_id_t proposal_id, name author, uint8_t type) {
    eosio::check(type <= proposal_t::TYPE_DONE, "wrong type");
    require_app_member(author);

    eosio::check(_proposals.find(proposal_id) == _proposals.end(), "already exists");

    CHECK_POST(proposal_id, author);

    _proposals.emplace(author, [&](auto &o) {
        o.id = proposal_id;
        o.type = type;
        o.author = author;
        o.state = (uint8_t)proposal_t::STATE_TSPEC_APP;
        o.created = TIMESTAMP_NOW;
        o.modified = TIMESTAMP_UNDEFINED;
    });
}

void worker::editpropos(comment_id_t proposal_id, uint8_t type) {
    eosio::check(type <= proposal_t::TYPE_DONE, "wrong type");
    auto proposal_ptr = get_proposal(proposal_id);
    require_app_member(proposal_ptr->author);

    CHECK_PROPOSAL_NO_TSPECS((*proposal_ptr));

    _proposals.modify(proposal_ptr, proposal_ptr->author, [&](auto& o) {
        o.type = type;
        o.modified = TIMESTAMP_NOW;
    });
}

void worker::delpropos(comment_id_t proposal_id) {
    auto proposal_ptr = get_proposal(proposal_id);
    require_app_member(proposal_ptr->author);

    CHECK_PROPOSAL_NO_TSPECS((*proposal_ptr));

    _proposals.erase(proposal_ptr);
    _proposal_votes.erase_all(proposal_id);
}

void worker::upvtpropos(comment_id_t proposal_id, name voter) {
	require_app_member(voter);
    auto proposal_ptr = get_proposal(proposal_id);
    eosio::check(eosio::current_time_point().sec_since_epoch() <= proposal_ptr->created + voting_time_s, "voting time is over");
    _proposal_votes.vote(proposal_id, voter, true);
}

void worker::downvtpropos(comment_id_t proposal_id, name voter) {
	require_app_member(voter);
    auto proposal_ptr = get_proposal(proposal_id);
    eosio::check(eosio::current_time_point().sec_since_epoch() <= proposal_ptr->created + voting_time_s, "voting time is over");
    _proposal_votes.vote(proposal_id, voter, false);
}

void worker::unvtpropos(comment_id_t proposal_id, name voter) {
	require_app_member(voter);
    auto proposal_ptr = get_proposal(proposal_id);
    eosio::check(eosio::current_time_point().sec_since_epoch() <= proposal_ptr->created + voting_time_s, "voting time is over");
    _proposal_votes.erase(proposal_id, voter);
}

void worker::addcomment(comment_id_t comment_id, eosio::name author, std::optional<comment_id_t> parent_id, const string& title, const string& body) {
    require_auth(author);
    eosio::check(title.length() <= config::max_comment_title_length, config::err_too_long_title);
    eosio::check(!body.empty(), "body cannot be empty");
    eosio::check(_comments.find(comment_id) == _comments.end(), "already exists");
    eosio::check(!parent_id || _comments.find(*parent_id) != _comments.end(), "parent comment not exists");
    _comments.emplace(author, [&](auto &obj) {
        obj.id = comment_id;
        obj.author = author;
        obj.parent_id = parent_id;
    });
}

void worker::editcomment(comment_id_t comment_id, const string& title, const string& body) {
    eosio::check(title.length() <= config::max_comment_title_length, config::err_too_long_title);
    eosio::check(!body.empty(), "body cannot be empty");
    const auto& comment = _comments.get(comment_id);
    require_auth(comment.author);
}

void worker::delcomment(comment_id_t comment_id) {
    const auto& comment = _comments.get(comment_id);
    require_auth(comment.author);

    auto index = _comments.get_index<name("parent")>();
    eosio::check(index.find(comment_id) == index.end(), "comment has child comments");

    eosio::check(_proposals.find(comment_id) == _proposals.end(), "comment has proposal");
    eosio::check(_tspecs.find(comment_id) == _tspecs.end(), "comment has tspec");
    auto tspec_res_idx = _tspecs.get_index<name("resultc")>();
    eosio::check(tspec_res_idx.find(comment_id) == tspec_res_idx.end(), "comment used as result for tspec");

    _comments.erase(comment);
}

void worker::addtspec(comment_id_t tspec_id, eosio::name author, comment_id_t proposal_id, const tspec_data_t& tspec, std::optional<name> worker) {
    auto proposal_ptr = get_proposal(proposal_id);
    eosio::check(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for addtspec");

    eosio::check(_tspecs.find(tspec_id) == _tspecs.end(), "already exists");

    CHECK_POST(tspec_id, author);

    eosio::check(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
    eosio::check(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

    if (proposal_ptr->type == proposal_t::TYPE_DONE) {
        eosio::check(author == proposal_ptr->author, "you are not proposal author");
        eosio::check(worker.has_value(), "worker not set for done tspec");
    }

    if (worker) {
        eosio::check(is_account(*worker), "worker account not exists");
    } else {
        worker = name();
    }

    _tspecs.emplace(author, [&](auto& o) {
        o.id = tspec_id;
        o.author = author;
        o.set_state(tspec_app_t::STATE_CREATED);
        o.fund_name = _self;
        o.data = tspec;
        o.foreign_id = proposal_id;
        o.worker = *worker;
        o.next_payout = TIMESTAMP_MAX;
        o.created = TIMESTAMP_NOW;
        o.modified = TIMESTAMP_UNDEFINED;
    });
}

void worker::edittspec(comment_id_t tspec_id, const tspec_data_t& tspec, std::optional<name> worker) {
    const auto& tspec_app = _tspecs.get(tspec_id);
    const auto& proposal = _proposals.get(tspec_app.foreign_id);

    eosio::check(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state");

    eosio::check(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
    eosio::check(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

    require_app_member(tspec_app.author);

    if (proposal.type == proposal_t::TYPE_DONE) {
        eosio::check(worker.has_value(), "worker not set for done tspec");
    }

    if (worker) {
        eosio::check(is_account(*worker), "worker account not exists");
    } else {
        worker = name();
    }

    _tspecs.modify(tspec_app, tspec_app.author, [&](auto& o) {
        o.data = tspec;
        o.worker = *worker;
        o.modified = TIMESTAMP_NOW;
    });
}

void worker::deltspec(comment_id_t tspec_id)
{
    const auto& tspec_app = _tspecs.get(tspec_id);
    const auto& proposal = _proposals.get(tspec_app.foreign_id);
    eosio::check(tspec_app.state <= tspec_app_t::STATE_PAYMENT, "techspec already closed");
    eosio::check(tspec_app.state < tspec_app_t::STATE_PAYMENT, "techspec paying, cannot delete");

    require_app_member(tspec_app.author);

    close_tspec(tspec_app, tspec_app_t::STATE_CLOSED_BY_AUTHOR, proposal);
}

void worker::apprtspec(comment_id_t tspec_id, name approver) {
    const auto& tspec = _tspecs.get(tspec_id);
    CHECK_APPROVE_TSPEC(tspec, approver);
    _tspec_votes.vote(tspec_id, approver, true);

    //TODO: check that all voters are delegates in this moment
    if (_tspec_votes.count_positive(tspec_id) < config::witness_count_51) {
        return;
    }
    const auto& proposal = _proposals.get(tspec.foreign_id);
    _proposals.modify(proposal, same_payer, [&](auto& p) {
        p.set_state(proposal_t::STATE_TSPEC_CHOSE);
    });
    _tspecs.modify(tspec, same_payer, [&](auto& tspec) {
        if (proposal.type == proposal_t::TYPE_DONE) {
            tspec.set_state(tspec_app_t::STATE_PAYMENT);
            send_tspecstate_event(tspec, tspec_app_t::STATE_PAYMENT);
            tspec.next_payout = TIMESTAMP_NOW + tspec.data.payments_interval;
        } else if (tspec.worker != name()) {
            tspec.set_state(tspec_app_t::STATE_WORK);
            send_tspecstate_event(tspec, tspec_app_t::STATE_WORK);
            tspec.work_begining_time = TIMESTAMP_NOW;
        } else {
            tspec.set_state(tspec_app_t::STATE_APPROVED);
            send_tspecstate_event(tspec, tspec_app_t::STATE_APPROVED);
        }
        deposit(tspec);
    });
}

void worker::dapprtspec(comment_id_t tspec_id, name approver) {
    const auto& tspec = _tspecs.get(tspec_id);
    CHECK_APPROVE_TSPEC(tspec, approver);
    _tspec_votes.vote(tspec_id, approver, false);

    //TODO: check that all voters are delegates in this moment
    if (_tspec_votes.count_negative(tspec_id) < config::witness_count_75) {
        return;
    }
    const auto& proposal = _proposals.get(tspec.foreign_id);
    close_tspec(tspec, tspec_app_t::STATE_CLOSED_BY_WITNESSES, proposal);
    send_tspecstate_event(tspec, tspec_app_t::STATE_CLOSED_BY_WITNESSES);
}

void worker::unapprtspec(comment_id_t tspec_id, name approver) {
    const auto& tspec = _tspecs.get(tspec_id);
    CHECK_APPROVE_TSPEC(tspec, approver);
    _tspec_votes.erase(tspec_id, approver);
}

void worker::startwork(comment_id_t tspec_id, name worker) {
    const auto& tspec_app = _tspecs.get(tspec_id);
    require_auth(tspec_app.author);
    eosio::check(tspec_app.state == tspec_app_t::STATE_APPROVED, "invalid state for startwork");

    auto proposal_ptr = get_proposal(tspec_app.foreign_id);
    eosio::check(proposal_ptr->type == proposal_t::TYPE_TASK, "unsupported action");

    eosio::check(is_account(worker), "worker account not exists");

    _tspecs.modify(tspec_app, tspec_app.author, [&](auto& tspec) {
        tspec.set_state(tspec_app_t::STATE_WORK);
        tspec.worker = worker;
        tspec.work_begining_time = TIMESTAMP_NOW;
    });
}

void worker::cancelwork(comment_id_t tspec_id, eosio::name initiator) {
    const auto& tspec_app = _tspecs.get(tspec_id);
    eosio::check(tspec_app.state == tspec_app_t::STATE_WORK, "invalid state");

    if (initiator == tspec_app.worker)
    {
        require_auth(tspec_app.worker);
    }
    else
    {
        require_auth(tspec_app.author);
    }

    _tspecs.modify(tspec_app, same_payer, [&](auto& tspec) {
        tspec.set_state(tspec_app_t::STATE_APPROVED);
        tspec.worker = name();
        tspec.work_begining_time = TIMESTAMP_UNDEFINED;
    });
}

void worker::acceptwork(comment_id_t tspec_id, comment_id_t result_comment_id) {
    const auto& tspec_app = _tspecs.get(tspec_id);
    require_auth(tspec_app.author);
    eosio::check(tspec_app.state == tspec_app_t::STATE_WORK || tspec_app.state == tspec_app_t::STATE_WIP, "invalid state");

    CHECK_POST(result_comment_id, tspec_app.author);

    _tspecs.modify(tspec_app, tspec_app.author, [&](auto& tspec) {
        tspec.set_state(tspec_app_t::STATE_DELEGATES_REVIEW);
        tspec.result_comment_id = result_comment_id;
    });
}

void worker::unacceptwork(comment_id_t tspec_id) {
    const auto& tspec_app = _tspecs.get(tspec_id);
    require_auth(tspec_app.author);
    eosio::check(tspec_app.state == tspec_app_t::STATE_DELEGATES_REVIEW, "invalid state");

    _tspecs.modify(tspec_app, tspec_app.author, [&](auto& tspec) {
        tspec.set_state(tspec_app_t::STATE_WIP);
        tspec.result_comment_id.reset();
    });
}

void worker::apprwork(comment_id_t tspec_id, name approver) {
    require_app_delegate(approver);
    const auto& tspec = _tspecs.get(tspec_id);
    eosio::check(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW, "invalid state");
    _tspec_review_votes.vote(tspec_id, approver, true);

    //TODO: check that all voters are delegates in this moment
    if (_tspec_review_votes.count_positive(tspec_id) < config::witness_count_51) {
        return;
    }
    const auto& proposal = _proposals.get(tspec.foreign_id);
    _tspecs.modify(tspec, same_payer, [&](auto& tspec) {
        if (tspec.deposit.amount == 0 && proposal.type == proposal_t::TYPE_DONE) {
            deposit(tspec);
        }

        tspec.set_state(tspec_app_t::STATE_PAYMENT);
        send_tspecstate_event(tspec, tspec_app_t::STATE_PAYMENT);
        tspec.next_payout = TIMESTAMP_NOW + tspec.data.payments_interval;
    });
}

void worker::dapprwork(comment_id_t tspec_id, name approver) {
    require_app_delegate(approver);
    const auto& tspec = _tspecs.get(tspec_id);
    eosio::check(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW || tspec.state == tspec_app_t::STATE_WORK ||
        tspec.state == tspec_app_t::STATE_WIP || tspec.state == tspec_app_t::STATE_PAYMENT, "invalid state");
    _tspec_review_votes.vote(tspec_id, approver, false);

    //TODO: check that all voters are delegates in this moment
    if (_tspec_review_votes.count_negative(tspec_id) < config::witness_count_75) {
        return;
    }
    const auto& proposal = _proposals.get(tspec.foreign_id);
    if (tspec.state == tspec_app_t::STATE_PAYMENT) {
        close_tspec(tspec, tspec_app_t::STATE_DISAPPROVED_BY_WITNESSES, proposal);
        send_tspecstate_event(tspec, tspec_app_t::STATE_DISAPPROVED_BY_WITNESSES);
    } else {
        close_tspec(tspec, tspec_app_t::STATE_CLOSED_BY_WITNESSES, proposal);
        send_tspecstate_event(tspec, tspec_app_t::STATE_CLOSED_BY_WITNESSES);
    }
}

void worker::unapprwork(comment_id_t tspec_id, name approver) {
    require_app_delegate(approver);
    const auto& tspec = _tspecs.get(tspec_id);
    eosio::check(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW || tspec.state == tspec_app_t::STATE_WORK ||
        tspec.state == tspec_app_t::STATE_WIP || tspec.state == tspec_app_t::STATE_PAYMENT, "invalid state");
    _tspec_review_votes.erase(tspec_id, approver);
}

void worker::payout(name ram_payer) {
    auto transfer = [&](const name& to, const asset& payment, const std::string& memo) {
        action(
            permission_level{_self, "active"_n},
            config::token_name, "transfer"_n,
            std::make_tuple(_self, to, payment, memo)
        ).send();
    };

    auto now = TIMESTAMP_NOW;
    auto tspec_idx = _tspecs.get_index<name("payout")>();
    size_t i = 0;
    for (auto tspec_itr = tspec_idx.begin(); tspec_itr != tspec_idx.end() && tspec_itr->next_payout <= now; ++tspec_itr) {
        if (i++ >= config::max_payed_tspecs_per_action) {
            transaction trx(eosio::current_time_point() + eosio::seconds(config::payout_expiration_sec));
            trx.actions.emplace_back(action{permission_level(_self, config::code_name), _self, "payout"_n, std::make_tuple(_self)});
            trx.delay_sec = 0;
            trx.send(static_cast<uint128_t>(config::payout_sender_id) << 64, ram_payer, true);
            break;
        }

        const auto& tspec = tspec_itr->data;
        auto author_payment = tspec.specification_cost / tspec.payments_count;
        auto dev_payment = tspec.development_cost / tspec.payments_count;

        if (tspec_itr->worker_payments_count + 1 == tspec.payments_count) {
            author_payment += asset(tspec.specification_cost.amount % tspec.payments_count, author_payment.symbol);
            dev_payment += asset(tspec.development_cost.amount % tspec.payments_count, dev_payment.symbol);

            tspec_idx.modify(tspec_itr, same_payer, [&](auto& t) {
                t.deposit -= (author_payment + dev_payment);
                t.worker_payments_count += 1;
                t.next_payout = TIMESTAMP_MAX;
                t.set_state(tspec_app_t::STATE_PAYMENT_COMPLETE);
            });
            send_tspecstate_event(*tspec_itr, tspec_app_t::STATE_PAYMENT_COMPLETE);
        } else {
            tspec_idx.modify(tspec_itr, same_payer, [&](auto& t) {
                t.deposit -= (author_payment + dev_payment);
                t.worker_payments_count += 1;
                t.next_payout += tspec.payments_interval;
            });
        }

        transfer(tspec_itr->author, author_payment, "tspec author reward");
        transfer(tspec_itr->worker, dev_payment, "worker reward");
    }
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
        _funds.modify(fund_ptr, same_payer, [&](auto &fund) {
            fund.quantity += quantity;
        });
    }

    payout(_self);

    LOG("added % credits to % fund", quantity, memo.c_str());
}

} // cyber

DISPATCH_WITH_TRANSFER(cyber::worker, config::token_name, on_transfer, (createpool)
    (addpropos)(editpropos)(delpropos)(upvtpropos)(downvtpropos)(unvtpropos)
    (addcomment)(editcomment)(delcomment)
    (addtspec)(edittspec)(deltspec)(apprtspec)(dapprtspec)(unapprtspec)
    (startwork)(cancelwork)(acceptwork)(unacceptwork)(apprwork)(dapprwork)(unapprwork)
    (payout)
)
