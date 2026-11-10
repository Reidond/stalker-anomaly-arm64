/*
** $Id: lmathlib.c,v 1.67.1.1 2007/12/27 13:02:25 roberto Exp $
** Standard mathematical library
** See Copyright Notice in lua.h
*/


#include <stdlib.h>
#include <math.h>
#include <stdint.h>  /* ANOMALY */
#include <time.h>    /* ANOMALY */

#define lmathlib_c
#define LUA_LIB

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


#undef PI
#define PI (3.14159265358979323846)
#define RADIANS_PER_DEGREE (PI/180.0)



static int math_abs (lua_State *L) {
  lua_pushnumber(L, fabs(luaL_checknumber(L, 1)));
  return 1;
}

static int math_sin (lua_State *L) {
  lua_pushnumber(L, sin(luaL_checknumber(L, 1)));
  return 1;
}

static int math_sinh (lua_State *L) {
  lua_pushnumber(L, sinh(luaL_checknumber(L, 1)));
  return 1;
}

static int math_cos (lua_State *L) {
  lua_pushnumber(L, cos(luaL_checknumber(L, 1)));
  return 1;
}

static int math_cosh (lua_State *L) {
  lua_pushnumber(L, cosh(luaL_checknumber(L, 1)));
  return 1;
}

static int math_tan (lua_State *L) {
  lua_pushnumber(L, tan(luaL_checknumber(L, 1)));
  return 1;
}

static int math_tanh (lua_State *L) {
  lua_pushnumber(L, tanh(luaL_checknumber(L, 1)));
  return 1;
}

static int math_asin (lua_State *L) {
  lua_pushnumber(L, asin(luaL_checknumber(L, 1)));
  return 1;
}

static int math_acos (lua_State *L) {
  lua_pushnumber(L, acos(luaL_checknumber(L, 1)));
  return 1;
}

static int math_atan (lua_State *L) {
  lua_pushnumber(L, atan(luaL_checknumber(L, 1)));
  return 1;
}

static int math_atan2 (lua_State *L) {
  lua_pushnumber(L, atan2(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
  return 1;
}

static int math_ceil (lua_State *L) {
  lua_pushnumber(L, ceil(luaL_checknumber(L, 1)));
  return 1;
}

static int math_floor (lua_State *L) {
  lua_pushnumber(L, floor(luaL_checknumber(L, 1)));
  return 1;
}

static int math_fmod (lua_State *L) {
  lua_pushnumber(L, fmod(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
  return 1;
}

static int math_modf (lua_State *L) {
  double ip;
  double fp = modf(luaL_checknumber(L, 1), &ip);
  lua_pushnumber(L, ip);
  lua_pushnumber(L, fp);
  return 2;
}

static int math_sqrt (lua_State *L) {
  lua_pushnumber(L, sqrt(luaL_checknumber(L, 1)));
  return 1;
}

static int math_pow (lua_State *L) {
  lua_pushnumber(L, pow(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
  return 1;
}

static int math_log (lua_State *L) {
  lua_pushnumber(L, log(luaL_checknumber(L, 1)));
  return 1;
}

static int math_log10 (lua_State *L) {
  lua_pushnumber(L, log10(luaL_checknumber(L, 1)));
  return 1;
}

static int math_exp (lua_State *L) {
  lua_pushnumber(L, exp(luaL_checknumber(L, 1)));
  return 1;
}

static int math_deg (lua_State *L) {
  lua_pushnumber(L, luaL_checknumber(L, 1)/RADIANS_PER_DEGREE);
  return 1;
}

static int math_rad (lua_State *L) {
  lua_pushnumber(L, luaL_checknumber(L, 1)*RADIANS_PER_DEGREE);
  return 1;
}

static int math_frexp (lua_State *L) {
  int e;
  lua_pushnumber(L, frexp(luaL_checknumber(L, 1), &e));
  lua_pushinteger(L, e);
  return 2;
}

static int math_ldexp (lua_State *L) {
  lua_pushnumber(L, ldexp(luaL_checknumber(L, 1), luaL_checkint(L, 2)));
  return 1;
}



static int math_min (lua_State *L) {
  int n = lua_gettop(L);  /* number of arguments */
  lua_Number dmin = luaL_checknumber(L, 1);
  int i;
  for (i=2; i<=n; i++) {
    lua_Number d = luaL_checknumber(L, i);
    if (d < dmin)
      dmin = d;
  }
  lua_pushnumber(L, dmin);
  return 1;
}


static int math_max (lua_State *L) {
  int n = lua_gettop(L);  /* number of arguments */
  lua_Number dmax = luaL_checknumber(L, 1);
  int i;
  for (i=2; i<=n; i++) {
    lua_Number d = luaL_checknumber(L, i);
    if (d > dmax)
      dmax = d;
  }
  lua_pushnumber(L, dmax);
  return 1;
}


/*
** ANOMALY: math.random and math.randomseed are LuaJIT 2.1's (lib_math.c and
** lj_prng.c, MIT, Copyright (C) 2005-2025 Mike Pall): a Tausworthe generator
** with period 2^223, one state per Lua state. A seed gives the same numbers as
** under LuaJIT, and the arguments are taken as LuaJIT takes them: as numbers,
** with no interval check (stock 5.1 uses the C library's rand(), truncates
** the bounds to int and raises "interval is empty" when m > n).
*/

typedef struct RandomState { uint64_t u[4]; } RandomState;
typedef union { uint64_t u64; double d; } U64double;

#define TW223_GEN(rs, z, r, i, k, q, s) \
  z = rs->u[i]; \
  z = (((z<<q)^z) >> (k-s)) ^ ((z&((uint64_t)(int64_t)-1 << (64-k)))<<s); \
  r ^= z; rs->u[i] = z;

/* A double bit pattern in the range 1.0 <= d < 2.0. */
static uint64_t random_step (RandomState *rs) {
  uint64_t z, r = 0;
  TW223_GEN(rs, z, r, 0, 63, 31, 18)
  TW223_GEN(rs, z, r, 1, 58, 19, 28)
  TW223_GEN(rs, z, r, 2, 55, 24,  7)
  TW223_GEN(rs, z, r, 3, 47, 21,  8)
  return (r & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
}

static void random_seed (RandomState *rs, double d) {
  uint32_t r = 0x11090601;  /* 64-k[i] as four 8 bit constants. */
  int i;
  for (i = 0; i < 4; i++) {
    U64double u;
    uint32_t m = 1u << (r&255);
    r >>= 8;
    u.d = d = d * 3.14159265358979323846 + 2.7182818284590452354;
    if (u.u64 < m) u.u64 += m;  /* Ensure k[i] MSB of u[i] are non-zero. */
    rs->u[i] = u.u64;
  }
  for (i = 0; i < 10; i++)
    (void)random_step(rs);
}

static int math_random (lua_State *L) {
  RandomState *rs = (RandomState *)lua_touserdata(L, lua_upvalueindex(1));
  int n = lua_gettop(L);
  U64double u;
  double d;
  u.u64 = random_step(rs);
  d = u.d - 1.0;
  if (n > 0) {
    double r1 = luaL_checknumber(L, 1);
    if (n == 1) {
      d = floor(d*r1) + 1.0;  /* d is an int in range [1, r1] */
    } else {
      double r2 = luaL_checknumber(L, 2);
      d = floor(d*(r2-r1+1.0)) + r1;  /* d is an int in range [r1, r2] */
    }
  }  /* else: d is a double in range [0, 1] */
  lua_pushnumber(L, d);
  return 1;
}


static int math_randomseed (lua_State *L) {
  RandomState *rs = (RandomState *)lua_touserdata(L, lua_upvalueindex(1));
  /* LuaJIT seeds from the OS without an argument; the clock will do here */
  random_seed(rs, lua_isnoneornil(L, 1) ? (double)time(NULL) : luaL_checknumber(L, 1));
  return 0;
}


static const luaL_Reg mathlib[] = {
  {"abs",   math_abs},
  {"acos",  math_acos},
  {"asin",  math_asin},
  {"atan2", math_atan2},
  {"atan",  math_atan},
  {"ceil",  math_ceil},
  {"cosh",   math_cosh},
  {"cos",   math_cos},
  {"deg",   math_deg},
  {"exp",   math_exp},
  {"floor", math_floor},
  {"fmod",   math_fmod},
  {"frexp", math_frexp},
  {"ldexp", math_ldexp},
  {"log10", math_log10},
  {"log",   math_log},
  {"max",   math_max},
  {"min",   math_min},
  {"modf",   math_modf},
  {"pow",   math_pow},
  {"rad",   math_rad},
  {"sinh",   math_sinh},
  {"sin",   math_sin},
  {"sqrt",  math_sqrt},
  {"tanh",   math_tanh},
  {"tan",   math_tan},
  {NULL, NULL}
};


/*
** Open math library
*/
LUALIB_API int luaopen_math (lua_State *L) {
  RandomState *rs;
  luaL_register(L, LUA_MATHLIBNAME, mathlib);
  /* ANOMALY: random and randomseed share one generator state (an upvalue) */
  rs = (RandomState *)lua_newuserdata(L, sizeof(RandomState));
  random_seed(rs, 0.0);  /* LuaJIT's fixed initial state */
  lua_pushvalue(L, -1);
  lua_pushcclosure(L, math_random, 1);
  lua_setfield(L, -3, "random");
  lua_pushcclosure(L, math_randomseed, 1);
  lua_setfield(L, -2, "randomseed");
  lua_pushnumber(L, PI);
  lua_setfield(L, -2, "pi");
  lua_pushnumber(L, HUGE_VAL);
  lua_setfield(L, -2, "huge");
#if defined(LUA_COMPAT_MOD)
  lua_getfield(L, -1, "fmod");
  lua_setfield(L, -2, "mod");
#endif
  return 1;
}

