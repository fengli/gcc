;; Machine description for DTA.
;; Copyright (C) 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011
;; Free Software Foundation, Inc.
;; Contributed by Feng Li (feng.li@inria.fr)

;; This file is part of GCC.

;; GCC is free software; you can redistribute it and/or modify it
;; under the terms of the GNU General Public License as published
;; by the Free Software Foundation; either version 3, or (at your
;; option) any later version.

;; GCC is distributed in the hope that it will be useful, but WITHOUT
;; ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
;; or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
;; License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GCC; see the file COPYING3.  If not see
;; <http://www.gnu.org/licenses/>.

(define_expand "dta_tcreate"
  [(set (match_operand:DI 0 "register_operand" "=r")
	(unspec_volatile:DI
	  [(match_operand:DI 1 "register_operand" "r")
	   (match_operand:SI 2 "register_operand" "r")]
	  UNSPEC_TCREATE))]
  "TARGET_64BIT && TARGET_DTA"
{
  rtx edx = gen_rtx_REG (DImode, DX_REG);
  emit_insn (gen_rtx_SET (DImode, edx, const0_rtx));
})

(define_insn "*dta_tcreate"
  [(set (match_operand:DI 0 "register_operand" "=r")
	(unspec_volatile:DI
	  [(match_operand:DI 1 "register_operand" "r")
	   (match_operand:SI 2 "register_operand" "r")]
	  UNSPEC_TCREATE))]
  "TARGET_64BIT && TARGET_DTA"
  "TCREATE\t%1, %2, %0")

(define_insn "dta_tdecrease"
  [(unspec_volatile
     [(match_operand:DI 0 "register_operand" "r")]
      UNSPEC_TDECREASE)]
  "TARGET_64BIT && TARGET_DTA"
  "TDEC\t %0"
  [(set_attr "length" "3")])

(define_expand "dta_tread"
  [(set (match_operand:SI 0 "register_operand" "=r")
	(unspec_volatile:SI [(match_operand:DI 1 "register_operand" "r")]
	  UNSPEC_TREAD))]
  "TARGET_64BIT && TARGET_DTA"
  ""
)

(define_insn "*dta_tread"
  [(set (match_operand:SI 0 "register_operand" "=r")
	(unspec_volatile:SI [(match_operand:DI 1 "register_operand" "r")]
	  UNSPEC_TREAD))]
  "TARGET_64BIT && TARGET_DTA"
  "TREAD\t%1, %0"
  [(set_attr "length" "3")
   (set_attr "mode" "DI")])

(define_expand "dta_tstore"
  [(unspec_volatile [(match_operand:SI 0 "register_operand" "r")
       (match_operand:DI 1 "register_operand" "r")
       (match_operand:SI 2 "register_operand" "r")]
       UNSPEC_TSTORE)]
  "TARGET_64BIT && TARGET_DTA"
  ""
)

(define_insn "*dta_tstore"
  [(unspec_volatile [(match_operand:SI 0 "register_operand" "r")
       (match_operand:DI 1 "register_operand" "r")
       (match_operand:SI 2 "register_operand" "r")]
       UNSPEC_TSTORE)]
  "TARGET_64BIT && TARGET_DTA"
  "TSTORE\t%0, %1, %2"
  [(set_attr "length" "3")])

(define_insn "dta_tend"
  [(unspec_volatile [(const_int 0)] UNSPEC_TEND)]
  "TARGET_64BIT && TARGET_DTA"
  "TEND"
  [(set_attr "length" "3")
   (set_attr "mode" "none")])
