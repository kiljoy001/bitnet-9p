package main

import (
	"bufio"
	"flag"
	"log"
	"net"

	"github.com/knusbaum/go9p"
	"github.com/knusbaum/go9p/fs"
)

var (
	listenAddr = flag.String("l", "0.0.0.0:5647", "Listen address for 9P 1-bit LLM streaming service")
)

func main() {
	flag.Parse()

	engine := NewBitNetEngine()
	log.Printf("[bitnet-9p] Initialized 1-Bit BitNet LLM Streaming 9P Server (%s, %s)", engine.ModelName, engine.Quantization)

	// Build 9P virtual filesystem tree
	fsys, root := fs.NewFS("bitnet-9p", "scott", 0755)

	// Create /info file
	infoStat := fsys.NewStat("info", "scott", "scott", 0444)
	infoFile := fs.NewDynamicFile(infoStat, func() []byte {
		return []byte(engine.GetInfo())
	})
	root.AddChild(infoFile)

	// Create /stream synthetic file
	streamStat := fsys.NewStat("stream", "scott", "scott", 0444)
	streamFile := fs.NewDynamicFile(streamStat, func() []byte {
		buf := engine.GetStreamBuffer()
		if len(buf) > 0 {
			return buf
		}
		return []byte("IDLE\n")
	})
	root.AddChild(streamFile)

	// Create /ctl file
	ctlStat := fsys.NewStat("ctl", "scott", "scott", 0666)
	ctlFile := NewCtlFile(ctlStat, engine)
	root.AddChild(ctlFile)

	// Create /prompt synthetic file
	promptStat := fsys.NewStat("prompt", "scott", "scott", 0666)
	promptFile := NewPromptFile(promptStat, engine)
	root.AddChild(promptFile)

	listener, err := net.Listen("tcp", *listenAddr)
	if err != nil {
		log.Fatalf("Failed to listen on %s: %v", *listenAddr, err)
	}
	defer listener.Close()

	log.Printf("[bitnet-9p] 9P Server listening on %s (Port 5647)", *listenAddr)
	srv := fsys.Server()
	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("Accept error: %v", err)
			continue
		}
		go func(nc net.Conn) {
			read := bufio.NewReader(nc)
			if err := go9p.ServeReadWriter(read, nc, srv); err != nil {
				// Connection exit
			}
		}(conn)
	}
}
