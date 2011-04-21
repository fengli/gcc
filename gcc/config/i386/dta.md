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
  [(set (match_operand:SI 0 "register_operand" "=a")
	(unspec_volatile:SI
	  [(match_operand:SI 1 "register_operand" "a")
	   (match_operand:SI 2 "register_operand" "c")]
	  UNSPEC_TCREATE))]
  "TARGET_DTA"
{
  rtx edx = gen_rtx_REG (SImode, DX_REG);
  emit_insn (gen_rtx_SET (SImode, edx, const0_rtx));
})

(define_insn "*dta_tcreate"
  [(set (match_operand:SI 0 "register_operand" "=a")
	(unspec_volatile:SI
	  [(match_operand:SI 1 "register_operand" "a")
	   (match_operand:SI 2 "register_operand" "c")]
	  UNSPEC_TCREATE))]
  "TARGET_DTA"
  "dtacreate")

(define_insn "dta_tdecrease"
  [(unspec_volatile
     [(match_operand:SI 0 "register_operand" "c")]
      UNSPEC_TDECREASE)]
  "TARGET_DTA"
  "dtadecrease"
  [(set_attr "length" "3")])

(define_expand "dta_tread"
  [(set (match_operand:SI 0 "register_operand" "=a")
	(unspec_volatile:SI [(match_operand:SI 1 "register_operand" "c")]
	  UNSPEC_TREAD))]
  "TARGET_DTA"
  ""
)

(define_insn "*dta_tread"
  [(set (match_operand:SI 0 "register_operand" "=a")
	(unspec_volatile:SI [(match_operand:SI 1 "register_operand" "c")]
	  UNSPEC_TREAD))]
  "TARGET_DTA"
  "dtaread"
  [(set_attr "length" "3")
   (set_attr "mode" "SI")])

(define_insn "dta_tstore"
  [(unspec_volatile [(match_operand:SI 0 "register_operand" "a")
       (match_operand:SI 1 "register_operand" "c")
       (match_operand:SI 2 "register_operand" "b")]
       UNSPEC_TSTORE)]
  "TARGET_DTA"
  "dtastore"
  [(set_attr "length" "3")])

(define_insn "dta_tend"
  [(unspec_volatile [(const_int 0)] UNSPEC_TEND)]
  ""
  "dtaend"
  [(set_attr "length" "3")
   (set_attr "mode" "none")])
