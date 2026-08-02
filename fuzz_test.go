package main

import (
	"testing"

	"github.com/knusbaum/go9p/fs"
)

// FuzzCtlFile fuzzes the /ctl 9P file write parser with random byte payloads
func FuzzCtlFile(f *testing.F) {
	engine := NewBitNetEngine()
	fsys, _ := fs.NewFS("bitnet-9p", "scott", 0755)
	ctlStat := fsys.NewStat("ctl", "scott", "scott", 0666)
	ctl := NewCtlFile(ctlStat, engine)

	// Seed corpus
	f.Add([]byte("sys You are a Plan 9 MUD narrator"))
	f.Add([]byte("temp 0.7"))
	f.Add([]byte("max 128"))
	f.Add([]byte(""))
	f.Add([]byte("invalid command"))
	f.Add([]byte("\x00\x01\x02\x03\x04\x05"))

	f.Fuzz(func(t *testing.T, data []byte) {
		// Assert ctl.Write never panics regardless of input
		n, err := ctl.Write(1, 0, data)
		if err != nil {
			t.Fatalf("Unexpected error from ctl.Write: %v", err)
		}
		if n != uint32(len(data)) {
			t.Fatalf("Wrote %d bytes, expected %d", n, len(data))
		}

		// Assert GetInfo never panics after arbitrary write
		_ = engine.GetInfo()
	})
}

// FuzzPromptFile fuzzes the /prompt 9P file write ingestion with random byte payloads
func FuzzPromptFile(f *testing.F) {
	engine := NewBitNetEngine()
	fsys, _ := fs.NewFS("bitnet-9p", "scott", 0755)
	promptStat := fsys.NewStat("prompt", "scott", "scott", 0666)
	prompt := NewPromptFile(promptStat, engine)

	// Seed corpus
	f.Add([]byte("What is Sector Baudway?"))
	f.Add([]byte("Describe the grid"))
	f.Add([]byte("\n\n\n"))
	f.Add([]byte("\x00\x00\x00"))
	f.Add([]byte("A" + string(make([]byte, 4096))))

	f.Fuzz(func(t *testing.T, data []byte) {
		// Assert prompt.Write never panics regardless of input
		n, err := prompt.Write(1, 0, data)
		if err != nil {
			t.Fatalf("Unexpected error from prompt.Write: %v", err)
		}
		if n != uint32(len(data)) {
			t.Fatalf("Wrote %d bytes, expected %d", n, len(data))
		}

		// Assert GetStreamBuffer never panics
		_ = engine.GetStreamBuffer()
	})
}
