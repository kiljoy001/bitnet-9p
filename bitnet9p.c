/*
 * bitnet9p - 9P file server exposing a 1-bit BitNet LLM narrator for a MUD.
 *
 * Namespace:
 *	/info		read	engine + session status
 *	/ctl		write	sys <text> | temp <f> | top_p <f> | max <n> |
 *			        seed <n> | reset
 *	/world		r/w	pinned world state, prepended to every prompt
 *	/prompt		write	player action; triggers narration
 *	/stream		read	narration text, at any offset
 *
 * Each connection is served by its own process with its own file tree, so
 * every session gets an independent llama_context, world state and stream
 * buffer. The model itself is loaded once before any fork and shared
 * read-only.
 *
 * lib9p has no TCP listener (it serves a pair of fds), so main() runs the
 * accept loop and hands each connection to srv().
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "narrator.h"

enum {
	Qinfo = 1,
	Qctl,
	Qworld,
	Qprompt,
	Qstream,
};

enum {
	Maxworld = 16 * 1024,	/* pinned world state per session */
	Maxprompt = 8 * 1024,	/* single player action */
	Infobuf = 2 * 1024,
};

/* Per-file aux: which file this is, plus the session it belongs to. */
typedef struct Session Session;
typedef struct Fnode Fnode;

struct Session {
	Narrator	*nar;
	char		*sys;		/* narrator persona */
	char		*world;		/* pinned world state */
	long		worldlen;
	QLock		lk;		/* guards sys and world */
};

struct Fnode {
	int		type;
	Session		*s;
};

static char *modelpath;
static int nthreads = 4;
static int nctx = 512;

static char *defaultsys =
	"You are the narrator of a text adventure. Describe what the player "
	"perceives in vivid, concise second-person prose. Answer only with the "
	"description, in two or three sentences. Never break character, never "
	"address the player as an assistant, and never continue past the "
	"description.";

static char Enomem[] = "out of memory";
static char Enosession[] = "no session";

/* Turn separator from the model's chat template. */
static char Eot[] = "<|eot_id|>";

static Session *
sessionnew(void)
{
	Session *s;

	s = mallocz(sizeof(Session), 1);
	if (s == nil)
		return nil;

	s->nar = narrator_new(nctx, nthreads);
	if (s->nar == nil) {
		free(s);
		return nil;
	}
	s->sys = strdup(defaultsys);
	s->world = mallocz(1, 1);
	s->worldlen = 0;
	if (s->sys == nil || s->world == nil) {
		narrator_free(s->nar);
		free(s->sys);
		free(s->world);
		free(s);
		return nil;
	}
	return s;
}

static void
sessionfree(Session *s)
{
	if (s == nil)
		return;
	narrator_free(s->nar);
	free(s->sys);
	free(s->world);
	free(s);
}

/*
 * Assemble the full prompt: persona, pinned world state, then the player's
 * action. Folding the world in here is what makes the narrator answer in
 * context instead of free-associating from the action alone.
 *
 * The layout follows BitNet-b1.58-2B-4T's chat template:
 *	System: <text><|eot_id|>User: <text><|eot_id|>Assistant:
 * Using the model's own format matters -- feed it a bare completion prompt
 * and it emits stray "Assistant:" turns and loops.
 */
static char *
buildprompt(Session *s, char *action)
{
	char *p;
	int n;

	qlock(&s->lk);
	n = strlen(s->sys) + s->worldlen + strlen(action) + 128;
	p = mallocz(n, 1);
	if (p != nil) {
		/*
		 * World state goes in the System turn, alongside the persona.
		 * Putting it in the User turn instead was measurably worse: the
		 * model loses the scene and degenerates into repetition.
		 */
		if (s->worldlen > 0)
			snprint(p, n, "System: %s\n\n%s%sUser: %s%sAssistant:",
			    s->sys, s->world, Eot, action, Eot);
		else
			snprint(p, n, "System: %s%sUser: %s%sAssistant:",
			    s->sys, Eot, action, Eot);
	}
	qunlock(&s->lk);

	return p;
}

static void
fsread(Req *r)
{
	Fnode *fn;
	Session *s;
	char buf[Infobuf];
	long n;

	fn = r->fid->file->aux;
	if (fn == nil || fn->s == nil) {
		respond(r, Enosession);
		return;
	}
	s = fn->s;

	switch (fn->type) {
	case Qinfo:
		n = narrator_info(s->nar, buf, sizeof(buf));
		readbuf(r, buf, n);
		respond(r, nil);
		return;

	case Qctl:
		qlock(&s->lk);
		n = snprint(buf, sizeof(buf), "sys %s\n", s->sys);
		qunlock(&s->lk);
		readbuf(r, buf, n);
		respond(r, nil);
		return;

	case Qworld:
		qlock(&s->lk);
		readbuf(r, s->world, s->worldlen);
		qunlock(&s->lk);
		respond(r, nil);
		return;

	case Qprompt:
		readstr(r, "write a player action here to narrate it\n");
		respond(r, nil);
		return;

	case Qstream:
		/*
		 * Serve straight from the narrator buffer at the client's own
		 * offset: read forward and you get exactly the new text, with
		 * no diffing. A short read means you have caught up, not that
		 * generation has ended -- check /info for that.
		 */
		n = r->ifcall.count;
		if (n > (long)sizeof(buf))
			n = sizeof(buf);
		n = narrator_read(s->nar, (size_t)r->ifcall.offset, buf, n);
		if (n > 0)
			memmove(r->ofcall.data, buf, n);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}

	respond(r, "bug in fsread");
}

/* Parse and apply a /ctl command. Returns an error string, or nil on success. */
static char *
ctlwrite(Session *s, char *msg, int len)
{
	Cmdbuf *cb;
	char *err, *t;
	double f;
	int i, n, v;

	err = nil;
	cb = parsecmd(msg, len);
	if (cb == nil)
		return "parsecmd failed";
	if (cb->nf < 1) {
		free(cb);
		return "empty control message";
	}

	if (strcmp(cb->f[0], "sys") == 0) {
		if (cb->nf < 2) {
			err = "usage: sys <text>";
		} else {
			/* parsecmd split on whitespace; rejoin the persona text. */
			n = 1;
			for (i = 1; i < cb->nf; i++)
				n += strlen(cb->f[i]) + 1;
			t = mallocz(n, 1);
			if (t == nil) {
				err = Enomem;
			} else {
				for (i = 1; i < cb->nf; i++) {
					if (i > 1)
						strcat(t, " ");
					strcat(t, cb->f[i]);
				}
				qlock(&s->lk);
				free(s->sys);
				s->sys = t;
				qunlock(&s->lk);
			}
		}
	} else if (strcmp(cb->f[0], "temp") == 0) {
		if (cb->nf < 2) {
			err = "usage: temp <float>";
		} else {
			f = strtod(cb->f[1], nil);
			if (f < 0.0 || f > 4.0)
				err = "temp out of range [0,4]";
			else
				narrator_set_temp(s->nar, (float)f);
		}
	} else if (strcmp(cb->f[0], "top_p") == 0) {
		if (cb->nf < 2) {
			err = "usage: top_p <float>";
		} else {
			f = strtod(cb->f[1], nil);
			if (f <= 0.0 || f > 1.0)
				err = "top_p out of range (0,1]";
			else
				narrator_set_top_p(s->nar, (float)f);
		}
	} else if (strcmp(cb->f[0], "max") == 0) {
		if (cb->nf < 2) {
			err = "usage: max <int>";
		} else {
			v = atoi(cb->f[1]);
			if (v <= 0 || v >= nctx)
				err = "max out of range";
			else
				narrator_set_max_tokens(s->nar, v);
		}
	} else if (strcmp(cb->f[0], "seed") == 0) {
		if (cb->nf < 2)
			err = "usage: seed <int>";
		else
			narrator_set_seed(s->nar, (uint)strtoul(cb->f[1], nil, 10));
	} else if (strcmp(cb->f[0], "reset") == 0) {
		narrator_reset(s->nar);
	} else {
		err = "unknown control message";
	}

	free(cb);
	return err;
}

static void
fswrite(Req *r)
{
	Fnode *fn;
	Session *s;
	char *msg, *full, *err, *nb;
	vlong off;
	long n;
	int rc;

	fn = r->fid->file->aux;
	if (fn == nil || fn->s == nil) {
		respond(r, Enosession);
		return;
	}
	s = fn->s;
	n = r->ifcall.count;
	off = r->ifcall.offset;

	switch (fn->type) {
	case Qctl:
		msg = mallocz(n + 1, 1);
		if (msg == nil) {
			respond(r, Enomem);
			return;
		}
		memmove(msg, r->ifcall.data, n);
		err = ctlwrite(s, msg, n);
		free(msg);
		if (err != nil) {
			respond(r, err);
			return;
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;

	case Qworld:
		/*
		 * Pin the world by writing it here. Offset writes let a client
		 * build the state up incrementally.
		 */
		if (off < 0 || off + n > Maxworld) {
			respond(r, "world state too large");
			return;
		}
		qlock(&s->lk);
		if (off + n > s->worldlen) {
			nb = realloc(s->world, off + n + 1);
			if (nb == nil) {
				qunlock(&s->lk);
				respond(r, Enomem);
				return;
			}
			s->world = nb;
			/* A write past the end leaves a hole; zero it. */
			if (off > s->worldlen)
				memset(s->world + s->worldlen, 0, off - s->worldlen);
			s->worldlen = off + n;
			s->world[s->worldlen] = '\0';
		}
		memmove(s->world + off, r->ifcall.data, n);
		qunlock(&s->lk);
		r->ofcall.count = n;
		respond(r, nil);
		return;

	case Qprompt:
		if (n > Maxprompt) {
			respond(r, "prompt too large");
			return;
		}
		if (narrator_is_generating(s->nar)) {
			respond(r, "already generating");
			return;
		}

		msg = mallocz(n + 1, 1);
		if (msg == nil) {
			respond(r, Enomem);
			return;
		}
		memmove(msg, r->ifcall.data, n);

		full = buildprompt(s, msg);
		free(msg);
		if (full == nil) {
			respond(r, Enomem);
			return;
		}

		/* Clear previous narration so /stream offsets start from zero. */
		narrator_reset(s->nar);

		rc = narrator_generate(s->nar, full);
		free(full);
		if (rc < 0) {
			respond(r, "generation failed");
			return;
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}

	respond(r, "permission denied");
}

/* Attach a file to the tree, tagging it with its type and session. */
static int
mkfile(File *root, char *name, ulong perm, int type, Session *s)
{
	Fnode *fn;
	File *f;

	fn = mallocz(sizeof(Fnode), 1);
	if (fn == nil)
		return -1;
	fn->type = type;
	fn->s = s;

	f = createfile(root, name, "bitnet", perm, fn);
	if (f == nil) {
		free(fn);
		return -1;
	}
	return 0;
}

/* Build a fresh file tree for one connection. */
static Tree *
mktree(Session *s)
{
	Tree *t;
	File *root;

	t = alloctree("bitnet", "bitnet", DMDIR|0555, nil);
	if (t == nil)
		return nil;
	root = t->root;

	if (mkfile(root, "info", 0444, Qinfo, s) < 0
	 || mkfile(root, "ctl", 0666, Qctl, s) < 0
	 || mkfile(root, "world", 0666, Qworld, s) < 0
	 || mkfile(root, "prompt", 0222, Qprompt, s) < 0
	 || mkfile(root, "stream", 0444, Qstream, s) < 0) {
		freetree(t);
		return nil;
	}
	return t;
}

static void
usage(void)
{
	fprint(2, "usage: bitnet9p -m model.gguf [-a addr] [-t threads] [-c ctx] [-D]\n");
	fprint(2, "  -m  path to BitNet .gguf model (required)\n");
	fprint(2, "  -a  9P listen address (default tcp!*!5647)\n");
	fprint(2, "  -t  inference threads (default 4)\n");
	fprint(2, "  -c  context window in tokens (default 512)\n");
	fprint(2, "  -D  9P protocol tracing\n");
	threadexitsall("usage");
}

/*
 * Serve one connection to completion. Runs in its own process (proccreate),
 * so a slow narration blocks only that player. The session and tree are torn
 * down when the client disconnects.
 */
static void
serveconn(void *arg)
{
	Session *s;
	Srv *fs;
	Tree *t;
	int fd;

	fd = (int)(uintptr)arg;

	s = sessionnew();
	if (s == nil) {
		fprint(2, "bitnet9p: cannot create session\n");
		close(fd);
		threadexits("session");
	}

	t = mktree(s);
	if (t == nil) {
		fprint(2, "bitnet9p: cannot build tree\n");
		sessionfree(s);
		close(fd);
		threadexits("tree");
	}

	fs = mallocz(sizeof(Srv), 1);
	if (fs == nil) {
		freetree(t);
		sessionfree(s);
		close(fd);
		threadexits("nomem");
	}
	fs->tree = t;
	fs->read = fsread;
	fs->write = fswrite;
	fs->infd = fd;
	fs->outfd = fd;
	fs->nopipe = 1;

	/* Blocks until the client hangs up. */
	srv(fs);

	close(fd);
	sessionfree(s);
	free(fs);
	threadexits(nil);
}

void
threadmain(int argc, char **argv)
{
	char *addr, adir[40], ldir[40];
	int actl, lctl, fd;

	addr = "tcp!*!5647";

	ARGBEGIN {
	case 'm':
		modelpath = EARGF(usage());
		break;
	case 'a':
		addr = EARGF(usage());
		break;
	case 't':
		nthreads = atoi(EARGF(usage()));
		break;
	case 'c':
		nctx = atoi(EARGF(usage()));
		break;
	case 'D':
		chatty9p++;
		break;
	default:
		usage();
	} ARGEND

	if (modelpath == nil)
		usage();
	if (nthreads <= 0 || nctx <= 0)
		usage();

	/* Load before forking so every session shares one copy of the weights. */
	if (narrator_init_model(modelpath, nthreads) != 0)
		sysfatal("cannot load model %s", modelpath);

	actl = announce(addr, adir);
	if (actl < 0)
		sysfatal("announce %s: %r", addr);

	fprint(2, "bitnet9p: model loaded, serving 9P on %s\n", addr);

	for (;;) {
		lctl = listen(adir, ldir);
		if (lctl < 0) {
			fprint(2, "bitnet9p: listen: %r\n");
			continue;
		}
		fd = accept(lctl, ldir);
		if (fd < 0) {
			fprint(2, "bitnet9p: accept: %r\n");
			close(lctl);
			continue;
		}
		close(lctl);

		/*
		 * One process per connection: inference is CPU-bound and
		 * blocking, so a slow narration must not stall other players.
		 */
		proccreate(serveconn, (void*)(uintptr)fd, 32 * 1024);
	}
}
