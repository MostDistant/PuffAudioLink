package main

import (
	"testing"
	"time"
)

// The core's lifecycle is driven from a host thread that is not a Go thread
// (Rack's UI thread). Start must be idempotent, the goroutine must actually
// run, and Stop must leave it restartable.
func TestCoreLifecycle(t *testing.T) {
	c := &core{}
	if !c.start() {
		t.Fatal("first start should report it started")
	}
	if c.start() {
		t.Fatal("second start should be a no-op")
	}
	deadline := time.Now().Add(2 * time.Second)
	for c.ticks.Load() < 3 {
		if time.Now().After(deadline) {
			t.Fatalf("goroutine not ticking: %d ticks", c.ticks.Load())
		}
		time.Sleep(5 * time.Millisecond)
	}
	c.stop()
	stopped := c.ticks.Load()
	time.Sleep(50 * time.Millisecond)
	if got := c.ticks.Load(); got != stopped {
		t.Fatalf("ticks advanced after stop: %d → %d", stopped, got)
	}
	c.stop() // idempotent
	if !c.start() {
		t.Fatal("start after stop should start again")
	}
	c.stop()
}
