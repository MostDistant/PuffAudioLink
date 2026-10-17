package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// fakeRelay accepts one client, answers its join with the given existing
// peers, and forwards every sync payload the client broadcasts.
func fakeRelay(t *testing.T, existingPeers []string) (url string, syncs <-chan SyncMessage) {
	t.Helper()
	out := make(chan SyncMessage, 256)
	upgrader := websocket.Upgrader{}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/ws" {
			http.NotFound(w, r)
			return
		}
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		var join map[string]any
		if err := conn.ReadJSON(&join); err != nil || join["type"] != "join" {
			return
		}
		if err := conn.WriteJSON(map[string]any{"type": "join_ok", "peers": existingPeers}); err != nil {
			return
		}
		for {
			_, data, err := conn.ReadMessage()
			if err != nil {
				return
			}
			var env struct {
				Type    string          `json:"type"`
				Payload json.RawMessage `json:"payload"`
			}
			if json.Unmarshal(data, &env) != nil || env.Type != "sync" {
				continue
			}
			var msg SyncMessage
			if json.Unmarshal(env.Payload, &msg) == nil {
				select {
				case out <- msg:
				default:
				}
			}
		}
	}))
	t.Cleanup(srv.Close)
	return "ws" + strings.TrimPrefix(srv.URL, "http"), out
}

// A peer founding an empty room asserts its own tempo and interval config, so
// the relay records the founder's BPM. Without the declaration the relay falls
// back to 120 BPM and the first joiner pulls the founder onto it.
func TestFounderDeclaresTempoAndIntervalConfig(t *testing.T) {
	url, syncs := fakeRelay(t, nil)
	handle, err := SpawnSession(&NoopEmitter{}, SessionConfig{
		Server:        url,
		Room:          "founder-test",
		DisplayName:   "Founder",
		LinkAudioName: "WAIL-founder-test",
		Identity:      "founder-identity",
		BPM:           95,
		Bars:          4,
		Quantum:       4,
		StreamCount:   1,
	})
	if err != nil {
		t.Fatalf("SpawnSession: %v", err)
	}
	t.Cleanup(func() {
		handle.cancel()
		<-handle.done
	})

	var gotTempo, gotConfig bool
	deadline := time.After(5 * time.Second)
	for !gotTempo || !gotConfig {
		select {
		case msg := <-syncs:
			switch msg.Type {
			case "TempoDeclare":
				if msg.BPM != 95 {
					t.Fatalf("founder declared %.1f BPM, want 95", msg.BPM)
				}
				if msg.Owner != "founder-identity" {
					t.Fatalf("founder declared tempo as owner %q, want its identity", msg.Owner)
				}
				gotTempo = true
			case "IntervalConfig":
				if msg.Bars != 4 || msg.Quantum != 4 {
					t.Fatalf("founder declared %dx%.0f interval config, want 4x4", msg.Bars, msg.Quantum)
				}
				gotConfig = true
			}
		case <-deadline:
			t.Fatalf("founder of an empty room never declared its tempo (TempoDeclare seen: %v) or interval config (IntervalConfig seen: %v)", gotTempo, gotConfig)
		}
	}
}
