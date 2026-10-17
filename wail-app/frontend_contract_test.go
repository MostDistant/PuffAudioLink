package main

import (
	"os"
	"reflect"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// The frontend calls the backend by Tauri-style command names, and
// wails-adapter.js maps each one onto an App method. A command missing from
// the map throws "Unknown command", which call sites swallow with
// .catch(() => {}) — so a gap is silent at runtime (set_remember_enabled went
// unmapped this way and the "Remember settings" toggle never reached the app).
// This test pins the contract: every invoked command is mapped, every mapped
// method exists on App, and every positional arg list matches its arity.
func TestFrontendInvokesMapToAppMethods(t *testing.T) {
	mainJS := readFrontend(t, "main.js")
	adapter := readFrontend(t, "wails-adapter.js")

	commandMap := map[string]string{}
	for _, m := range regexp.MustCompile(`'([a-z_]+)':\s*'main\.App\.([A-Za-z]+)'`).FindAllStringSubmatch(adapter, -1) {
		commandMap[m[1]] = m[2]
	}
	if len(commandMap) == 0 {
		t.Fatal("parsed no commandMap entries from wails-adapter.js; has its format changed?")
	}

	argOrder := map[string]int{}
	for _, m := range regexp.MustCompile(`'([a-z_]+)':\s*\[([^\]]*)\]`).FindAllStringSubmatch(adapter, -1) {
		argOrder[m[1]] = len(regexp.MustCompile(`'[A-Za-z]+'`).FindAllString(m[2], -1))
	}
	if len(argOrder) == 0 {
		t.Fatal("parsed no argOrder entries from wails-adapter.js; has its format changed?")
	}

	invoked := map[string]bool{}
	for _, m := range regexp.MustCompile(`invoke\(\s*['"]([a-z_]+)['"]`).FindAllStringSubmatch(mainJS, -1) {
		invoked[m[1]] = true
	}
	if len(invoked) == 0 {
		t.Fatal("parsed no invoke() calls from main.js")
	}

	var unmapped []string
	for cmd := range invoked {
		if _, ok := commandMap[cmd]; !ok {
			unmapped = append(unmapped, cmd)
		}
	}
	sort.Strings(unmapped)
	if len(unmapped) > 0 {
		t.Errorf("frontend invokes commands wails-adapter.js does not map: %s", strings.Join(unmapped, ", "))
	}

	appType := reflect.TypeOf((*App)(nil))
	for cmd, method := range commandMap {
		m, ok := appType.MethodByName(method)
		if !ok {
			t.Errorf("wails-adapter.js maps %s to App.%s, which does not exist", cmd, method)
			continue
		}
		if n, ok := argOrder[cmd]; ok && n != m.Type.NumIn()-1 {
			t.Errorf("wails-adapter.js passes %d positional args to App.%s, which takes %d", n, method, m.Type.NumIn()-1)
		}
	}
}

func readFrontend(t *testing.T, name string) string {
	t.Helper()
	b, err := os.ReadFile("frontend/" + name)
	if err != nil {
		t.Fatalf("read frontend/%s: %v", name, err)
	}
	return string(b)
}
