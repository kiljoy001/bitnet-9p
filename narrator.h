/*
 * narrator.h - 1-bit BitNet LLM narration engine for the MUD 9P server.
 *
 * One Narrator per 9P session: each holds its own llama_context and sampler
 * chain, so concurrent players never share generation state. The model itself
 * is loaded once and shared read-only across all sessions.
 */

#ifndef NARRATOR_H
#define NARRATOR_H

#include <stddef.h>

typedef struct Narrator Narrator;

/*
 * Load the shared model. Call once at startup, before any narrator_new().
 * Returns 0 on success, negative on failure.
 */
int narrator_init_model(const char *model_path, int n_threads);

/* Release the shared model. Call once at shutdown. */
void narrator_free_model(void);

/*
 * Create a per-session narrator with its own context and KV cache.
 * n_ctx is the context window in tokens. Returns NULL on failure.
 */
Narrator *narrator_new(int n_ctx, int n_threads);

/* Destroy a session narrator. Safe on NULL. */
void narrator_free(Narrator *n);

/* Sampling controls. Applied to the next generation. */
void narrator_set_temp(Narrator *n, float temp);
void narrator_set_top_p(Narrator *n, float top_p);
void narrator_set_seed(Narrator *n, unsigned int seed);
void narrator_set_max_tokens(Narrator *n, int max_tokens);

/*
 * Generate narration for prompt, appending output to the narrator's stream
 * buffer as tokens are produced. Blocks until generation completes or
 * max_tokens is reached. Returns number of tokens generated, negative on error.
 *
 * The world state (room, inventory, recent events) should already be folded
 * into prompt by the caller.
 */
int narrator_generate(Narrator *n, const char *prompt);

/*
 * Read from the narrator's stream buffer at offset. Returns bytes copied into
 * buf. A short read means the caller has caught up with generation; it does
 * not mean generation has finished. Safe to call from another thread while
 * narrator_generate() runs.
 */
size_t narrator_read(Narrator *n, size_t offset, char *buf, size_t count);

/* Total bytes currently in the stream buffer. */
size_t narrator_len(Narrator *n);

/* Non-zero while a generation is in progress. */
int narrator_is_generating(Narrator *n);

/* Clear the stream buffer and reset the KV cache for a fresh scene. */
void narrator_reset(Narrator *n);

/* Human-readable status line for /info. Writes at most count bytes. */
size_t narrator_info(Narrator *n, char *buf, size_t count);

#endif /* NARRATOR_H */
