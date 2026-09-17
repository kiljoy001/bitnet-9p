# bitnet9p - 9P narrator server for a MUD, backed by a 1-bit BitNet LLM.
#
# Builds against plan9port's lib9p and bitnet.cpp's llama libraries. Both are
# expected to be built already; override the paths below if they live
# elsewhere:
#
#	make PLAN9=/path/to/plan9port BITNET=/path/to/bitnet.cpp

PLAN9 ?= $(HOME)/Repo/plan9port
BITNET ?= $(HOME)/Repo/bitnet.cpp

LLAMA_INC = $(BITNET)/3rdparty/llama.cpp/include
GGML_INC = $(BITNET)/3rdparty/llama.cpp/ggml/include
LLAMA_LIB = $(BITNET)/build/bin

# plan9port headers redefine much of libc, so the 9P and llama translation
# units are compiled separately and only linked together.
P9_CFLAGS = -I$(PLAN9)/include -D_POSIX_SOURCE -D_SUSV2_SOURCE -D_C99_SNPRINTF_EXTENSION
LLAMA_CFLAGS = -I$(LLAMA_INC) -I$(GGML_INC)

CFLAGS = -O2 -Wall -Wno-parentheses -g
LDFLAGS = -L$(PLAN9)/lib -L$(LLAMA_LIB)
LIBS = -l9p -lthread -l9 -lllama -lggml -lggml-base -lggml-cpu -lstdc++ -lpthread -lm

OBJS = bitnet9p.o narrator.o

all: bitnet9p narrate

bitnet9p: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBS) \
		-Wl,-rpath,$(LLAMA_LIB)

# Test/demo client. One connection, so the session persists across writes.
narrate: narrate.o
	$(CC) $(CFLAGS) -o $@ narrate.o -L$(PLAN9)/lib \
		-l9pclient -lmux -lthread -l9 -lpthread -lm

narrate.o: narrate.c
	$(CC) $(CFLAGS) $(P9_CFLAGS) -c -o $@ narrate.c

# 9P layer: plan9port headers only.
bitnet9p.o: bitnet9p.c narrator.h
	$(CC) $(CFLAGS) $(P9_CFLAGS) -c -o $@ bitnet9p.c

# Inference layer: llama.cpp headers only.
narrator.o: narrator.c narrator.h
	$(CC) $(CFLAGS) $(LLAMA_CFLAGS) -c -o $@ narrator.c

clean:
	rm -f $(OBJS) narrate.o bitnet9p narrate

.PHONY: all clean
