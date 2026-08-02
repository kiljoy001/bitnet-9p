package main

import (
	"log"
	"strconv"
	"strings"
	"sync"

	"github.com/knusbaum/go9p/fs"
	"github.com/knusbaum/go9p/proto"
)

// CtlFile handles writes to /ctl
type CtlFile struct {
	*fs.BaseFile
	engine *BitNetEngine
	mu     sync.Mutex
}

var _ fs.File = (*CtlFile)(nil)

func NewCtlFile(stat *proto.Stat, engine *BitNetEngine) *CtlFile {
	return &CtlFile{
		BaseFile: fs.NewBaseFile(stat),
		engine:   engine,
	}
}

func (c *CtlFile) Read(fid uint64, offset uint64, count uint64) ([]byte, error) {
	data := []byte(c.engine.GetInfo())
	if offset >= uint64(len(data)) {
		return []byte{}, nil
	}
	end := offset + count
	if end > uint64(len(data)) {
		end = uint64(len(data))
	}
	return data[offset:end], nil
}

func (c *CtlFile) Write(fid uint64, offset uint64, data []byte) (uint32, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	cmd := strings.TrimSpace(string(data))
	parts := strings.Fields(cmd)
	if len(parts) > 0 {
		switch parts[0] {
		case "sys":
			if len(parts) > 1 {
				c.engine.SystemPrompt = strings.Join(parts[1:], " ")
				log.Printf("[bitnet-9p] Updated System Prompt: %s", c.engine.SystemPrompt)
			}
		case "temp":
			if len(parts) > 1 {
				if t, err := strconv.ParseFloat(parts[1], 32); err == nil {
					c.engine.Temperature = float32(t)
					log.Printf("[bitnet-9p] Updated Temp: %.2f", c.engine.Temperature)
				}
			}
		case "max":
			if len(parts) > 1 {
				if m, err := strconv.Atoi(parts[1]); err == nil {
					c.engine.MaxTokens = m
					log.Printf("[bitnet-9p] Updated Max Tokens: %d", c.engine.MaxTokens)
				}
			}
		}
	}
	return uint32(len(data)), nil
}

// PromptFile handles writes to /prompt
type PromptFile struct {
	*fs.BaseFile
	engine *BitNetEngine
	mu     sync.Mutex
}

var _ fs.File = (*PromptFile)(nil)

func NewPromptFile(stat *proto.Stat, engine *BitNetEngine) *PromptFile {
	return &PromptFile{
		BaseFile: fs.NewBaseFile(stat),
		engine:   engine,
	}
}

func (p *PromptFile) Read(fid uint64, offset uint64, count uint64) ([]byte, error) {
	data := []byte("Write prompt text here to trigger 1-bit LLM token generation stream.\n")
	if offset >= uint64(len(data)) {
		return []byte{}, nil
	}
	end := offset + count
	if end > uint64(len(data)) {
		end = uint64(len(data))
	}
	return data[offset:end], nil
}

func (p *PromptFile) Write(fid uint64, offset uint64, data []byte) (uint32, error) {
	p.mu.Lock()
	defer p.mu.Unlock()

	promptText := strings.TrimSpace(string(data))
	if len(promptText) > 0 {
		log.Printf("[bitnet-9p] Received Prompt: %q. Starting 1-bit token streaming...", promptText)
		p.engine.SetPrompt(promptText)
	}
	return uint32(len(data)), nil
}
