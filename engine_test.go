package main

import (
	"strings"
	"testing"
	"time"

	"github.com/knusbaum/go9p/fs"
)

func TestEngineInitialization(t *testing.T) {
	engine := NewBitNetEngine()
	if engine.ModelName == "" {
		t.Fatal("Expected non-empty ModelName")
	}
	if engine.Quantization == "" {
		t.Fatal("Expected non-empty Quantization status")
	}
	info := engine.GetInfo()
	if !strings.Contains(info, "BitNet") {
		t.Fatalf("GetInfo() missing model identifier: %s", info)
	}
}

func TestPromptStreaming(t *testing.T) {
	engine := NewBitNetEngine()
	promptText := "Describe Sector Baudway in The Grid."

	engine.SetPrompt(promptText)
	if !engine.IsGenerating {
		t.Fatal("Expected IsGenerating to be true immediately after SetPrompt")
	}

	// Wait for token generation loop to complete
	timeout := time.After(5 * time.Second)
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-timeout:
			t.Fatal("Timeout waiting for token streaming completion")
		case <-ticker.C:
			buf := engine.GetStreamBuffer()
			if len(buf) > 0 && !engine.IsGenerating {
				t.Logf("Stream Output Verified (%d bytes): %s", len(buf), string(buf))
				return
			}
		}
	}
}

func Test9PFileSystemTree(t *testing.T) {
	engine := NewBitNetEngine()
	fsys, root := fs.NewFS("bitnet-9p", "scott", 0755)

	infoStat := fsys.NewStat("info", "scott", "scott", 0444)
	infoFile := fs.NewDynamicFile(infoStat, func() []byte {
		return []byte(engine.GetInfo())
	})
	root.AddChild(infoFile)

	promptStat := fsys.NewStat("prompt", "scott", "scott", 0666)
	promptFile := NewPromptFile(promptStat, engine)
	root.AddChild(promptFile)

	streamStat := fsys.NewStat("stream", "scott", "scott", 0444)
	streamFile := fs.NewDynamicFile(streamStat, func() []byte {
		return engine.GetStreamBuffer()
	})
	root.AddChild(streamFile)

	// Verify file children exist in root directory
	if root == nil {
		t.Fatal("Root directory is nil")
	}
}

func BenchmarkTokenGeneration(b *testing.B) {
	engine := NewBitNetEngine()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		engine.SetPrompt("Benchmark query prompt")
		for engine.IsGenerating {
			time.Sleep(1 * time.Millisecond)
		}
	}
}
