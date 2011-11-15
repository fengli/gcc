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

;; TSCHEDULE RS1, RS2, RD
(define_insn "dta_tcreate"
  [(set (match_operand:DI 0 "register_operand" "=r")
	(unspec_volatile:DI
	  [(match_operand:SI 1 "register_operand" "r")
	   (match_operand:DI 2 "register_operand" "r")]
	  UNSPEC_TCREATE))
    (clobber (reg:CC FLAGS_REG))]
  "TARGET_64BIT && TARGET_DTA"
  {
     int rs1 = REGNO (operands[0]);
     int rs2 = REGNO (operands[1]);
     int rd = REGNO (operands[2]);
     operands[3] = GEN_INT(0000<<28|rs1<<24|rs2<<20|rd<<16);

     return ";;begin TSCHEDULE %0, %1, %2\n\tclc; \n\tcmovc\t %1, %3;\n\t;;end TSCHEDULE";
   }
)

;; TDESTROY
(define_insn "dta_tend"
  [(unspec_volatile [(const_int 0)] UNSPEC_TEND)]
  "TARGET_64BIT && TARGET_DTA"
  "TDESTROY"
  [(set_attr "length" "3")
   (set_attr "mode" "none")])

;; TREAD offset, RD
(define_insn "dta_tread"
  [(set (match_operand:DI 0 "register_operand" "=r")
	(unspec_volatile:DI [(match_operand:DI 1 "register_operand" "r")]
	  UNSPEC_TREAD))
   (clobber (reg:CC FLAGS_REG))]
  "TARGET_64BIT && TARGET_DTA"
  {
    int offset = REGNO (operands[0]);
    int rd = REGNO (operands[1]);
    operands[2] = GEN_INT (0010<<28|offset<<24|rd<<20);

    return ";;begin TREAD %0, %1\n\tclc\n\tcmovc\t%1,%2\n\t;;end TREAD";
  }
  [(set_attr "length" "3")
   (set_attr "mode" "DI")])

;; TSTORE RS, RD, offset
(define_insn "dta_tstore"
  [(unspec_volatile [(match_operand:SI 0 "register_operand" "r")
       (match_operand:DI 1 "register_operand" "r")
       (match_operand:SI 2 "register_operand" "r")]
       UNSPEC_TSTORE)
   (clobber (reg:CC FLAGS_REG))]
  "TARGET_64BIT && TARGET_DTA"
  {
    int rs = REGNO (operands[0]);
    int rd = REGNO (operands[1]);
    int offset = REGNO (operands[2]);
    operands[3] = GEN_INT (0011<<28|rs<<24|rd<<20|offset<<16);

    return ";;begin TSTORE %0, %1, %2\n\tclc\n\tcmovc\t%1, %3\n\t;;end TSTORE";
  }
  [(set_attr "length" "3")])

;; This will not be used. Keep here for compatibility reasons.
(define_insn "dta_tdecrease"
  [(unspec_volatile
     [(match_operand:DI 0 "register_operand" "r")]
      UNSPEC_TDECREASE)]
  "TARGET_64BIT && TARGET_DTA"
  "TDEC\t %0"
  [(set_attr "length" "3")])

;; (define_expand "dta_tread"
;;   [(set (match_operand:SI 0 "register_operand" "=r")
;; 	(unspec_volatile:SI [(match_operand:DI 1 "register_operand" "r")]
;; 	  UNSPEC_TREAD))]
;;   "TARGET_64BIT && TARGET_DTA"
;;   ""
;; )

;; (define_insn "*dta_tread"
;;   [(set (match_operand:SI 0 "register_operand" "=r")
;; 	(unspec_volatile:SI [(match_operand:DI 1 "register_operand" "r")]
;; 	  UNSPEC_TREAD))]
;;   "TARGET_64BIT && TARGET_DTA"
;;   "TREAD\t%1, %0"
;;   [(set_attr "length" "3")
;;    (set_attr "mode" "DI")])

;; (define_expand "dta_tstore"
;;   [(unspec_volatile [(match_operand:SI 0 "register_operand" "r")
;;        (match_operand:DI 1 "register_operand" "r")
;;        (match_operand:SI 2 "register_operand" "r")]
;;        UNSPEC_TSTORE)]
;;   "TARGET_64BIT && TARGET_DTA"
;;   ""
;; )

;; (define_insn "*dta_tstore"
;;   [(unspec_volatile [(match_operand:SI 0 "register_operand" "r")
;;        (match_operand:DI 1 "register_operand" "r")
;;        (match_operand:SI 2 "register_operand" "r")]
;;        UNSPEC_TSTORE)]
;;   "TARGET_64BIT && TARGET_DTA"
;;   "TSTORE\t%0, %1, %2"
;;   [(set_attr "length" "3")])


;; (define_insn_and_split "dta_tcreate"
;;   [(set (match_operand:DI 0 "register_operand" "=r")
;; 	(unspec_volatile:DI
;; 	  [(match_operand:DI 1 "register_operand" "r")
;; 	   (match_operand:DI 2 "register_operand" "r")]
;; 	  UNSPEC_TCREATE))]
;;   "TARGET_64BIT && TARGET_DTA"
;;   ""
;;   "reload_completed"
;;   [(clobber (reg:CC FLAGS_REG))
;;    (set (match_dup 0) (match_dup 4))
;;    (set (match_dup 1) (match_dup 5))
;;    (set (match_dup 2) (match_dup 6))
;;    ]
;;   "{
;;      int a = REGNO (operands[0]);
;;      int b = REGNO (operands[1]);
;;      int c = REGNO (operands[2]);
;;      operands[4] = GEN_INT (a);
;;      operands[5] = GEN_INT (b);
;;      operands[6] = GEN_INT (c);
;;    }"
;; )

;; (define_insn "*dta_tcreate"
;;   [(set (match_operand:DI 0 "register_operand" "=r")
;; 	(unspec_volatile:DI
;; 	  [(match_operand:DI 1 "register_operand" "r")
;; 	   (match_operand:SI 2 "register_operand" "r")]
;; 	  UNSPEC_TCREATE))]
;;   "TARGET_64BIT && TARGET_DTA"
;;   "TCREATE\t%1, %2, %0")
