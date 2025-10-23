#pragma once

#if !defined(__EMSCRIPTEN__) && !defined(__wasm__)  && !defined(__wasm32__)  && !defined(__wasm64__)
typedef __int128 int_128;
typedef __int128 int_128;
typedef __int128 int128;
typedef __int128 int128_t;

typedef unsigned __int128 uint_128;
typedef unsigned __int128 uint_128;
typedef unsigned __int128 uint128;
typedef unsigned __int128 uint128_t;

#endif