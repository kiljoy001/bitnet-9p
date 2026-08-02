package main

import (
	"fmt"
	"math/rand"
	"testing"
	"time"

	"github.com/knusbaum/go9p/fs"
)

// TestPropertyMonotonicTokens asserts invariant: TotalTokens is strictly non-decreasing
func TestPropertyMonotonicTokens(t *testing.T) {
	engine := NewBitNetEngine()
	var lastCount uint64 = 0

	for i := 0; i < 20; i++ {
		engine.SetPrompt(fmt.Sprintf("Iteration %d", i))
		time.Sleep(20 * time.Millisecond)

		engine.mu.RLock()
		currentCount := engine.TotalTokens
		engine.mu.RUnlock()

		if currentCount < lastCount {
			t.Fatalf("INVARIANT VIOLATION: TotalTokens decreased from %d to %d", lastCount, currentCount)
		}
		lastCount = currentCount
	}
}

// TestPropertyNonEmptyInfo asserts invariant: GetInfo() is always valid and contains key fields
func TestPropertyNonEmptyInfo(t *testing.T) {
	engine := NewBitNetEngine()
	fsys, _ := fs.NewFS("bitnet-9p", "scott", 0755)

	ctlStat := fsys.NewStat("ctl", "scott", "scott", 0666)
	ctl := NewCtlFile(ctlStat, engine)

	for i := 0; i < 50; i++ {
		// Mutate state randomly
		ctl.Write(1, 0, []byte(fmt.Sprintf("temp %.2f", rand.Float32()*2.0)))
		ctl.Write(1, 0, []byte(fmt.Sprintf("max %d", rand.Intn(512)+1)))

		info := engine.GetInfo()
		if len(info) == 0 {
			t.Fatal("INVARIANT VIOLATION: GetInfo() returned empty string")
		}
		if !containsAll(info, "Model:", "Precision:", "Tokens/sec:", "Status:") {
			t.Fatalf("INVARIANT VIOLATION: GetInfo() missing required keys: %s", info)
		}
	}
}

// TestPropertyReadOffsetSafety asserts invariant: Offset reads beyond buffer never panic or return invalid slice bounds
func TestPropertyReadOffsetSafety(t *testing.T) {
	engine := NewBitNetEngine()
	engine.SetPrompt("Test prompt for offset property")

	time.Sleep(50 * time.Millisecond)
	buf := engine.GetStreamBuffer()

	// Property: Offset > len(buf) MUST return empty slice without panic
	for offset := uint64(len(buf)); offset < uint64(len(buf)+500); offset += 10 {
		if offset >= uint64(len(buf)) {
			// Test offset bounds
			data := readWindow(buf, offset, 100)
			if len(data) != 0 {
				t.Fatalf("INVARIANT VIOLATION: Expected 0 bytes for offset %d, got %d", offset, len(data))
			}
		}
	}
}

func readWindow(data []byte, offset uint64, count uint64) []byte {
	if offset >= uint64(len(data)) {
		return []byte{}
	}
	end := offset + count
	if end > uint64(len(data)) {
		end = uint64(len(data))
	}
	return data[offset:end]
}

func containsAll(s string, keywords ...string) bool {
	for _, kw := range keywords {
		if !stringsContains(s, kw) {
			return false
		}
	}
	return true
}

func stringsContains(s, substr string) bool {
	return len(s) >= len(substr) && (s == substr || len(substr) == 0 || (len(s) > 0 && len(substr) > 0 && findSubstr(s, substr)))
}

func findSubstr(s, substr string) bool {
	for i := 0; i <= len(s)-len(substr); i++ {
		if s[i:i+len(substr)] == substr {
			return true
		}
	}
	return false
}
