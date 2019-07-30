#include "golos.worker.hpp"

#define CHECK_POST(post_id) { \
	auto post = _comments.find(post_id); \
    eosio::check(post != _comments.end(), "comment not exists"); \
    eosio::check(post->parent_id == COMMENT_ROOT, "comment is not root"); \
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

void worker::deposit(proposal_t& proposal) {
    const tspec_data_t &tspec = _proposal_tspecs.get(proposal.tspec_id).data;
    const asset budget = tspec.development_cost + tspec.specification_cost;
    const auto &fund = _funds.get(proposal.fund_name.value);
    eosio::check(budget <= fund.quantity, "insufficient funds");

    proposal.deposit = budget;
    _funds.modify(fund, name(), [&](auto &obj) {
        obj.quantity -= budget;
    });
}

void worker::choose_proposal_tspec(proposal_t& proposal, const tspec_app_t& tspec_app) {
    eosio::check(proposal.type == proposal_t::TYPE_TASK, "invalid state for choose_proposal_tspec");
    proposal.tspec_id = tspec_app.id;
    proposal.set_state(proposal_t::STATE_TSPEC_CHOSE);
    deposit(proposal);
}

void worker::pay_tspec_author(proposal_t& proposal) {
    const tspec_app_t& tspec_app = _proposal_tspecs.get(proposal.tspec_id);
    const tspec_data_t& tspec = tspec_app.data;

    proposal.deposit -= tspec.specification_cost;

    action(permission_level{_self, "active"_n},
           config::token_name,
           "transfer"_n,
           std::make_tuple(_self, tspec_app.author,
                           tspec.specification_cost,
                           std::string("technical specification reward")))
            .send();
}

void worker::refund(proposal_t& proposal, eosio::name modifier) {
    eosio::check(proposal.deposit.amount > 0, "no funds were deposited");

    const auto &fund = _funds.get(proposal.fund_name.value);
    _funds.modify(fund, modifier, [&](auto &obj) {
        obj.quantity += proposal.deposit;
    });

    proposal.deposit = ZERO_ASSET;
}

void worker::close_tspec(name payer, const tspec_app_t& tspec_app, tspec_app_t::state_t state, const proposal_t& proposal) {
    if (state == tspec_app_t::STATE_CLOSED && proposal.state > proposal_t::STATE_TSPEC_APP) {
        _proposals.modify(proposal, payer, [&](proposal_t& proposal) {
            proposal.set_state(proposal_t::STATE_TSPEC_APP);
            // TODO: clear tspec_id
        });
    }
    _proposal_tspecs.modify(tspec_app, payer, [&](tspec_app_t& tspec) {
        tspec.set_state(state);
    });
}

void worker::del_tspec(const tspec_app_t& tspec_app) {
    _proposal_tspec_votes.erase_all(tspec_app.id);
    _proposal_tspecs.erase(tspec_app);
}

void worker::createpool(eosio::symbol token_symbol) {
    eosio::check(!_state.exists(), "workers pool is already initialized for the specified app domain");
    require_auth(_self);

    state_t state{.token_symbol = token_symbol};
    _state.set(state, _self);
}

void worker::addpropos(proposal_id_t proposal_id, const eosio::name& author, comment_id_t comment) {
    require_app_member(author);

    CHECK_POST(comment);

    _proposals.emplace(author, [&](auto &o) {
        o.id = proposal_id;
        o.type = proposal_t::TYPE_TASK;
        o.author = author;
        o.comment = comment;
        o.fund_name = _self;

        o.state = (uint8_t)proposal_t::STATE_TSPEC_APP;
        o.created = TIMESTAMP_NOW;
        o.modified = TIMESTAMP_UNDEFINED;
    });
}

void worker::addproposdn(proposal_id_t proposal_id, const eosio::name& author, comment_id_t comment, const eosio::name& worker,
        const tspec_data_t& tspec) {
    require_app_member(author);

    CHECK_POST(comment);

    tspec_id_t tspec_id = _proposal_tspecs.available_primary_key();

    _proposals.emplace(author, [&](proposal_t &o) {
        o.id = proposal_id;
        o.type = proposal_t::TYPE_DONE;
        o.author = author;
        o.comment = comment;
        o.fund_name = _self;
        o.tspec_id = tspec_id;
        o.worker = worker;
        o.work_begining_time = TIMESTAMP_NOW;
        o.worker_payments_count = 0;

        o.state = (uint8_t)proposal_t::STATE_TSPEC_CHOSE;
        o.created = TIMESTAMP_NOW;
        o.modified = TIMESTAMP_UNDEFINED;
    });

    _proposal_tspecs.emplace(author, [&](tspec_app_t &obj) {
        obj.id = tspec_id;
        obj.foreign_id = proposal_id;
        obj.author = author;
        obj.comment = comment;
        obj.data = tspec;
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
    eosio::check(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for delpropos");
    eosio::check(proposal_ptr->type == proposal_t::TYPE_TASK, "unsupported action");
    require_app_member(proposal_ptr->author);

    auto tspec_index = _proposal_tspecs.get_index<"foreign"_n>();
    auto tspec_lower_bound = tspec_index.lower_bound(proposal_id);


    for (auto tspec_ptr = tspec_lower_bound; tspec_ptr != tspec_index.upper_bound(proposal_id); tspec_ptr++) {
        eosio::check(_proposal_tspec_votes.count_positive(tspec_ptr->id) == 0, "proposal contains partly-approved technical specification applications");
    }

    _proposal_votes.erase_all(proposal_id);

    for (auto tspec_ptr = tspec_lower_bound; tspec_ptr != tspec_index.upper_bound(proposal_id); ) {
        del_tspec(*(tspec_ptr++));
    }

    _proposals.erase(proposal_ptr);
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

void worker::addcomment(comment_id_t comment_id, eosio::name author, comment_id_t parent_id, const string& text) {
    require_auth(author);
    eosio::check(!text.empty(), "comment cannot be empty");
    eosio::check(comment_id != COMMENT_ROOT && _comments.find(comment_id) == _comments.end(), "comment exists");
    eosio::check(parent_id == COMMENT_ROOT || _comments.find(parent_id) != _comments.end(), "parent comment not exists");
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
    auto ptr = index.lower_bound(comment_id);
    eosio::check(ptr == index.end(), "cannot delete comment with child comments");

    _comments.erase(comment);
}

void worker::addtspec(tspec_id_t tspec_app_id, eosio::name author, proposal_id_t proposal_id, comment_id_t comment, const tspec_data_t& tspec) {
    auto proposal_ptr = get_proposal(proposal_id);
    eosio::check(proposal_ptr->type == proposal_t::TYPE_TASK, "unsupported action");
    eosio::check(proposal_ptr->state == proposal_t::STATE_TSPEC_APP, "invalid state for addtspec");

    CHECK_POST(comment);

    eosio::check(get_state().token_symbol == tspec.specification_cost.symbol, "invalid symbol for the specification cost");
    eosio::check(get_state().token_symbol == tspec.development_cost.symbol, "invalid symbol for the development cost");

    _proposal_tspecs.emplace(author, [&](tspec_app_t &spec) {
        spec.id = tspec_app_id;
        spec.author = author;
        spec.comment = comment;
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
    const tspec_app_t &tspec_app = _proposal_tspecs.get(tspec_app_id);
    const proposal_t &proposal = _proposals.get(tspec_app.foreign_id);
    eosio::check(proposal.type == proposal_t::TYPE_TASK, "unsupported action");
    eosio::check(proposal.state == proposal_t::STATE_TSPEC_APP, "invalid state for deltspec");
    eosio::check(_proposal_tspec_votes.count_positive(tspec_app_id) == 0, "upvoted technical specification application can be removed");

    require_app_member(tspec_app.author);

    eosio::check(_proposal_tspec_votes.count_positive(tspec_app.foreign_id) == 0,
                 "technical specification application can't be deleted because it already has been upvoted"); //Technical Specification 1.e

    del_tspec(tspec_app);
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

    _proposals.modify(proposal_ptr, tspec_app.author, [&](proposal_t &proposal) {
        proposal.worker = worker;
        proposal.work_begining_time = TIMESTAMP_NOW;
    });

    _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](tspec_app_t& tspec) {
        tspec.set_state(tspec_app_t::STATE_WORK);
    });
}

void worker::cancelwork(tspec_id_t tspec_app_id, eosio::name initiator) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    eosio::check(tspec_app.state == tspec_app_t::STATE_WORK, "invalid state for cancelwork");
    auto proposal_ptr = get_proposal(tspec_app.foreign_id);
    eosio::check(proposal_ptr->type == proposal_t::TYPE_TASK, "unsupported action");

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

    close_tspec(initiator, tspec_app, tspec_app_t::STATE_CLOSED, *proposal_ptr);
}

void worker::acceptwork(tspec_id_t tspec_app_id, comment_id_t comment) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    require_auth(tspec_app.author);
    eosio::check(tspec_app.state == tspec_app_t::STATE_WORK, "invalid state for acceptwork");

    auto proposal_ptr = get_proposal(tspec_app.foreign_id);
    eosio::check(proposal_ptr->type == proposal_t::TYPE_TASK, "unsupported action");

    CHECK_POST(comment);

    _proposal_tspecs.modify(tspec_app, tspec_app.author, [&](auto& tspec) {
        tspec.set_state(tspec_app_t::STATE_DELEGATES_REVIEW);
        tspec.result_comment = comment;
    });
}

void worker::reviewwork(tspec_id_t tspec_app_id, eosio::name reviewer, uint8_t status) {
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
        eosio::check(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW ||
                     tspec.state == tspec_app_t::STATE_WORK,
                     "invalid state for negative review");

        size_t negative_votes_count = _tspec_review_votes.count_negative(tspec_app_id);
        if (negative_votes_count >= config::witness_count_75)
        {
            //TODO: check that all voters are delegates in this moment
            LOG("work has been rejected by the delegates voting, got % negative votes", negative_votes_count);
            _proposals.modify(proposal, reviewer, [&](proposal_t& proposal) {
                refund(proposal, reviewer);
            });

            close_tspec(reviewer, tspec, tspec_app_t::STATE_CLOSED, proposal);
        }
    } else {
        eosio::check(tspec.state == tspec_app_t::STATE_DELEGATES_REVIEW, "invalid state for positive review");

        size_t positive_votes_count = _tspec_review_votes.count_positive(tspec_app_id);
        if (positive_votes_count >= config::witness_count_51)
        {
            //TODO: check that all voters are delegates in this moment
            LOG("work has been accepted by the delegates voting, got % positive votes", positive_votes_count);

            _proposals.modify(proposal, reviewer, [&](proposal_t& proposal) {
                if (proposal.deposit.amount == 0 && proposal.type == proposal_t::TYPE_DONE) {
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

void worker::withdraw(tspec_id_t tspec_app_id) {
    const auto& tspec_app = _proposal_tspecs.get(tspec_app_id);
    const auto& tspec = tspec_app.data;
    eosio::check(tspec_app.state == tspec_app_t::STATE_PAYMENT, "invalid state for withdraw");

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
        const uint32_t payment_epoch = (eosio::current_time_point().sec_since_epoch() - proposal_ptr->payment_begining_time)
            / tspec.payments_interval;

        LOG("payment epoch: %, interval: %s, worker payments: %",
            payment_epoch, tspec.payments_interval,
            int(proposal_ptr->worker_payments_count));

        eosio::check(payment_epoch > proposal_ptr->worker_payments_count, "can't withdraw right now");

        quantity = tspec.development_cost / tspec.payments_count;

        if (proposal_ptr->worker_payments_count + 1 == tspec.payments_count)
        {
            quantity += asset(tspec.development_cost.amount % tspec.payments_count, quantity.symbol);
        }
    }

    if (proposal_ptr->worker_payments_count+1 == tspec.payments_count) {
        close_tspec(proposal_ptr->worker, tspec_app, tspec_app_t::STATE_PAYMENT_COMPLETE, *proposal_ptr);
    }

    _proposals.modify(proposal_ptr, proposal_ptr->worker, [&](proposal_t &proposal) {
        proposal.deposit -= quantity;
        proposal.worker_payments_count += 1;
    });

    action(permission_level{_self, "active"_n},
           config::token_name, "transfer"_n,
           std::make_tuple(_self, proposal_ptr->worker,
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
    (addtspec)(edittspec)(deltspec)(approvetspec)(dapprovetspec)(startwork)(acceptwork)(reviewwork)(cancelwork)(withdraw)
)
