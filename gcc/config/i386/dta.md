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

;; General version for void tdecreasen (void *fp, size_t n);
(define_insn "*tstar_decreasen"
  [(unspec_volatile:DI
          [(match_operand:DI 0 "register_operand" "R")
          (match_operand:DI 1 "register_operand" "R")]
          UNSPEC_TDECREASEN)]
  "TARGET_64BIT && TARGET_DTA"
  {
     int tsu_decreasen = 0x0c;
     int magic_num = 0x2DAF;
     unsigned int tdecreasen_inst = magic_num<<16|tsu_decreasen;
     operands[2] = GEN_INT (tdecreasen_inst);

     return "#begin TDECREASEN %0 (fp), %1 (n)\n\tprefetchnta\t%c2(%0,%1,1)\n\t#end TDECREASEN\n";
   }
)

;; Immediate version for void tdecreasen (void *fp, size_t n);
(define_insn "tstar_decreasen"
  [(unspec_volatile:DI
          [(match_operand:DI 0 "register_operand" "R")
          (match_operand:DI 1 "immediate_operand" "")]
          UNSPEC_TDECREASEN)]
  "TARGET_64BIT && TARGET_DTA && INTVAL(operands[1])<256"
  {
     int tsu_decreasen = 0x0c;
     int magic_num = 0x2DAF;
     int n = INTVAL(operands[1]);
     unsigned int tdecreasen_inst = magic_num<<16|n<<8|tsu_decreasen;
     operands[2] = GEN_INT (tdecreasen_inst);

     return "#begin TDECREASENi %0 (fp), %1 (n)\n\tprefetchnta\t%c2(%0,%0,1)\n\t#end TDECREASENi\n";
   }
)

(define_insn "tstar_destroy"
  [(set (match_operand:DI 0 "register_operand" "=R")
        (unspec_volatile [(const_int 0)] UNSPEC_TDESTROY))]
  "TARGET_DTA && TARGET_64BIT"
   {
     int tsu_destroy = 0x05;
     int magic_num = 0x2DAF;
     unsigned int tdestroy_inst = magic_num<<16|tsu_destroy;
     operands[1] = GEN_INT (tdestroy_inst);

     return "#begin %0 (fp) < TDESTROY\n\tprefetchnta\t%c1(%0,%0,1)\n\t#destroy TDESTROY\n";
   }
   [(set_attr "length" "3")
    (set_attr "mode" "none")])

(define_insn "tstar_cache"
  [(set (match_operand:DI 0 "register_operand" "=R")
        (unspec_volatile:DI
          [(match_operand:DI 1 "register_operand" "0")]
          UNSPEC_TCACHE))]
  "TARGET_64BIT && TARGET_DTA"
  {
     int tsu_cache = 0x0a;
     int magic_num = 0x2DAF;
     unsigned int tcache_inst = magic_num<<16|tsu_cache;
     operands[2] = GEN_INT (tcache_inst);

     return "#begin %0 (tfp) < TCACHE %0 (tid)\n\tprefetchnta\t%c2(%0,%0,1)\n\t#end TCACHE\n";
   }
)

(define_insn "tstar_load"
  [(set (match_operand:DI 0 "register_operand" "=R")
        (unspec_volatile [(const_int 0)] UNSPEC_TLOAD))]
  "TARGET_DTA && TARGET_64BIT"
   {
     int tsu_load = 0x09;
     int magic_num = 0x2DAF;
     unsigned int tload_inst = magic_num<<16|tsu_load;
     operands[1] = GEN_INT (tload_inst);

     return "#begin %0 (fp) < TLOAD\n\tprefetchnta\t%c1(%0,%0,1)\n\t#end TLOAD\n";
   }
   [(set_attr "length" "3")
    (set_attr "mode" "none")])

;; General version for void *tschedule
(define_insn "tstar_schedule"
  [(set (match_operand:DI 0 "register_operand" "=R")
        (unspec_volatile:DI
          [(match_operand:DI 1 "register_operand" "R")
          (match_operand:DI 2 "register_operand" "0")]
          UNSPEC_TSCHEDULE))]
  "TARGET_64BIT && TARGET_DTA"
  {
     int tsu_schedule = 0x04;
     int magic_num = 0x2DAF;
     unsigned int tschedule_inst = magic_num<<16|tsu_schedule;
     operands[3] = GEN_INT (tschedule_inst);

     return "#begin %0 (fp) < TSCHEDULE %0 (workfn), %1 (sc<<32+size)\n\tprefetchnta\t%c3(%0,%1,1)\n\t#end TSCHEDULE\n";
   }
)
