// wailcore is the WAIL client core built as a C static library
// (go build -buildmode=c-archive) for hosts that embed it — first the VCV
// Rack plugin (rack/), which links it into its plugin.dylib.
//
// Packaging spike: this proves the Go runtime starts, runs goroutines and
// stops cleanly inside a host process, called from a host thread. The session
// engine is not here yet; it moves in once it is extracted from package main.
//
// Threading contract for hosts: call these from a non-realtime thread (Rack's
// UI thread). Never from an audio callback — a cgo call into Go can block on
// the Go scheduler (ADR-0002); audio crosses the boundary through C rings.
package main

/*
#include <stdint.h>
*/
import "C"

import (
	"sync"
	"sync/atomic"
	"time"
)

// version is set at build time: -ldflags "-X main.version=…".
var version = "dev"

var versionC = C.CString("wailcore " + version) // never freed: lives for the process

// core is the running state behind the exported calls.
type core struct {
	mu    sync.Mutex
	done  chan struct{}
	wg    sync.WaitGroup
	ticks atomic.Uint64
}

func (c *core) start() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.done != nil {
		return false
	}
	c.done = make(chan struct{})
	c.wg.Add(1)
	go func(done <-chan struct{}) {
		defer c.wg.Done()
		t := time.NewTicker(10 * time.Millisecond)
		defer t.Stop()
		for {
			select {
			case <-done:
				return
			case <-t.C:
				c.ticks.Add(1)
			}
		}
	}(c.done)
	return true
}

func (c *core) stop() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.done == nil {
		return
	}
	close(c.done)
	c.wg.Wait()
	c.done = nil
}

var theCore core

// WailCoreVersion returns a static, NUL-terminated version string.
//
//export WailCoreVersion
func WailCoreVersion() *C.char { return versionC }

// WailCoreStart starts the core; 1 if it started, 0 if it was already running.
//
//export WailCoreStart
func WailCoreStart() C.int {
	if theCore.start() {
		return 1
	}
	return 0
}

// WailCoreStop stops the core and waits for its goroutines. Idempotent.
//
//export WailCoreStop
func WailCoreStop() { theCore.stop() }

// WailCoreTicks counts the core goroutine's 10 ms ticks — liveness for the spike.
//
//export WailCoreTicks
func WailCoreTicks() C.uint64_t { return C.uint64_t(theCore.ticks.Load()) }

func main() {} // required by -buildmode=c-archive; never runs
