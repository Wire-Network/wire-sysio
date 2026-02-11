#pragma once
#include <stdint.h>

namespace fc {

struct unsigned_int {
    using base_uint = uint32_t;
    constexpr unsigned_int( base_uint v = 0 ):value(v){}

    template<typename T>
    constexpr unsigned_int( T v ):value(v){}

    constexpr operator uint32_t()const { return value; }
    //operator uint64_t()const { return value; }

    constexpr unsigned_int& operator=( uint32_t v ) { value = v; return *this; }

    base_uint value;

    friend constexpr bool operator==( const unsigned_int& i, const base_uint& v )     { return i.value == v; }
    friend constexpr bool operator==( const base_uint& i, const unsigned_int& v )     { return i       == v.value; }
    friend constexpr bool operator==( const unsigned_int& i, const unsigned_int& v )  { return i.value == v.value; }

    friend constexpr bool operator!=( const unsigned_int& i, const base_uint& v )     { return i.value != v; }
    friend constexpr bool operator!=( const base_uint& i, const unsigned_int& v )     { return i       != v.value; }
    friend constexpr bool operator!=( const unsigned_int& i, const unsigned_int& v )  { return i.value != v.value; }

    friend constexpr bool operator<( const unsigned_int& i, const base_uint& v )      { return i.value < v; }
    friend constexpr bool operator<( const base_uint& i, const unsigned_int& v )      { return i       < v.value; }
    friend constexpr bool operator<( const unsigned_int& i, const unsigned_int& v )   { return i.value < v.value; }

    friend constexpr bool operator>=( const unsigned_int& i, const base_uint& v )     { return i.value >= v; }
    friend constexpr bool operator>=( const base_uint& i, const unsigned_int& v )     { return i       >= v.value; }
    friend constexpr bool operator>=( const unsigned_int& i, const unsigned_int& v )  { return i.value >= v.value; }
};

constexpr auto format_as(const unsigned_int& ui) { return ui.value; }

/**
 *  @brief serializes a 32 bit signed interger in as few bytes as possible
 *
 *  Uses the google protobuf algorithm for seralizing signed numbers
 */
struct signed_int {
    using base_int = int32_t;
    constexpr signed_int( base_int v = 0 ):value(v){}
    constexpr operator int32_t()const { return value; }
    template<typename T>
    constexpr signed_int& operator=( const T& v ) { value = v; return *this; }
    constexpr signed_int operator++(int) { return value++; }
    constexpr signed_int& operator++(){ ++value; return *this; }

    base_int value;

    friend constexpr bool operator==( const signed_int& i, const base_int& v )    { return i.value == v; }
    friend constexpr bool operator==( const base_int& i, const signed_int& v )    { return i       == v.value; }
    friend constexpr bool operator==( const signed_int& i, const signed_int& v )  { return i.value == v.value; }

    friend constexpr bool operator!=( const signed_int& i, const base_int& v )    { return i.value != v; }
    friend constexpr bool operator!=( const base_int& i, const signed_int& v )    { return i       != v.value; }
    friend constexpr bool operator!=( const signed_int& i, const signed_int& v )  { return i.value != v.value; }

    friend constexpr bool operator<( const signed_int& i, const base_int& v )     { return i.value < v; }
    friend constexpr bool operator<( const base_int& i, const signed_int& v )     { return i       < v.value; }
    friend constexpr bool operator<( const signed_int& i, const signed_int& v )   { return i.value < v.value; }

    friend constexpr bool operator>=( const signed_int& i, const base_int& v )    { return i.value >= v; }
    friend constexpr bool operator>=( const base_int& i, const signed_int& v )    { return i       >= v.value; }
    friend constexpr bool operator>=( const signed_int& i, const signed_int& v )  { return i.value >= v.value; }
};

constexpr auto format_as(const signed_int& si) { return si.value; }

class variant;

void to_variant( const signed_int& var,  variant& vo );
void from_variant( const variant& var,  signed_int& vo );
void to_variant( const unsigned_int& var,  variant& vo );
void from_variant( const variant& var,  unsigned_int& vo );

}  // namespace fc

#include <unordered_map>
namespace std
{
   template<>
   struct hash<fc::signed_int>
   {
       public:
         constexpr size_t operator()(const fc::signed_int &a) const
         {
            return std::hash<fc::signed_int::base_int>()(a.value);
         }
   };
   template<>
   struct hash<fc::unsigned_int>
   {
       public:
         constexpr size_t operator()(const fc::signed_int &a) const
         {
            return std::hash<uint32_t>()(a.value);
         }
   };
}
