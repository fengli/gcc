/* { dg-options "-O2 -mdta" } */
/* #include <x86intrin.h> */

typedef unsigned long long uint64_t;

void
TSCHEDULE()
{
  uint64_t tid_ret;
  void *ip;
  int sc = 6;
  int sz = 100;
  tid_ret = __builtin_tstar_create(ip, sc, sz);
}

void
TDESTROY()
{
  uint64_t tid;
  tid = __builtin_tstar_destroy ();
}

void
TLOAD()
{
  uint64_t tid;
  tid = __builtin_tstar_load ();
}

void
TCACHE ()
{
  uint64_t tid;
  uint64_t localfp;

  localfp = __builtin_tstar_cache (tid);
}

void
DECREASE_N ()
{
  uint64_t n;
  uint64_t tid;

  __builtin_tstar_decreasen (tid, n);
}
