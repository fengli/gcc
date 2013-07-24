// (C) Copyright 2009-2013 Hewlett-Packard Development Company, L.P.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
#ifndef _TSU_H_INCLUDED
#define _TSU_H_INCLUDED

#define _XS(s) #s

#define _TSU_BASE 0x2DAF0000
#define _TSUOP(x) (_TSU_BASE+(x))
#define _TSUOPI(x,i) (_TSU_BASE+((i)<<8)+(x))
#define _ASMOP(x) ((x)-_TSU_BASE + 5000)

#define _TSU_TINIT          _TSUOP(0x01)
#define _TSU_TPOLL          _TSUOP(0x02)
#define _TSU_TRESET         _TSUOP(0x03)
#define _TSU_TSCHEDULE      _TSUOP(0x04)
#define _TSU_TDESTROY       _TSUOP(0x05)
#define _TSU_CONSTRAIN      _TSUOP(0x06)
// _TSUOP(0x07)
// _TSUOP(0x08)
#define _TSU_TLOAD          _TSUOP(0x09)
#define _TSU_TCACHE         _TSUOP(0x0a)
#define _TSU_TDECREASE      _TSUOP(0x0b)
#define _TSU_TDECREASEN     _TSUOP(0x0c)
#define _TSU_TSUBSCRIBE     _TSUOP(0x0d)
#define _TSU_TPUBLISH       _TSUOP(0x0e)

#define _TSU_TSTAMP         _TSUOP(0x10)

#define _COMBINE(_hi,_lo) (((unsigned long long)(_hi)<<32) + (_lo))
#define _LO32(_x) ((unsigned long long)(_x)&0xffffffffULL)
#define _HI32(_x) ((unsigned long long)(_x)>>32)
#define _TLOC(_tp,_l) ((_tp)+(unsigned long long)(_l))
#define _TLOC_OFF(_x) _LO32(_x)
#define _TLOC_TID(_x) _HI32(_x)
#define _SCSZ(_sc,_sz) _COMBINE(_sc,_sz)

#define _ASMVOL  __asm__ volatile 
#define _ASMNV   __asm__ 
#define _INL  __always_inline 

#define _REG_ARG1  "R"     /* argument 1 */
#define _REG_ARG1d "0"     /* argument 1 = dest*/
#define _REG_ARG2  "R"     /* argument 2 */
#define _REG_RES   "=R"    /* result */

#define _REG_CLOB  /*example: #define _REG_CLOB :"%rax" // don't forget the colon! */

#define _ASM_SEQ1(_of)  _XS(prefetchnta _of(%q0,%q0,1))
#define _ASM_SEQ2(_of)  _XS(prefetchnta _of(%q0,%q1,1))
#define _ASM_SEQ3(_of)  _XS(prefetchnta _of(%q0,%q2,1))

#define TSU_TINIT(_nop,_sp) \
    _ASMVOL (_ASM_SEQ2(_TSU_TINIT)::_REG_ARG1(_nop),_REG_ARG2(_sp) _REG_CLOB)
#define TSU_POLL(_ip) \
    _ASMVOL (_ASM_SEQ1(_TSU_TPOLL):_REG_RES(_ip): _REG_CLOB)
#define TSU_RESET(_ptr,_s) \
    _ASMVOL (_ASM_SEQ2(_TSU_TRESET)::_REG_ARG1(_ptr),_REG_ARG2(_s) _REG_CLOB)
#define TSU_TSCHEDULE(_ip,_sc,_sz,_tid) \
    _ASMVOL (_ASM_SEQ3(_TSU_TSCHEDULE):_REG_RES(_tid):_REG_ARG1d(_ip),_REG_ARG2(_SCSZ(_sc,_sz)) _REG_CLOB)
#define TSU_DESTROY(_tid) \
    _ASMVOL (_ASM_SEQ1(_TSU_TDESTROY):_REG_RES(_tid): _REG_CLOB)
#define TSU_CONSTRAIN(_tloc,_xc) \
    _ASMVOL (_ASM_SEQ2(_TSU_CONSTRAIN)::_REG_ARG1(_tloc),_REG_ARG2(_xc) _REG_CLOB)
#define TSU_TLOAD(_fp) \
    _ASMVOL (_ASM_SEQ1(_TSU_TLOAD):_REG_RES(_fp): _REG_CLOB)
#define TSU_TCACHE(_tfp,_tloc) \
    _ASMNV  (_ASM_SEQ1(_TSU_TCACHE):_REG_RES(_tfp):_REG_ARG1d(_tloc) _REG_CLOB)
#define TSU_TDECREASE(_tloc)\
    _ASMNV  (_ASM_SEQ1(_TSU_TDECREASE)::_REG_ARG1(_tloc) _REG_CLOB)
#define TSU_TDECREASEN(_tloc, _n)	\
    _ASMNV  (_ASM_SEQ2(_TSU_TDECREASEN)::_REG_ARG1(_tloc),_REG_ARG2(_n) _REG_CLOB)
#define TSU_TSUBSCRIBE(_tloc,_rp) \
	_ASMNV  (_ASM_SEQ2(_TSU_TSUBSCRIBE)::_REG_ARG1(_tloc),_REG_ARG2(_rp) _REG_CLOB)
#define TSU_TPUBLISH(_rp) \
	_ASMNV  (_ASM_SEQ1(_TSU_TRELEASE)::_REG_ARG1(_rp) _REG_CLOB)
#define TSU_TSTAMP(_ts,_buf) \
    _ASMVOL (_ASM_SEQ1(_TSU_TSTAMP):_REG_RES(_ts):_REG_ARG1d(_buf) _REG_CLOB)

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef void (*df_thread_t)(void);
void df_exit(void);
void df_printstats(const uint64_t *, const uint64_t *, uint64_t);
void* df_shmem(void);

// _INL static uint64_t df_self() { uint64_t s; _ASMVOL ("mov %%fs:0x10,%0":"=r"(s)::); return s; }
// _INL static uint64_t df_self() { return (uint64_t)pthread_self(); }

_INL static uint64_t df_tschedule64(df_thread_t ip, uint32_t sc)
{
    uint64_t tid;
    tid = __builtin_tstar_schedule ((void *)ip, sc, sc*sizeof (uint64_t));    
    //TSU_TSCHEDULE((uint64_t)ip,sc,sc*sizeof(uint64_t),tid); 
    return tid; 
}

_INL static uint64_t df_tschedulez(df_thread_t ip, uint32_t sc, uint32_t sz)
{
    uint64_t tid;
    tid = __builtin_tstar_schedule (ip, sc, sz);    
    //TSU_TSCHEDULE((uint64_t)ip,sc,sz,tid); 
    return tid; 
}

_INL static void df_tdecrease(uint64_t tid)
{
    __builtin_tstar_decreasen (tid, 1);
}

_INL static void df_tdecreaseN(uint64_t tid, int n)
{
    __builtin_tstar_decreasen (tid, n);
}

_INL static void df_destroy()
{
    df_thread_t dft;
    dft = (df_thread_t) __builtin_tstar_destroy ();
    (*dft)();
    // _ASMVOL ("jmp *%0\n"::"R"(dft));
}

_INL static void df_constrain(uint64_t tid, uint64_t xc)
{
    TSU_CONSTRAIN(tid,xc);
}

_INL static void df_reset(void* ptr, const char* s)
{
	TSU_RESET(ptr,s);
}

_INL static uint64_t df_tstamp(uint64_t *buf)
{
    uint64_t ts;
    TSU_TSTAMP(ts,buf);
    return ts;
}

_INL static void* df_tload() 
{
    uint64_t p;
    p = __builtin_tstar_load ();
    return (void*)p; 
}

_INL static void* df_tcache(uint64_t tid) 
{
    uint64_t tfp;
    tfp = __builtin_tstar_cache (tid);
    return (void*)tfp;
}

_INL static void df_subscribe(uint64_t tid, int32_t off, void* regptr)
{
	TSU_TSUBSCRIBE(_TLOC(tid,off),regptr);
}

_INL static void df_publish(void *regptr)
{
	TSU_TPUBLISH(regptr);
}

#ifdef __cplusplus
}
#endif
#endif
