/*
 * narrator.c - 1-bit BitNet LLM narration engine.
 *
 * Wraps llama.cpp's decode loop. The model is loaded once and shared; each
 * session gets its own llama_context so players don't clobber each other's
 * KV cache.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llama.h"
#include "narrator.h"

/* Grow the stream buffer in chunks rather than per token. */
#define STREAM_CHUNK 4096

/* Longest single token piece llama_token_to_piece can emit. */
#define PIECE_MAX 256

static struct llama_model *g_model = NULL;
static int g_n_threads = 4;
static char g_model_name[128] = "unknown";

/*
 * Turn separator. BitNet-b1.58-2B-4T's chat template ends each turn with
 * <|eot_id|>, but its GGUF carries no tokenizer.ggml.eot_token_id, so
 * llama_vocab_is_eog() only recognises EOS and generation runs on past the
 * end of the reply. Resolved by name at load time; -1 if absent.
 */
static llama_token g_eot = -1;

struct Narrator {
	struct llama_context *ctx;
	struct llama_sampler *smpl;

	float temp;
	float top_p;
	unsigned int seed;
	int max_tokens;
	int n_ctx;
	int n_threads;

	/* Stream buffer. Guarded by mu so 9P readers can drain it while the
	 * generation thread appends. */
	pthread_mutex_t mu;
	char *buf;
	size_t len;
	size_t cap;

	int generating;
	unsigned long total_tokens;
	double tokens_per_sec;

	/* Scratch, reused across generations to avoid churn. */
	llama_token *tokens;
	int tokens_cap;
};

int
narrator_init_model(const char *model_path, int n_threads)
{
	struct llama_model_params mparams;

	if (g_model != NULL)
		return 0;

	llama_backend_init();

	mparams = llama_model_default_params();
	g_model = llama_model_load_from_file(model_path, mparams);
	if (g_model == NULL) {
		fprintf(stderr, "narrator: failed to load model %s\n", model_path);
		return -1;
	}

	g_n_threads = n_threads > 0 ? n_threads : 4;

	/* Report what actually loaded rather than a hardcoded guess. */
	if (llama_model_desc(g_model, g_model_name, sizeof(g_model_name)) <= 0)
		snprintf(g_model_name, sizeof(g_model_name), "%s", model_path);

	/* Locate the turn separator so generation stops at the end of a reply. */
	{
		const struct llama_vocab *vocab = llama_model_get_vocab(g_model);
		llama_token t;
		char buf[64];

		g_eot = -1;
		if (vocab != NULL) {
			const int nvocab = llama_vocab_n_tokens(vocab);
			for (t = 0; t < nvocab; t++) {
				int len = llama_token_to_piece(vocab, t, buf,
				    sizeof(buf) - 1, 0, true);
				if (len <= 0 || len >= (int)sizeof(buf))
					continue;
				buf[len] = '\0';
				if (strcmp(buf, "<|eot_id|>") == 0) {
					g_eot = t;
					break;
				}
			}
		}
	}

	return 0;
}

void
narrator_free_model(void)
{
	if (g_model != NULL) {
		llama_model_free(g_model);
		g_model = NULL;
	}
	llama_backend_free();
}

/*
 * Build the sampler chain from current settings. A temp of 0 or less means
 * greedy decoding, which keeps narration deterministic for testing.
 */
static struct llama_sampler *
build_sampler(Narrator *n)
{
	struct llama_sampler_chain_params sparams;
	struct llama_sampler *chain;

	sparams = llama_sampler_chain_default_params();
	chain = llama_sampler_chain_init(sparams);
	if (chain == NULL)
		return NULL;

	if (n->temp <= 0.0f) {
		llama_sampler_chain_add(chain, llama_sampler_init_greedy());
	} else {
		llama_sampler_chain_add(chain, llama_sampler_init_top_p(n->top_p, 1));
		llama_sampler_chain_add(chain, llama_sampler_init_temp(n->temp));
		llama_sampler_chain_add(chain, llama_sampler_init_dist(n->seed));
	}
	return chain;
}

Narrator *
narrator_new(int n_ctx, int n_threads)
{
	Narrator *n;
	struct llama_context_params cparams;

	if (g_model == NULL)
		return NULL;

	n = calloc(1, sizeof(Narrator));
	if (n == NULL)
		return NULL;

	n->temp = 0.7f;
	n->top_p = 0.9f;
	n->seed = (unsigned int)LLAMA_DEFAULT_SEED;
	n->max_tokens = 128;
	n->n_ctx = n_ctx > 0 ? n_ctx : 512;
	n->n_threads = n_threads > 0 ? n_threads : g_n_threads;

	if (pthread_mutex_init(&n->mu, NULL) != 0) {
		free(n);
		return NULL;
	}

	cparams = llama_context_default_params();
	cparams.n_ctx = n->n_ctx;
	/*
	 * n_batch covers the prompt prefill, which arrives in one go. n_ubatch
	 * must stay small: generation decodes one token at a time, and a large
	 * micro-batch makes every step pay for a full ubatch-sized graph.
	 */
	cparams.n_batch = n->n_ctx;
	cparams.n_ubatch = 128;
	cparams.n_threads = n->n_threads;
	cparams.n_threads_batch = n->n_threads;

	n->ctx = llama_init_from_model(g_model, cparams);
	if (n->ctx == NULL) {
		pthread_mutex_destroy(&n->mu);
		free(n);
		return NULL;
	}

	n->tokens_cap = n->n_ctx;
	n->tokens = calloc(n->tokens_cap, sizeof(llama_token));
	if (n->tokens == NULL) {
		llama_free(n->ctx);
		pthread_mutex_destroy(&n->mu);
		free(n);
		return NULL;
	}

	return n;
}

void
narrator_free(Narrator *n)
{
	if (n == NULL)
		return;
	if (n->smpl != NULL)
		llama_sampler_free(n->smpl);
	if (n->ctx != NULL)
		llama_free(n->ctx);
	free(n->tokens);
	free(n->buf);
	pthread_mutex_destroy(&n->mu);
	free(n);
}

void
narrator_set_temp(Narrator *n, float temp)
{
	if (n != NULL)
		n->temp = temp;
}

void
narrator_set_top_p(Narrator *n, float top_p)
{
	if (n != NULL && top_p > 0.0f && top_p <= 1.0f)
		n->top_p = top_p;
}

void
narrator_set_seed(Narrator *n, unsigned int seed)
{
	if (n != NULL)
		n->seed = seed;
}

void
narrator_set_max_tokens(Narrator *n, int max_tokens)
{
	if (n == NULL || max_tokens <= 0)
		return;
	/* Cap to the context window; a larger request can only fail mid-decode. */
	n->max_tokens = max_tokens < n->n_ctx ? max_tokens : n->n_ctx - 1;
}

/* Append to the stream buffer. Caller must hold n->mu. */
static int
stream_append(Narrator *n, const char *data, size_t len)
{
	size_t need;
	char *nb;

	if (len == 0)
		return 0;

	need = n->len + len + 1;
	if (need > n->cap) {
		size_t cap = n->cap ? n->cap : STREAM_CHUNK;
		while (cap < need)
			cap *= 2;
		nb = realloc(n->buf, cap);
		if (nb == NULL)
			return -1;
		n->buf = nb;
		n->cap = cap;
	}

	memcpy(n->buf + n->len, data, len);
	n->len += len;
	n->buf[n->len] = '\0';
	return 0;
}

int
narrator_generate(Narrator *n, const char *prompt)
{
	const struct llama_vocab *vocab;
	struct llama_batch batch;
	char piece[PIECE_MAX];
	int n_prompt, n_decoded, np;
	llama_token tok;
	double t0, t1;

	if (n == NULL || prompt == NULL || g_model == NULL)
		return -1;

	vocab = llama_model_get_vocab(g_model);
	if (vocab == NULL)
		return -1;

	/* Rebuild the sampler so ctl changes since the last prompt take effect. */
	if (n->smpl != NULL)
		llama_sampler_free(n->smpl);
	n->smpl = build_sampler(n);
	if (n->smpl == NULL)
		return -1;

	/*
	 * add_special adds BOS. parse_special must be true: buildprompt() emits
	 * the model's own <|eot_id|> turn separators, and they only work as
	 * separators if tokenized as control tokens rather than literal text.
	 */
	n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
	    n->tokens, n->tokens_cap, true, true);
	if (n_prompt < 0) {
		/* Prompt longer than the buffer: llama returns -needed. */
		fprintf(stderr, "narrator: prompt too long (%d tokens, cap %d)\n",
		    -n_prompt, n->tokens_cap);
		return -1;
	}
	if (n_prompt == 0)
		return 0;
	if (n_prompt >= n->n_ctx) {
		fprintf(stderr, "narrator: prompt fills context (%d >= %d)\n",
		    n_prompt, n->n_ctx);
		return -1;
	}

	pthread_mutex_lock(&n->mu);
	n->generating = 1;
	pthread_mutex_unlock(&n->mu);

	/* Fresh KV cache per generation: each narration is a standalone scene. */
	llama_memory_clear(llama_get_memory(n->ctx), true);

	t0 = (double)llama_time_us() / 1e6;

	/* Prefill. */
	batch = llama_batch_get_one(n->tokens, n_prompt);
	if (llama_decode(n->ctx, batch) != 0) {
		fprintf(stderr, "narrator: prefill decode failed\n");
		pthread_mutex_lock(&n->mu);
		n->generating = 0;
		pthread_mutex_unlock(&n->mu);
		return -1;
	}

	/* Decode loop: sample, emit, feed back. */
	n_decoded = 0;
	while (n_decoded < n->max_tokens) {
		tok = llama_sampler_sample(n->smpl, n->ctx, -1);

		if (llama_vocab_is_eog(vocab, tok) || (g_eot >= 0 && tok == g_eot))
			break;

		np = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, false);
		if (np < 0) {
			fprintf(stderr, "narrator: token_to_piece overflow\n");
			break;
		}

		/* Append the raw piece. Unlike the old word-scanner this preserves
		 * newlines, punctuation spacing and '/' so directions and paths
		 * survive intact. */
		pthread_mutex_lock(&n->mu);
		if (stream_append(n, piece, (size_t)np) != 0) {
			pthread_mutex_unlock(&n->mu);
			fprintf(stderr, "narrator: out of memory appending token\n");
			break;
		}
		n->total_tokens++;
		pthread_mutex_unlock(&n->mu);

		llama_sampler_accept(n->smpl, tok);

		/* Stop before overrunning the context window. */
		if (n_prompt + n_decoded + 1 >= n->n_ctx)
			break;

		batch = llama_batch_get_one(&tok, 1);
		if (llama_decode(n->ctx, batch) != 0) {
			fprintf(stderr, "narrator: decode failed at token %d\n", n_decoded);
			break;
		}
		n_decoded++;
	}

	t1 = (double)llama_time_us() / 1e6;

	pthread_mutex_lock(&n->mu);
	if (t1 > t0)
		n->tokens_per_sec = (double)n_decoded / (t1 - t0);
	n->generating = 0;
	pthread_mutex_unlock(&n->mu);

	return n_decoded;
}

size_t
narrator_read(Narrator *n, size_t offset, char *buf, size_t count)
{
	size_t avail;

	if (n == NULL || buf == NULL || count == 0)
		return 0;

	pthread_mutex_lock(&n->mu);
	if (offset >= n->len) {
		pthread_mutex_unlock(&n->mu);
		return 0;
	}
	avail = n->len - offset;
	if (avail > count)
		avail = count;
	memcpy(buf, n->buf + offset, avail);
	pthread_mutex_unlock(&n->mu);

	return avail;
}

size_t
narrator_len(Narrator *n)
{
	size_t len;

	if (n == NULL)
		return 0;
	pthread_mutex_lock(&n->mu);
	len = n->len;
	pthread_mutex_unlock(&n->mu);
	return len;
}

int
narrator_is_generating(Narrator *n)
{
	int g;

	if (n == NULL)
		return 0;
	pthread_mutex_lock(&n->mu);
	g = n->generating;
	pthread_mutex_unlock(&n->mu);
	return g;
}

void
narrator_reset(Narrator *n)
{
	if (n == NULL)
		return;
	pthread_mutex_lock(&n->mu);
	n->len = 0;
	if (n->buf != NULL)
		n->buf[0] = '\0';
	pthread_mutex_unlock(&n->mu);

	if (n->ctx != NULL)
		llama_memory_clear(llama_get_memory(n->ctx), true);
}

size_t
narrator_info(Narrator *n, char *buf, size_t count)
{
	int written;

	if (buf == NULL || count == 0)
		return 0;
	if (n == NULL) {
		written = snprintf(buf, count, "Status: NO SESSION\n");
		return written < 0 ? 0 : (size_t)written;
	}

	pthread_mutex_lock(&n->mu);
	written = snprintf(buf, count,
	    "Model: %s\n"
	    "Precision: 1.58-bit ternary, I2_S\n"
	    "Context: %d tokens\n"
	    "Threads: %d\n"
	    "Temperature: %.2f\n"
	    "Top-p: %.2f\n"
	    "Max tokens: %d\n"
	    "Tokens/sec: %.1f\n"
	    "Total tokens: %lu\n"
	    "Stream bytes: %lu\n"
	    "Status: %s\n",
	    g_model_name, n->n_ctx, n->n_threads, n->temp, n->top_p,
	    n->max_tokens, n->tokens_per_sec, n->total_tokens,
	    (unsigned long)n->len, n->generating ? "GENERATING" : "IDLE");
	pthread_mutex_unlock(&n->mu);

	return written < 0 ? 0 : (size_t)written;
}
