;; Machine description for DDM (todo).
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

(define_insn "altivec_vforku<VI_char>"
  [(set (match_operand:VI 0 "register_operand" "=v")
        (unspec:VI [(match_operand:VI 1 "register_operand" "v")
                    (match_operand:VI 2 "register_operand" "v")]
		   UNSPEC_VFORKU))]
  "TARGET_ALTIVEC"
  "vforku<VI_char> %0,%1,%2"
  [(set_attr "type" "vecsimple")])

(define_insn "altivec_vforks<VI_char>"
  [(set (match_operand:VI 0 "register_operand" "=v")
        (unspec:VI [(match_operand:VI 1 "register_operand" "v")
                    (match_operand:VI 2 "register_operand" "v")]
		   UNSPEC_VFORKS))]
  "TARGET_ALTIVEC"
  "vforks<VI_char> %0,%1,%2"
  [(set_attr "type" "vecsimple")])
