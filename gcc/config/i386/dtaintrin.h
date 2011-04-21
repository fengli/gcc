/* Copyright (C) 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011
   Free Software Foundation, Inc.
   Contributed by Feng Li (feng.li@inria.fr)

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef _DTAINTRIN_H_INCLUDED
#define _DTAINTRIN_H_INCLUDED

#ifndef __DTA__
# error "DTA instruction set not enabled"
#else

extern void *
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__TCreate (void * fp, unsigned int SC)
{
  __builtin_ia32_tcreate (fp, SC);
}

extern void
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__TDecrease (void * fp)
{
  __builtin_ia32_tdecrease (fp);
}

extern unsigned int
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__TRead (unsigned int offset)
{
  __builtin_ia32_tread (offset);
}

extern void
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__TStore (unsigned int value, void * fp, unsigned int offset)
{
  __builtin_ia32_tstore (value, fp, offset);
}

extern __inline void __attribute__((__gnu_inline__, __always_inline__, __artificial__))
__TEnd (void)
{
  __builtin_ia32_tend ();
}

/* 32bit DTA FFORK.  */
extern __inline unsigned int
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__fforkb (unsigned int __C, unsigned char __V)
{
  return __builtin_ia32_forkqi (__C, __V);
}

extern __inline unsigned int
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__fforkw (unsigned int __C, unsigned short __V)
{
  return __builtin_ia32_forkhi (__C, __V);
}

extern __inline unsigned int
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__fforkd (unsigned int __C, unsigned int __V)
{
  return __builtin_ia32_forksi (__C, __V);
}

/* 64bit accumulate FORK (polynomial 0x11EDC6F41) value.  */
extern __inline unsigned long long
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__fforkq (unsigned long long __C, unsigned long long __V)
{
  return __builtin_ia32_forkdi (__C, __V);
}

#endif /* __DTA__ */
#endif /* _DTAINTRIN_H_INCLUDED */
