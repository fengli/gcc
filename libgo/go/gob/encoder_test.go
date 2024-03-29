// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package gob

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"reflect"
	"strings"
	"testing"
)

type ET2 struct {
	X string
}

type ET1 struct {
	A    int
	Et2  *ET2
	Next *ET1
}

// Like ET1 but with a different name for a field
type ET3 struct {
	A             int
	Et2           *ET2
	DifferentNext *ET1
}

// Like ET1 but with a different type for a field
type ET4 struct {
	A    int
	Et2  float64
	Next int
}

func TestEncoderDecoder(t *testing.T) {
	b := new(bytes.Buffer)
	enc := NewEncoder(b)
	et1 := new(ET1)
	et1.A = 7
	et1.Et2 = new(ET2)
	err := enc.Encode(et1)
	if err != nil {
		t.Error("encoder fail:", err)
	}
	dec := NewDecoder(b)
	newEt1 := new(ET1)
	err = dec.Decode(newEt1)
	if err != nil {
		t.Fatal("error decoding ET1:", err)
	}

	if !reflect.DeepEqual(et1, newEt1) {
		t.Fatalf("invalid data for et1: expected %+v; got %+v", *et1, *newEt1)
	}
	if b.Len() != 0 {
		t.Error("not at eof;", b.Len(), "bytes left")
	}

	enc.Encode(et1)
	newEt1 = new(ET1)
	err = dec.Decode(newEt1)
	if err != nil {
		t.Fatal("round 2: error decoding ET1:", err)
	}
	if !reflect.DeepEqual(et1, newEt1) {
		t.Fatalf("round 2: invalid data for et1: expected %+v; got %+v", *et1, *newEt1)
	}
	if b.Len() != 0 {
		t.Error("round 2: not at eof;", b.Len(), "bytes left")
	}

	// Now test with a running encoder/decoder pair that we recognize a type mismatch.
	err = enc.Encode(et1)
	if err != nil {
		t.Error("round 3: encoder fail:", err)
	}
	newEt2 := new(ET2)
	err = dec.Decode(newEt2)
	if err == nil {
		t.Fatal("round 3: expected `bad type' error decoding ET2")
	}
}

// Run one value through the encoder/decoder, but use the wrong type.
// Input is always an ET1; we compare it to whatever is under 'e'.
func badTypeCheck(e interface{}, shouldFail bool, msg string, t *testing.T) {
	b := new(bytes.Buffer)
	enc := NewEncoder(b)
	et1 := new(ET1)
	et1.A = 7
	et1.Et2 = new(ET2)
	err := enc.Encode(et1)
	if err != nil {
		t.Error("encoder fail:", err)
	}
	dec := NewDecoder(b)
	err = dec.Decode(e)
	if shouldFail && err == nil {
		t.Error("expected error for", msg)
	}
	if !shouldFail && err != nil {
		t.Error("unexpected error for", msg, err)
	}
}

// Test that we recognize a bad type the first time.
func TestWrongTypeDecoder(t *testing.T) {
	badTypeCheck(new(ET2), true, "no fields in common", t)
	badTypeCheck(new(ET3), false, "different name of field", t)
	badTypeCheck(new(ET4), true, "different type of field", t)
}

func corruptDataCheck(s string, err os.Error, t *testing.T) {
	b := bytes.NewBufferString(s)
	dec := NewDecoder(b)
	err1 := dec.Decode(new(ET2))
	if err1 != err {
		t.Errorf("from %q expected error %s; got %s", s, err, err1)
	}
}

// Check that we survive bad data.
func TestBadData(t *testing.T) {
	corruptDataCheck("", os.EOF, t)
	corruptDataCheck("\x7Fhi", io.ErrUnexpectedEOF, t)
	corruptDataCheck("\x03now is the time for all good men", errBadType, t)
}

// Types not supported by the Encoder.
var unsupportedValues = []interface{}{
	make(chan int),
	func(a int) bool { return true },
}

func TestUnsupported(t *testing.T) {
	var b bytes.Buffer
	enc := NewEncoder(&b)
	for _, v := range unsupportedValues {
		err := enc.Encode(v)
		if err == nil {
			t.Errorf("expected error for %T; got none", v)
		}
	}
}

func encAndDec(in, out interface{}) os.Error {
	b := new(bytes.Buffer)
	enc := NewEncoder(b)
	err := enc.Encode(in)
	if err != nil {
		return err
	}
	dec := NewDecoder(b)
	err = dec.Decode(out)
	if err != nil {
		return err
	}
	return nil
}

func TestTypeToPtrType(t *testing.T) {
	// Encode a T, decode a *T
	type Type0 struct {
		A int
	}
	t0 := Type0{7}
	t0p := (*Type0)(nil)
	if err := encAndDec(t0, t0p); err != nil {
		t.Error(err)
	}
}

func TestPtrTypeToType(t *testing.T) {
	// Encode a *T, decode a T
	type Type1 struct {
		A uint
	}
	t1p := &Type1{17}
	var t1 Type1
	if err := encAndDec(t1, t1p); err != nil {
		t.Error(err)
	}
}

func TestTypeToPtrPtrPtrPtrType(t *testing.T) {
	type Type2 struct {
		A ****float64
	}
	t2 := Type2{}
	t2.A = new(***float64)
	*t2.A = new(**float64)
	**t2.A = new(*float64)
	***t2.A = new(float64)
	****t2.A = 27.4
	t2pppp := new(***Type2)
	if err := encAndDec(t2, t2pppp); err != nil {
		t.Fatal(err)
	}
	if ****(****t2pppp).A != ****t2.A {
		t.Errorf("wrong value after decode: %g not %g", ****(****t2pppp).A, ****t2.A)
	}
}

func TestSlice(t *testing.T) {
	type Type3 struct {
		A []string
	}
	t3p := &Type3{[]string{"hello", "world"}}
	var t3 Type3
	if err := encAndDec(t3, t3p); err != nil {
		t.Error(err)
	}
}

func TestValueError(t *testing.T) {
	// Encode a *T, decode a T
	type Type4 struct {
		A int
	}
	t4p := &Type4{3}
	var t4 Type4 // note: not a pointer.
	if err := encAndDec(t4p, t4); err == nil || strings.Index(err.String(), "pointer") < 0 {
		t.Error("expected error about pointer; got", err)
	}
}

func TestArray(t *testing.T) {
	type Type5 struct {
		A [3]string
		B [3]byte
	}
	type Type6 struct {
		A [2]string // can't hold t5.a
	}
	t5 := Type5{[3]string{"hello", ",", "world"}, [3]byte{1, 2, 3}}
	var t5p Type5
	if err := encAndDec(t5, &t5p); err != nil {
		t.Error(err)
	}
	var t6 Type6
	if err := encAndDec(t5, &t6); err == nil {
		t.Error("should fail with mismatched array sizes")
	}
}

func TestRecursiveMapType(t *testing.T) {
	type recursiveMap map[string]recursiveMap
	r1 := recursiveMap{"A": recursiveMap{"B": nil, "C": nil}, "D": nil}
	r2 := make(recursiveMap)
	if err := encAndDec(r1, &r2); err != nil {
		t.Error(err)
	}
}

func TestRecursiveSliceType(t *testing.T) {
	type recursiveSlice []recursiveSlice
	r1 := recursiveSlice{0: recursiveSlice{0: nil}, 1: nil}
	r2 := make(recursiveSlice, 0)
	if err := encAndDec(r1, &r2); err != nil {
		t.Error(err)
	}
}

// Regression test for bug: must send zero values inside arrays
func TestDefaultsInArray(t *testing.T) {
	type Type7 struct {
		B []bool
		I []int
		S []string
		F []float64
	}
	t7 := Type7{
		[]bool{false, false, true},
		[]int{0, 0, 1},
		[]string{"hi", "", "there"},
		[]float64{0, 0, 1},
	}
	var t7p Type7
	if err := encAndDec(t7, &t7p); err != nil {
		t.Error(err)
	}
}

var testInt int
var testFloat32 float32
var testString string
var testSlice []string
var testMap map[string]int
var testArray [7]int

type SingleTest struct {
	in  interface{}
	out interface{}
	err string
}

var singleTests = []SingleTest{
	{17, &testInt, ""},
	{float32(17.5), &testFloat32, ""},
	{"bike shed", &testString, ""},
	{[]string{"bike", "shed", "paint", "color"}, &testSlice, ""},
	{map[string]int{"seven": 7, "twelve": 12}, &testMap, ""},
	{[7]int{4, 55, 0, 0, 0, 0, 0}, &testArray, ""}, // case that once triggered a bug
	{[7]int{4, 55, 1, 44, 22, 66, 1234}, &testArray, ""},

	// Decode errors
	{172, &testFloat32, "wrong type"},
}

func TestSingletons(t *testing.T) {
	b := new(bytes.Buffer)
	enc := NewEncoder(b)
	dec := NewDecoder(b)
	for _, test := range singleTests {
		b.Reset()
		err := enc.Encode(test.in)
		if err != nil {
			t.Errorf("error encoding %v: %s", test.in, err)
			continue
		}
		err = dec.Decode(test.out)
		switch {
		case err != nil && test.err == "":
			t.Errorf("error decoding %v: %s", test.in, err)
			continue
		case err == nil && test.err != "":
			t.Errorf("expected error decoding %v: %s", test.in, test.err)
			continue
		case err != nil && test.err != "":
			if strings.Index(err.String(), test.err) < 0 {
				t.Errorf("wrong error decoding %v: wanted %s, got %v", test.in, test.err, err)
			}
			continue
		}
		// Get rid of the pointer in the rhs
		val := reflect.NewValue(test.out).(*reflect.PtrValue).Elem().Interface()
		if !reflect.DeepEqual(test.in, val) {
			t.Errorf("decoding singleton: expected %v got %v", test.in, val)
		}
	}
}

func TestStructNonStruct(t *testing.T) {
	type Struct struct {
		A string
	}
	type NonStruct string
	s := Struct{"hello"}
	var sp Struct
	if err := encAndDec(s, &sp); err != nil {
		t.Error(err)
	}
	var ns NonStruct
	if err := encAndDec(s, &ns); err == nil {
		t.Error("should get error for struct/non-struct")
	} else if strings.Index(err.String(), "type") < 0 {
		t.Error("for struct/non-struct expected type error; got", err)
	}
	// Now try the other way
	var nsp NonStruct
	if err := encAndDec(ns, &nsp); err != nil {
		t.Error(err)
	}
	if err := encAndDec(ns, &s); err == nil {
		t.Error("should get error for non-struct/struct")
	} else if strings.Index(err.String(), "type") < 0 {
		t.Error("for non-struct/struct expected type error; got", err)
	}
}

type interfaceIndirectTestI interface {
	F() bool
}

type interfaceIndirectTestT struct{}

func (this *interfaceIndirectTestT) F() bool {
	return true
}

// A version of a bug reported on golang-nuts.  Also tests top-level
// slice of interfaces.  The issue was registering *T caused T to be
// stored as the concrete type.
func TestInterfaceIndirect(t *testing.T) {
	Register(&interfaceIndirectTestT{})
	b := new(bytes.Buffer)
	w := []interfaceIndirectTestI{&interfaceIndirectTestT{}}
	err := NewEncoder(b).Encode(w)
	if err != nil {
		t.Fatal("encode error:", err)
	}

	var r []interfaceIndirectTestI
	err = NewDecoder(b).Decode(&r)
	if err != nil {
		t.Fatal("decode error:", err)
	}
}

// Now follow various tests that decode into things that can't represent the
// encoded value, all of which should be legal.

// Also, when the ignored object contains an interface value, it may define
// types. Make sure that skipping the value still defines the types by using
// the encoder/decoder pair to send a value afterwards.  If an interface
// is sent, its type in the test is always NewType0, so this checks that the
// encoder and decoder don't skew with respect to type definitions.

type Struct0 struct {
	I interface{}
}

type NewType0 struct {
	S string
}

type ignoreTest struct {
	in, out interface{}
}

var ignoreTests = []ignoreTest{
	// Decode normal struct into an empty struct
	{&struct{ A int }{23}, &struct{}{}},
	// Decode normal struct into a nil.
	{&struct{ A int }{23}, nil},
	// Decode singleton string into a nil.
	{"hello, world", nil},
	// Decode singleton slice into a nil.
	{[]int{1, 2, 3, 4}, nil},
	// Decode struct containing an interface into a nil.
	{&Struct0{&NewType0{"value0"}}, nil},
	// Decode singleton slice of interfaces into a nil.
	{[]interface{}{"hi", &NewType0{"value1"}, 23}, nil},
}

func TestDecodeIntoNothing(t *testing.T) {
	Register(new(NewType0))
	for i, test := range ignoreTests {
		b := new(bytes.Buffer)
		enc := NewEncoder(b)
		err := enc.Encode(test.in)
		if err != nil {
			t.Errorf("%d: encode error %s:", i, err)
			continue
		}
		dec := NewDecoder(b)
		err = dec.Decode(test.out)
		if err != nil {
			t.Errorf("%d: decode error: %s", i, err)
			continue
		}
		// Now see if the encoder and decoder are in a consistent state.
		str := fmt.Sprintf("Value %d", i)
		err = enc.Encode(&NewType0{str})
		if err != nil {
			t.Fatalf("%d: NewType0 encode error: %s", i, err)
		}
		ns := new(NewType0)
		err = dec.Decode(ns)
		if err != nil {
			t.Fatalf("%d: NewType0 decode error: %s", i, err)
		}
		if ns.S != str {
			t.Fatalf("%d: expected %q got %q", i, str, ns.S)
		}
	}
}

// Another bug from golang-nuts, involving nested interfaces.
type Bug0Outer struct {
	Bug0Field interface{}
}

type Bug0Inner struct {
	A int
}

func TestNestedInterfaces(t *testing.T) {
	var buf bytes.Buffer
	e := NewEncoder(&buf)
	d := NewDecoder(&buf)
	Register(new(Bug0Outer))
	Register(new(Bug0Inner))
	f := &Bug0Outer{&Bug0Outer{&Bug0Inner{7}}}
	var v interface{} = f
	err := e.Encode(&v)
	if err != nil {
		t.Fatal("Encode:", err)
	}
	err = d.Decode(&v)
	if err != nil {
		t.Fatal("Decode:", err)
	}
	// Make sure it decoded correctly.
	outer1, ok := v.(*Bug0Outer)
	if !ok {
		t.Fatalf("v not Bug0Outer: %T", v)
	}
	outer2, ok := outer1.Bug0Field.(*Bug0Outer)
	if !ok {
		t.Fatalf("v.Bug0Field not Bug0Outer: %T", outer1.Bug0Field)
	}
	inner, ok := outer2.Bug0Field.(*Bug0Inner)
	if !ok {
		t.Fatalf("v.Bug0Field.Bug0Field not Bug0Inner: %T", outer2.Bug0Field)
	}
	if inner.A != 7 {
		t.Fatalf("final value %d; expected %d", inner.A, 7)
	}
}
