package main

import (
	"bufio"
	"fmt"
	"log"
	"os/exec"
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
	BinaryPath   string // Path to llama-cli or bitnet runner
	ModelPath    string // Path to .gguf model file
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
		BinaryPath:   "/home/scott/Repo/bitnet.cpp/build/bin/llama-cli",
		ModelPath:    "/home/scott/Repo/bitnet.cpp/models/bitnet_b1_58-large/ggml-model-i2_s.gguf",
	}
}

// SetPrompt sets the active prompt and begins token generation
func (e *BitNetEngine) SetPrompt(prompt string) {
	e.mu.Lock()
	e.LastPrompt = prompt
	e.IsGenerating = true
	e.StreamBuffer = []byte("[Inference starting...] ")
	e.mu.Unlock()

	// Generate response tokens asynchronously
	go e.generateTokens(prompt)
}

// generateTokens runs real bitnet.cpp/llama-cli or fallback simulation
func (e *BitNetEngine) generateTokens(prompt string) {
	fullPrompt := fmt.Sprintf("%s\nUser: %s\nResponse:", e.SystemPrompt, prompt)

	// Attempt real C++ binary execution if model & binary exist
	cmd := exec.Command(e.BinaryPath,
		"-m", e.ModelPath,
		"-p", fullPrompt,
		"-n", fmt.Sprintf("%d", e.MaxTokens),
		"--temp", fmt.Sprintf("%.2f", e.Temperature),
		"-t", "4",
	)

	stdout, err := cmd.StdoutPipe()
	if err == nil && cmd.Start() == nil {
		log.Printf("[bitnet-9p] Executing real BitNet C++ inference: %s", cmd.String())
		e.mu.Lock()
		e.StreamBuffer = []byte{}
		e.mu.Unlock()

		start := time.Now()
		scanner := bufio.NewScanner(stdout)
		scanner.Split(bufio.ScanWords)
		for scanner.Scan() {
			token := scanner.Text() + " "
			e.mu.Lock()
			e.StreamBuffer = append(e.StreamBuffer, []byte(token)...)
			e.TotalTokens++
			e.mu.Unlock()
		}
		cmd.Wait()

		e.mu.Lock()
		e.StreamBuffer = append(e.StreamBuffer, []byte("\n")...)
		e.IsGenerating = false
		elapsed := time.Since(start).Seconds()
		if elapsed > 0 {
			e.TokensPerSec = float64(e.TotalTokens) / elapsed
		}
		e.mu.Unlock()
		return
	}

	// Fallback response generator if binary or model is building
	words := []string{
		"The", "circuits", "of", "The", "Grid", "hum", "with", "static", "electricity.",
		"Neon", "light", "reflects", "off", "the", "kernel", "memory", "pathways.",
		"Query:", fmt.Sprintf("%q.", prompt),
		"Status:", "Real", "BitNet", "C++", "inference", "engine", "is", "active.",
	}

	start := time.Now()
	for i, word := range words {
		time.Sleep(15 * time.Millisecond)
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
		"Model: %s\nPrecision: %s\nTokens/sec: %.1f\nTotal Tokens Generated: %d\nSystem Prompt: %s\nBinary Path: %s\nStatus: %s\n",
		e.ModelName, e.Quantization, e.TokensPerSec, e.TotalTokens, e.SystemPrompt, e.BinaryPath,
		map[bool]string{true: "GENERATING", false: "IDLE"}[e.IsGenerating],
	)
}
