/* Standalone driver for the waterboxed RPCS3 core: runs core.wbx through
 * the miniBox host over a PS3 executable and reports per-frame RAM and TTY
 * digests in run-native's exact format, so the sandboxed build can be
 * diffed against the native reference.
 *
 * usage: run-wbx <core.wbx> [--firmware PS3UPDAT.PUP] [--frames N] [--report N] [--tty-out F]
 *        [--rewind] [--rerecord] <game.elf>
 *
 * The game is mounted under its own basename (extension drives type
 * detection) with "rom.name" carrying that name, exactly the frontend shape.
 */
#include "minibox.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_reader(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	memreader *m = (memreader *)ud;
	size_t take = s < (m->n - m->pos) ? s : (m->n - m->pos);
	memcpy(d, m->p + m->pos, take); m->pos += take; return (intptr_t)take;
}
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->b = realloc(m->b, m->cap); }
	memcpy(m->b + m->len, d, n); m->len += n; return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	uintptr_t avail = m->len - m->pos; if (n > avail) n = avail;
	memcpy(d, m->b + m->pos, n); m->pos += n; return (intptr_t)n;
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uint64_t (MB_GUEST_ABI *u64fn)(void);
typedef int64_t (MB_GUEST_ABI *i64fn)(void);

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	if (!r.data) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return r.data;
}

int main(int argc, char **argv)
{
	const char *core = NULL, *game = NULL, *ttyOut = NULL, *firmware = NULL;
	long frames = 60, report = 10;
	int rewind = 0, rerecord = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = atol(argv[++i]);
		else if (!strcmp(argv[i], "--tty-out") && i + 1 < argc) ttyOut = argv[++i];
		else if (!strcmp(argv[i], "--firmware") && i + 1 < argc) firmware = argv[++i];
		else if (!strcmp(argv[i], "--rewind")) rewind = 1;
		else if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!core) core = argv[i];
		else game = argv[i];
	}
	if (!core || !game) {
		fprintf(stderr, "usage: run-wbx <core.wbx> [--firmware PS3UPDAT.PUP] [--frames N] [--report N] [--tty-out F] [--rewind] [--rerecord] <game.elf>\n");
		return 2;
	}

	FILE *wf = fopen(core, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", core); return 1; }

	/* The PS3 is a big machine: a flat 4 GiB guest view, an 8+2 GiB
	 * execution table, a 2 GiB JIT arena and the emulator's own heap all
	 * come out of the mmap arena; the rest is rpcs3's small allocations. */
	mb_memory_layout_template layout = { 512u << 20, 16u << 20, 64u << 20, 256u << 20, (uintptr_t)20 << 30 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	/* the game under its original basename + rom.name, the frontend shape */
	const char *base = strrchr(game, '/');
	base = base ? base + 1 : game;
	char vfsname[512];
	snprintf(vfsname, sizeof vfsname, "/%s", base);
	wbx_mount_file_path(h, vfsname, game, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", vfsname, r.error_message); return 1; }
	memreader nr = { (const uint8_t *)vfsname, strlen(vfsname), 0 };
	wbx_mount_file(h, "rom.name", mem_reader, (uintptr_t)&nr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount rom.name: %s\n", r.error_message); return 1; }

	/* the firmware channel: Sony's PUP under its declared id, read lazily */
	if (firmware) {
		wbx_mount_file_path(h, "PS3UPDAT.PUP", firmware, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount firmware: %s\n", r.error_message); return 1; }
	}

	wbx_activate_host(h, &r);

	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) {
		ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	u64fn GetMainMemoryDigest = (u64fn)proc(h, "GetMainMemoryDigest");
	ptrfn GetTty = (ptrfn)proc(h, "GetTty");
	i64fn GetTtySize = (i64fn)proc(h, "GetTtySize");
	intfn GetThreadCount = (intfn)proc(h, "GetThreadCount");
	u64fn GetMachineTimeNs = (u64fn)proc(h, "GetMachineTimeNs");
	intfn IsRunning = (intfn)proc(h, "IsRunning");

	/* seal: the post-boot machine is the savestate baseline */
	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	printf("booted; threads %d\n", GetThreadCount());
	fflush(stdout);

	if (rewind) {
		long half = frames / 2;
		for (long f = 1; f <= half; f++) FrameAdvance(0);
		membuf st = {0};
		wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
		if (r.error_message[0]) { fprintf(stderr, "save: %s\n", r.error_message); return 1; }
		uint64_t pass1 = 0, pass2 = 0;
		for (long f = half + 1; f <= frames; f++) { FrameAdvance(0); uint64_t d = GetMainMemoryDigest(); pass1 = fnv(pass1, &d, sizeof d); }
		st.pos = 0;
		wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
		if (r.error_message[0]) { fprintf(stderr, "load: %s\n", r.error_message); return 1; }
		for (long f = half + 1; f <= frames; f++) { FrameAdvance(0); uint64_t d = GetMainMemoryDigest(); pass2 = fnv(pass2, &d, sizeof d); }
		printf("rewind: pass1 %016" PRIx64 " pass2 %016" PRIx64 " -> %s (state %zu bytes)\n", pass1, pass2,
		       pass1 == pass2 ? "EQUAL" : "DIFFERENT", st.len);
		free(st.b);
		wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
		return pass1 == pass2 ? 0 : 1;
	}

	for (long f = 1; f <= frames; f++) {
		if (rerecord) {
			membuf st = {0};
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "save@%ld: %s\n", f, r.error_message); return 1; }
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "load@%ld: %s\n", f, r.error_message); return 1; }
			free(st.b);
		}
		FrameAdvance(0);
		if (f % report == 0 || f == frames) {
			int64_t tn = GetTtySize();
			const uint8_t *tty = (const uint8_t *)GetTty();
			printf("frame %5ld ram %016" PRIx64 " tty %" PRId64 " %016" PRIx64 " threads %d time %" PRIu64 " running %d\n",
			       f, GetMainMemoryDigest(), tn, fnv(0, tty, (size_t)tn), GetThreadCount(),
			       GetMachineTimeNs() / 1000, IsRunning());
			fflush(stdout);
		}
	}
	if (ttyOut) {
		int64_t tn = GetTtySize();
		const uint8_t *tty = (const uint8_t *)GetTty();
		FILE *f = fopen(ttyOut, "wb");
		if (f) { fwrite(tty, 1, (size_t)tn, f); fclose(f); }
	}
	wbx_deactivate_host(h, &r);
	wbx_destroy_host(h, &r);
	printf("done\n");
	return 0;
}
