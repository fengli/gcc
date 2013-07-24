#include "tsu.h"
#include <stdio.h>

#define RECFIB

// reference version
inline uint64_t fastfib(int n)
{
#ifdef RECFIB
    return n < 2 ? n : fastfib(n-1) + fastfib(n-2);
#else
    uint64_t a=0,b=1; 
    int i=0;
    for(; i < n; ++i) {
        uint64_t t = a + b; 
        a = b;
        b = t; 
    }
    return a;
#endif
}

void adder(void)
{
    const uint64_t* fp=df_tload();
    uint64_t xloc = fp[0];
    uint64_t *xp = df_tcache(xloc);
    xp[_TLOC_OFF(xloc)] = fp[1]+fp[2];
    df_tdecrease(xloc);
    df_destroy();
}

void fib(void)
{
    const uint64_t* fp=df_tload();
    int n = fp[1]; // receive n
    uint64_t xloc = fp[0]; // target location

    if (n < 20) {
        uint64_t *tp = df_tcache(xloc);
        tp[_TLOC_OFF(xloc)] = fastfib(n);
        df_tdecrease(xloc);
    }
    else {
        uint64_t xadd = df_tschedule64(&adder,3); // spawn adder
        uint64_t* tadd = df_tcache(xadd);
        tadd[0]=xloc; // add.dst is this.dst
        df_tdecrease(xadd);

        uint64_t xfib1 = df_tschedule64(&fib,2);  // spawn fib1
        uint64_t* tfib1 = df_tcache(xfib1);
        tfib1[0]=_TLOC(xadd,1); // fib1.dst is add[1]
        tfib1[1]= n-1; // send fib1, n-1
        df_tdecreaseN(xfib1,2);

        uint64_t xfib2 = df_tschedule64(&fib,2);  // spawn fib2
        uint64_t* tfib2 = df_tcache(xfib2);
        tfib2[0]=_TLOC(xadd,2); // fib2.dst is add[2]
        tfib2[1]= n-2; // send fib2, n-2
        df_tdecreaseN(xfib2,2);
    }
    df_destroy();
}


int nn=0; // input, for checking purposes

// stat reporting
uint64_t tt;
uint64_t ts0[100],ts1[100];

void report(void)
{
    const uint64_t* fp=df_tload();
    printf("++report\n");
    uint64_t n=fp[0];
    tt = df_tstamp(ts1) - tt;
    printf("df fib= %lu\n",n);
    df_destroy();
    df_exit();

    uint64_t t1 = df_tstamp(ts0);
    uint64_t nx = fastfib(nn);
    t1 = df_tstamp(ts1) - t1;

    printf("ref fib= %lu\n",nx);
    df_printstats(ts0,ts1,t1);

    printf(n==nx?"*** SUCCESS\n":"***FAILURE\n");
}

void df_main()
{
    tt = df_tstamp(ts0);
    uint64_t xrep = df_tschedule64(&report,1); // spawn reporter
	df_constrain(xrep,0x1); // constrain execution on node 1
    uint64_t xfib = df_tschedule64(&fib, 2);  // spawn fib
    uint64_t* tfib = df_tcache(xfib);
    tfib[0] = _TLOC(xrep,0); // edge: fib.out -> report[0]
    tfib[1] = nn;
    df_tdecreaseN(xfib,2);
    df_destroy();
}

int main(int argc, char **argv)
{
    nn = 10;
    if (argc > 1)
        nn = atoi(argv[1]);
    printf("computing fibonacci(%d)\n",nn);
    uint64_t xmain = df_tschedule64(&df_main,1);
	df_constrain(xmain,0x1); // constrain execution on node 1
    df_tdecrease(xmain);
    printf("++main\n");
    return 0;
}

