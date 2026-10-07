// wailcore.h — the C API of the embedded WAIL client core, a Go static library
// (wail-app/cmd/wailcore, built with -buildmode=c-archive and linked into the
// plugin). Declared here rather than including Go's generated header, whose
// C-only complex typedefs don't compile as C++.
//
// Threading: call from a non-realtime thread (Rack's UI thread) only — never
// from process(). A call into Go can block on the Go scheduler.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

char* WailCoreVersion(void); // static string; do not free
int WailCoreStart(void);     // 1 = started, 0 = already running
void WailCoreStop(void);     // stops and joins its goroutines; idempotent
uint64_t WailCoreTicks(void);

#ifdef __cplusplus
}
#endif
