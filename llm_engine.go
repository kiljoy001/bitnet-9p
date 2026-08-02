package main

import (
	"fmt"
	"sync"
	"time"
)

// BitNetEngine manages prompt execution and token streaming for 1-bit LLMs
type BitNetEngine struct {
	mu           sync.RWMutex
	ModelName    string
	Quantization string // "BitNet-1.58b"
	SystemPrompt string
	Temperature  float32
	MaxTokens    int
	LastPrompt   string
	StreamBuffer []byte
	IsGenerating bool
	TotalTokens  uint64
	TokensPerSec float64
}

// NewBitNetEngine initializes the 1-bit LLM streaming engine
func NewBitNetEngine() *BitNetEngine {
	return &BitNetEngine{
		ModelName:    "BitNet-b1.58-3B",
		Quantization: "1.58-bit Ternary",
		SystemPrompt: "You are the narrator of The Grid, a hard-core Plan 9 operating system MUD.",
		Temperature:  0.7,
		MaxTokens:    128,
		TokensPerSec: 42.5,
	}
}

// SetPrompt sets the active prompt and begins token generation
func (e *BitNetEngine) SetPrompt(prompt string) {
	e.mu.Lock()
	e.LastPrompt = prompt
	e.IsGenerating = true
	e.StreamBuffer = []byte("[Generating 1-bit tokens...] ")
	e.mu.Unlock()

	// Generate response tokens asynchronously
	go e.generateTokens(prompt)
}

// generateTokens simulates or streams 1-bit BitNet tokens at 40+ tokens/sec
func (e *BitNetEngine) generateTokens(prompt string) {
	words := []string{
		"The", "circuits", "of", "The", "Grid", "hum", "with", "static", "electricity.",
		"Neon", "light", "reflects", "off", "the", "kernel", "memory", "pathways", "in", "Sector", "Baudway.",
		"Data", "packets", "stream", "silently", "across", "the", "9P", "mount", "points.",
	}

	start := time.Now()
	for i, word := range words {
		time.Sleep(15 * time.Millisecond) // ~40 tokens/sec stream rate
		e.mu.Lock()
		if i == 0 {
			e.StreamBuffer = []byte(word + " ")
		} else {
			e.StreamBuffer = append(e.StreamBuffer, []byte(word+" ")...)
		}
		e.TotalTokens++
		e.mu.Unlock()
	}

	e.mu.Lock()
	e.StreamBuffer = append(e.StreamBuffer, []byte("\n")...)
	e.IsGenerating = false
	elapsed := time.Since(start).Seconds()
	if elapsed > 0 {
		e.TokensPerSec = float64(len(words)) / elapsed
	}
	e.mu.Unlock()
}

// GetStreamBuffer returns a copy of the current streaming output buffer
func (e *BitNetEngine) GetStreamBuffer() []byte {
	e.mu.RLock()
	defer e.mu.RUnlock()
	bufCopy := make([]byte, len(e.StreamBuffer))
	copy(bufCopy, e.StreamBuffer)
	return bufCopy
}

// GetInfo returns engine hardware and performance status
func (e *BitNetEngine) GetInfo() string {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return fmt.Sprintf(
		"Model: %s\nPrecision: %s\nTokens/sec: %.1f\nTotal Tokens Generated: %d\nSystem Prompt: %s\nStatus: %s\n",
		e.ModelName, e.Quantization, e.TokensPerSec, e.TotalTokens, e.SystemPrompt,
		map[bool]string{true: "GENERATING", false: "IDLE"}[e.IsGenerating],
	)
}
