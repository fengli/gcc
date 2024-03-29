// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// The testing package provides support for automated testing of Go packages.
// It is intended to be used in concert with the ``gotest'' utility, which automates
// execution of any function of the form
//     func TestXxx(*testing.T)
// where Xxx can be any alphanumeric string (but the first letter must not be in
// [a-z]) and serves to identify the test routine.
// These TestXxx routines should be declared within the package they are testing.
//
// Functions of the form
//     func BenchmarkXxx(*testing.B)
// are considered benchmarks, and are executed by gotest when the -test.bench
// flag is provided.
//
// A sample benchmark function looks like this:
//     func BenchmarkHello(b *testing.B) {
//         for i := 0; i < b.N; i++ {
//             fmt.Sprintf("hello")
//         }
//     }
// The benchmark package will vary b.N until the benchmark function lasts
// long enough to be timed reliably.  The output
//     testing.BenchmarkHello	500000	      4076 ns/op
// means that the loop ran 500000 times at a speed of 4076 ns per loop.
//
// If a benchmark needs some expensive setup before running, the timer
// may be stopped:
//     func BenchmarkBigLen(b *testing.B) {
//         b.StopTimer()
//         big := NewBig()
//         b.StartTimer()
//         for i := 0; i < b.N; i++ {
//             big.Len()
//         }
//     }
package testing

import (
	"flag"
	"fmt"
	"os"
	"runtime"
	"runtime/pprof"
	"time"
)

var (
	// The short flag requests that tests run more quickly, but its functionality
	// is provided by test writers themselves.  The testing package is just its
	// home.  The all.bash installation script sets it to make installation more
	// efficient, but by default the flag is off so a plain "gotest" will do a
	// full test of the package.
	short = flag.Bool("test.short", false, "run smaller test suite to save time")

	// Report as tests are run; default is silent for success.
	chatty         = flag.Bool("test.v", false, "verbose: print additional output")
	match          = flag.String("test.run", "", "regular expression to select tests to run")
	memProfile     = flag.String("test.memprofile", "", "write a memory profile to the named file after execution")
	memProfileRate = flag.Int("test.memprofilerate", 0, "if >=0, sets runtime.MemProfileRate")
	cpuProfile     = flag.String("test.cpuprofile", "", "write a cpu profile to the named file during execution")
	timeout        = flag.Int64("test.timeout", 0, "if > 0, sets time limit for tests in seconds")
)

// Short reports whether the -test.short flag is set.
func Short() bool {
	return *short
}


// Insert final newline if needed and tabs after internal newlines.
func tabify(s string) string {
	n := len(s)
	if n > 0 && s[n-1] != '\n' {
		s += "\n"
		n++
	}
	for i := 0; i < n-1; i++ { // -1 to avoid final newline
		if s[i] == '\n' {
			return s[0:i+1] + "\t" + tabify(s[i+1:n])
		}
	}
	return s
}

// T is a type passed to Test functions to manage test state and support formatted test logs.
// Logs are accumulated during execution and dumped to standard error when done.
type T struct {
	errors string
	failed bool
	ch     chan *T
}

// Fail marks the Test function as having failed but continues execution.
func (t *T) Fail() { t.failed = true }

// Failed returns whether the Test function has failed.
func (t *T) Failed() bool { return t.failed }

// FailNow marks the Test function as having failed and stops its execution.
// Execution will continue at the next Test.
func (t *T) FailNow() {
	t.Fail()
	t.ch <- t
	runtime.Goexit()
}

// Log formats its arguments using default formatting, analogous to Print(),
// and records the text in the error log.
func (t *T) Log(args ...interface{}) { t.errors += "\t" + tabify(fmt.Sprintln(args...)) }

// Logf formats its arguments according to the format, analogous to Printf(),
// and records the text in the error log.
func (t *T) Logf(format string, args ...interface{}) {
	t.errors += "\t" + tabify(fmt.Sprintf(format, args...))
}

// Error is equivalent to Log() followed by Fail().
func (t *T) Error(args ...interface{}) {
	t.Log(args...)
	t.Fail()
}

// Errorf is equivalent to Logf() followed by Fail().
func (t *T) Errorf(format string, args ...interface{}) {
	t.Logf(format, args...)
	t.Fail()
}

// Fatal is equivalent to Log() followed by FailNow().
func (t *T) Fatal(args ...interface{}) {
	t.Log(args...)
	t.FailNow()
}

// Fatalf is equivalent to Logf() followed by FailNow().
func (t *T) Fatalf(format string, args ...interface{}) {
	t.Logf(format, args...)
	t.FailNow()
}

// An internal type but exported because it is cross-package; part of the implementation
// of gotest.
type InternalTest struct {
	Name string
	F    func(*T)
}

func tRunner(t *T, test *InternalTest) {
	test.F(t)
	t.ch <- t
}

// An internal function but exported because it is cross-package; part of the implementation
// of gotest.
func Main(matchString func(pat, str string) (bool, os.Error), tests []InternalTest, benchmarks []InternalBenchmark) {
	flag.Parse()

	before()
	startAlarm()
	RunTests(matchString, tests)
	stopAlarm()
	RunBenchmarks(matchString, benchmarks)
	after()
}

func RunTests(matchString func(pat, str string) (bool, os.Error), tests []InternalTest) {
	ok := true
	if len(tests) == 0 {
		println("testing: warning: no tests to run")
	}
	for i := 0; i < len(tests); i++ {
		matched, err := matchString(*match, tests[i].Name)
		if err != nil {
			println("invalid regexp for -test.run:", err.String())
			os.Exit(1)
		}
		if !matched {
			continue
		}
		if *chatty {
			println("=== RUN ", tests[i].Name)
		}
		ns := -time.Nanoseconds()
		t := new(T)
		t.ch = make(chan *T)
		go tRunner(t, &tests[i])
		<-t.ch
		ns += time.Nanoseconds()
		tstr := fmt.Sprintf("(%.2f seconds)", float64(ns)/1e9)
		if t.failed {
			println("--- FAIL:", tests[i].Name, tstr)
			print(t.errors)
			ok = false
		} else if *chatty {
			println("--- PASS:", tests[i].Name, tstr)
			print(t.errors)
		}
	}
	if !ok {
		println("FAIL")
		os.Exit(1)
	}
	println("PASS")
}

// before runs before all testing.
func before() {
	if *memProfileRate > 0 {
		runtime.MemProfileRate = *memProfileRate
	}
	if *cpuProfile != "" {
		f, err := os.Create(*cpuProfile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "testing: %s", err)
			return
		}
		if err := pprof.StartCPUProfile(f); err != nil {
			fmt.Fprintf(os.Stderr, "testing: can't start cpu profile: %s", err)
			f.Close()
			return
		}
		// Could save f so after can call f.Close; not worth the effort.
	}

}

// after runs after all testing.
func after() {
	if *cpuProfile != "" {
		pprof.StopCPUProfile() // flushes profile to disk
	}
	if *memProfile != "" {
		f, err := os.Create(*memProfile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "testing: %s", err)
			return
		}
		if err = pprof.WriteHeapProfile(f); err != nil {
			fmt.Fprintf(os.Stderr, "testing: can't write %s: %s", *memProfile, err)
		}
		f.Close()
	}
}

var timer *time.Timer

// startAlarm starts an alarm if requested.
func startAlarm() {
	if *timeout > 0 {
		timer = time.AfterFunc(*timeout*1e9, alarm)
	}
}

// stopAlarm turns off the alarm.
func stopAlarm() {
	if *timeout > 0 {
		timer.Stop()
	}
}

// alarm is called if the timeout expires.
func alarm() {
	panic("test timed out")
}
