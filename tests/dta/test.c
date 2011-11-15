/* { dg-options "-O2 -mdta" } */
/* #include <x86intrin.h> */

typedef struct fp{
  int st;
}fp;

void
TREAD ()
{
  int offset;
  int b;
  b = __builtin_ia32_tread(offset);
}

void
TSCHEDULE()
{
  fp *fp0, *fp_ret;
  int SC = 6;
  fp_ret = (fp *)__builtin_ia32_tcreate(fp0,SC);
}

void
TSTORE()
{
  int offset = 5;
  fp *fp;
  int value = 66;
  __builtin_ia32_tstore (value,fp,offset);
}

void
TDESTROY()
{
  __builtin_ia32_tend();
}
