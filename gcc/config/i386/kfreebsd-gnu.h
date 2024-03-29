/* Definitions for Intel 386 running kFreeBSD-based GNU systems with ELF format
   Copyright (C) 2004, 2007, 2011
   Free Software Foundation, Inc.
   Contributed by Robert Millan.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#undef GNU_USER_LINK_EMULATION
#define GNU_USER_LINK_EMULATION "elf_i386_fbsd"

#undef GNU_USER_DYNAMIC_LINKER32
#define GNU_USER_DYNAMIC_LINKER32 "/lib/ld.so.1"

#undef GNU_USER_DYNAMIC_LINKER64
#define GNU_USER_DYNAMIC_LINKER64 "/lib/ld-kfreebsd-x86-64.so.1"

#undef MD_UNWIND_SUPPORT
