#include <iostream>
#include <tuple>
#include <utility>

namespace golos
{

using namespace ::eosio;

template < typename T , typename... Ts >
auto tuple_head( std::tuple<T,Ts...> t )
{
   return  std::get<0>(t);
}

template < std::size_t... Ns , typename... Ts >
auto tail_impl( std::index_sequence<Ns...> , std::tuple<Ts...> t )
{
   return  std::make_tuple( std::get<Ns+1u>(t)... );
}

template < typename... Ts >
auto tuple_tail( std::tuple<Ts...> t )
{
   return  tail_impl( std::make_index_sequence<sizeof...(Ts) - 1u>() , t );
}

template <typename T, typename Q, typename... Args>
bool execute_app_action(uint64_t receiver, uint64_t code, void (Q::*func)(Args...))
{
    size_t size = action_data_size();
    //using malloc/free here potentially is not exception-safe, although WASM doesn't support exceptions
    constexpr size_t max_stack_buffer_size = 512;
    void *buffer = nullptr;
    if (size > 0)
    {
        buffer = max_stack_buffer_size < size ? malloc(size) : alloca(size);
        read_action_data(buffer, size);
    }

    auto args = unpack<std::tuple<std::decay_t<account_name> /* app domain */, std::decay_t<Args>... /* function args */>>((char *)buffer, size);

    if (max_stack_buffer_size < size)
    {
        free(buffer);
    }

    T obj(receiver, tuple_head(args));

    auto f2 = [&](auto... a) {
        (obj.*func)(a...);
    };

    boost::mp11::tuple_apply(f2, tuple_tail(args));
    return true;
}

#define APP_ACTION_API_CALL(r, TYPENAME, elem)                      \
    case ::eosio::string_to_name(BOOST_PP_STRINGIZE(elem)):         \
        ::golos::execute_app_action<TYPENAME>(receiver, code, &TYPENAME::elem);         \
        break;

#define APP_ACTIONS(TYPENAME, MEMBERS)                              \
    BOOST_PP_SEQ_FOR_EACH(APP_ACTION_API_CALL, TYPENAME, MEMBERS)


template <typename... Args>
bool execute_action(uint64_t receiver, uint64_t code, void (*func)(uint64_t, Args...))
{
    size_t size = action_data_size();
    //using malloc/free here potentially is not exception-safe, although WASM doesn't support exceptions
    constexpr size_t max_stack_buffer_size = 512;
    void *buffer = nullptr;
    if (size > 0)
    {
        buffer = max_stack_buffer_size < size ? malloc(size) : alloca(size);
        read_action_data(buffer, size);
    }

    auto args = unpack<std::tuple<std::decay_t<Args>... /* function args */>>((char *)buffer, size);

    if (max_stack_buffer_size < size)
    {
        free(buffer);
    }

    auto f2 = [&](auto... a) {
        (func)(code, a...);
    };

    boost::mp11::tuple_apply(f2, args);
    return true;
}


#define ACTION_API_CALL(r, TYPENAME, elem)                          \
    case ::eosio::string_to_name(BOOST_PP_STRINGIZE(elem)):         \
        ::golos::execute_action(receiver, code, TYPENAME::elem);         \
        break;

#define ACTIONS(TYPENAME, MEMBERS) \
    BOOST_PP_SEQ_FOR_EACH(ACTION_API_CALL, TYPENAME, MEMBERS)



/**
 * when an action is sent to the network, it specifically addresses a single contract account (the "code" account) and an action on that account.
 * However, the contract can use require_recipient to notify another account of the action so that a contract on that account may respond.
 * When it does this, it does not change the "code" account.
 */
#define APP_DOMAIN_ABI(TYPENAME, APP_MEMBERS /* actions that expect app_domain argument */, MEMBERS /* actions that*/)           \
    extern "C"                                                                                                                   \
    {                                                                                                                            \
        void apply(uint64_t receiver, uint64_t code, uint64_t action)                                                            \
        {                                                                                                                        \
            switch (action)                                                                                                      \
            {                                                                                                                    \
                APP_ACTIONS(TYPENAME, APP_MEMBERS)                                                                               \
                ACTIONS(TYPENAME, MEMBERS)                                                                                       \
                                                                                                                                 \
                default:                                                                                                         \
                    eosio_assert(false, "invalid action");                                                                       \
                break;                                                                                                           \
            }                                                                                                                    \
        }                                                                                                                        \
    }                                                                                                                            \
}
