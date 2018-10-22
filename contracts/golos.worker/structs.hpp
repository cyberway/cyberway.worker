#pragma once

#include <vector>
#include <algorithm>

#define EOSLIB_SERIALIZE_DERIVED2( TYPE, BASE ) \
 template<typename DataStream> \
 friend DataStream& operator << ( DataStream& ds, const TYPE& t ){ \
    return ds << static_cast<const BASE&>(t); \
 }\
 template<typename DataStream> \
 friend DataStream& operator >> ( DataStream& ds, TYPE& t ){ \
    return ds >> static_cast<BASE&>(t); \
 }

namespace golos {

using std::vector;

template <typename T>
class set_t : public vector<T>
{
  public:
    using vector<T>::end;
    using vector<T>::begin;
    using vector<T>::erase;
    using vector<T>::push_back;

    bool has(const T &v) const
    {
        return std::find(begin(), end(), v) != end();
    }

    void set(const T &v)
    {
        push_back(v);
    }

    bool unset(const T &v)
    {
        auto i = find(begin(), end(), v);
        if (i != end())
        {
            erase(i);
            return true;
        }
        return false;
    }

    EOSLIB_SERIALIZE_DERIVED2(set_t, vector<T>);
};

template <typename T>
bool contains(vector<T> c, const T &v)
{
  return std::find(c.begin(), c.end(), v) != c.end();
}

}
