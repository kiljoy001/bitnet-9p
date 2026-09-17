/*
 * narrate - minimal client for the bitnet9p narrator.
 *
 * Pins world state, sends a player action, and prints the narration, all over
 * a single 9P connection so the session (and its llama_context) persists
 * across the writes. This is the shape a MUD would use: connect once per
 * player, then read and write files for the life of the session.
 *
 *	narrate [-a addr] [-w worldfile] action...
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <9pclient.h>

static char *addr = "tcp!127.0.0.1!5647";

static void
usage(void)
{
	fprint(2, "usage: narrate [-a addr] [-w worldfile] action...\n");
	exits("usage");
}

/* Write the whole of buf to the named file, starting at offset 0. */
static int
putfile(CFsys *fs, char *name, char *buf, long n)
{
	CFid *fid;

	fid = fsopen(fs, name, OWRITE);
	if (fid == nil) {
		fprint(2, "narrate: open %s: %r\n", name);
		return -1;
	}
	if (fswrite(fid, buf, n) != n) {
		fprint(2, "narrate: write %s: %r\n", name);
		fsclose(fid);
		return -1;
	}
	fsclose(fid);
	return 0;
}

/* Stream a file to stdout from offset 0 until a short read. */
static void
catfile(CFsys *fs, char *name)
{
	char buf[4096];
	CFid *fid;
	long n;

	fid = fsopen(fs, name, OREAD);
	if (fid == nil) {
		fprint(2, "narrate: open %s: %r\n", name);
		return;
	}
	while ((n = fsread(fid, buf, sizeof(buf))) > 0)
		write(1, buf, n);
	fsclose(fid);
}

void
threadmain(int argc, char **argv)
{
	char *worldfile, *action, *wbuf;
	CFsys *fs;
	int i, len, off, fd;

	worldfile = nil;

	ARGBEGIN {
	case 'a':
		addr = EARGF(usage());
		break;
	case 'w':
		worldfile = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND

	if (argc < 1)
		usage();

	/* Join the action words back into one line. */
	len = 1;
	for (i = 0; i < argc; i++)
		len += strlen(argv[i]) + 1;
	action = mallocz(len, 1);
	if (action == nil)
		sysfatal("out of memory");
	for (i = 0; i < argc; i++) {
		if (i > 0)
			strcat(action, " ");
		strcat(action, argv[i]);
	}

	/*
	 * One connection for the whole run: the server gives each connection
	 * its own session, so world and prompt must share this fd.
	 */
	fd = dial(addr, nil, nil, nil);
	if (fd < 0)
		sysfatal("dial %s: %r", addr);
	fs = fsmount(fd, nil);
	if (fs == nil)
		sysfatal("fsmount %s: %r", addr);

	/* Pin the world first so it is in place before the prompt runs. */
	if (worldfile != nil) {
		int wfd;
		long n;

		wfd = open(worldfile, OREAD);
		if (wfd < 0)
			sysfatal("open %s: %r", worldfile);
		wbuf = mallocz(16 * 1024, 1);
		if (wbuf == nil)
			sysfatal("out of memory");
		off = 0;
		while ((n = read(wfd, wbuf + off, 16 * 1024 - off - 1)) > 0)
			off += n;
		close(wfd);
		if (off > 0 && putfile(fs, "world", wbuf, off) < 0)
			exits("world");
		free(wbuf);
	}

	/* Writing the prompt blocks until generation completes. */
	if (putfile(fs, "prompt", action, strlen(action)) < 0)
		exits("prompt");

	catfile(fs, "stream");
	write(1, "\n", 1);

	fsunmount(fs);
	exits(nil);
}
