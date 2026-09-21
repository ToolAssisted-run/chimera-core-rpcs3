/* Standalone driver for the waterboxed RPCS3 core: runs core.wbx through
 * the miniBox host over a PS3 executable and reports per-frame RAM and TTY
 * digests in run-native's exact format, so the sandboxed build can be
 * diffed against the native reference.
 *
 * usage: run-wbx <core.wbx> [--firmware PS3UPDAT.PUP] [--pkg file.pkg]... [--rap licence.rap]... [--settings JSON | --ports 1100000] [--cache DIR] [--precompile INDEX/COUNT[/game]] [--frames N] [--report N] [--tty-out F] [--video-out F]
 *        [--disc-copy PATH/ON/DISC [--disc-copy-raw] [--disc-verify]]
 *        [--log-trace CHANS] [--debug-at N]
 *        [--rewind] [--rerecord] [--save-state F] [--state F] <game.elf>
 *
 * The game is mounted under its own basename (extension drives type
 * detection) with "rom.name" carrying that name, exactly the frontend shape.
 */
#include "minibox.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <string.h>
#include "cache-bridge.h"
int chimera_cache_host_init(const char *dir);
const char *chimera_cache_host_description(void);
uintptr_t chimera_cache_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
#ifdef CHIMERA_GL_BRIDGE
#include "gl-bridge.h"
int chimera_gl_host_init(char *err, int errlen);
const char *chimera_gl_host_description(void);
uintptr_t chimera_gl_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
void chimera_gl_host_state_loaded(void);
unsigned long chimera_gl_host_unhandled(long *last_op);

/* What chimera's session does after a load (ce_gl_state_loaded), said here so
 * this runner asks the renderer the same question the frontend does. Without
 * it a load in this runner is a strictly EASIER test than a load in Chimera,
 * and the gate would be standing behind the easier one. */
static void gl_state_loaded(void)
{
	chimera_gl_host_state_loaded();
}

/* Every call the bridge shrugged at. Zero is a plausible answer to nearly
 * every opcode, so a run nobody answered looks exactly like a run that was
 * answered - which is how GL_OP_CONTEXT_ID went unanswered here for as long as
 * the opcode existed while gpu:flip passed. Said out loud so the gate can fail
 * on it instead of the log saying it to nobody. */
static void gl_report_unhandled(void)
{
	long last = 0;
	const unsigned long n = chimera_gl_host_unhandled(&last);
	if (n == 0) return;
	fprintf(stderr, "gpu bridge: %lu call(s) to opcodes this host has no case for"
		" (last: opcode %ld); every one was answered 0\n", n, last);
	fflush(stderr);
}
#else
static void gl_state_loaded(void) { }
static void gl_report_unhandled(void) { }
#endif

static void (MB_GUEST_ABI *g_coreStateLoaded)(void);

/* Both halves of "a state was loaded": the HOST mints a fresh GL context id
 * (the renderer's cue that the objects it remembers are another context's),
 * and the CORE is told, which is what lets a renderer whose stored id is still
 * its initial zero know that the zero cannot be trusted (chimera issue 126). */
static void state_was_loaded(void)
{
	gl_state_loaded();
	if (g_coreStateLoaded != NULL) g_coreStateLoaded();
}

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
typedef void (MB_GUEST_ABI *setfn_v)(uint64_t);
typedef void (MB_GUEST_ABI *prefn)(int32_t, int32_t, int32_t);
typedef uint32_t (MB_GUEST_ABI *u32fn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uint64_t (MB_GUEST_ABI *u64fn)(void);
typedef uint64_t (MB_GUEST_ABI *u64fn_i)(int32_t);
typedef void (MB_GUEST_ABI *voidfn)(void);
typedef int64_t (MB_GUEST_ABI *i64fn)(void);
typedef void (MB_GUEST_ABI *btnfn)(int32_t, int32_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	if (!r.data) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return r.data;
}

int main(int argc, char **argv)
{
#ifdef __linux__
	/* MB_ALLOW_PTRACE: let a gdb that is not an ancestor attach (yama scope 1).
	 * The guest is green threads on one host thread and its statics are in
	 * core.wbx's symbol table, so an attached gdb can read the scheduler's own
	 * state - which is how a thread that never gives way is found. */
	if (getenv("MB_ALLOW_PTRACE")) prctl(PR_SET_PTRACER, -1L, 0, 0, 0);
#endif
	const char *core = NULL, *game = NULL, *ttyOut = NULL, *videoOut = NULL, *firmware = NULL, *ramOut = NULL, *dkey = NULL, *settingsJson = NULL, *cacheDir = NULL, *preSpec = NULL;
	const char *logTrace = NULL;
	long debugAt = -1;
	long frames = 60, report = 10;
	int rewind = 0, rerecord = 0;
	/* A state written to a file, and one read from one: the only way to ask
	 * what happens to a machine whose GL context is not the one it drew on,
	 * because that question needs two PROCESSES. */
	const char *stateOut = NULL, *stateIn = NULL;
	/* a game's data install in miniature: this file, off the disc onto the hard
	 * disk, once the machine is up, and read back. --disc-copy-raw reads the
	 * image as it lies (the control), --disc-verify reads back without copying
	 * (for a machine that came out of a savestate). */
	const char *discCopy = NULL;
	int discCopyFlags = 0;
	struct { long first, count; int index; } press[32];
	int presses = 0;
	/* the pkg and rap slots: any number of each, reaching the guest through the
	 * "slots" map the frontend mounts (waterbox_slots.h), because nothing else
	 * can carry a LIST of files under names the core reads them by */
	const char *pkgs[32], *raps[32];
	int npkgs = 0, nraps = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = atol(argv[++i]);
		else if (!strcmp(argv[i], "--tty-out") && i + 1 < argc) ttyOut = argv[++i];
		else if (!strcmp(argv[i], "--log-trace") && i + 1 < argc) logTrace = argv[++i];
		else if (!strcmp(argv[i], "--debug-at") && i + 1 < argc) debugAt = atol(argv[++i]);
		else if (!strcmp(argv[i], "--firmware") && i + 1 < argc) firmware = argv[++i];
		else if (!strcmp(argv[i], "--dkey") && i + 1 < argc) dkey = argv[++i];
		else if (!strcmp(argv[i], "--disc-copy") && i + 1 < argc) discCopy = argv[++i];
		else if (!strcmp(argv[i], "--disc-copy-raw")) discCopyFlags |= 1;
		else if (!strcmp(argv[i], "--disc-verify")) discCopyFlags |= 2;
		else if (!strcmp(argv[i], "--pkg") && i + 1 < argc) {
			if (npkgs == 32) { fprintf(stderr, "too many --pkg\n"); return 2; }
			pkgs[npkgs++] = argv[++i];
		}
		else if (!strcmp(argv[i], "--rap") && i + 1 < argc) {
			if (nraps == 32) { fprintf(stderr, "too many --rap\n"); return 2; }
			raps[nraps++] = argv[++i];
		}
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc) {
			if (settingsJson) { fprintf(stderr, "--ports and --settings: say the ports in the settings\n"); return 2; }
			settingsJson = argv[++i];
		}
		else if (!strcmp(argv[i], "--ports") && i + 1 < argc) {
			/* run-native's port mask ("1100000": ports 1 and 2), sent the way the
			 * frontend sends it - as the port1..port7 settings - so one gate
			 * invocation plugs in the same pads in both runners */
			const char *mask = argv[++i];
			static char portsJson[256];
			int at = snprintf(portsJson, sizeof portsJson, "{");
			for (int pi = 0; pi < 7; pi++)
				at += snprintf(portsJson + at, sizeof portsJson - at, "%s\"port%d\":\"%s\"", pi ? "," : "", pi + 1,
				               (mask[0] && (int)strlen(mask) > pi && mask[pi] == '1') ? "dualshock3" : "none");
			snprintf(portsJson + at, sizeof portsJson - at, "}");
			if (settingsJson) { fprintf(stderr, "--ports and --settings: say the ports in the settings\n"); return 2; }
			settingsJson = portsJson;
		}
		else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cacheDir = argv[++i];
		else if (!strcmp(argv[i], "--precompile") && i + 1 < argc) preSpec = argv[++i];
		else if (!strcmp(argv[i], "--ram-out") && i + 1 < argc) ramOut = argv[++i];
		else if (!strcmp(argv[i], "--video-out") && i + 1 < argc) videoOut = argv[++i];
		else if (!strcmp(argv[i], "--press") && i + 1 < argc && presses < 32) {
			long a, b; int c;
			if (sscanf(argv[++i], "%ld:%ld:%d", &a, &b, &c) == 3) {
				press[presses].first = a; press[presses].count = b; press[presses].index = c; presses++;
			}
		}
		else if (!strcmp(argv[i], "--rewind")) rewind = 1;
		else if (!strcmp(argv[i], "--save-state") && i + 1 < argc) stateOut = argv[++i];
		else if (!strcmp(argv[i], "--state") && i + 1 < argc) stateIn = argv[++i];
		else if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!core) core = argv[i];
		else game = argv[i];
	}
	if (!core || !game) {
		fprintf(stderr, "usage: run-wbx <core.wbx> [--firmware PS3UPDAT.PUP] [--pkg file.pkg]... [--rap licence.rap]... [--settings JSON | --ports 1100000] [--cache DIR] [--precompile INDEX/COUNT[/game]] [--frames N] [--report N] [--tty-out F] [--video-out F] [--rewind] [--rerecord] [--save-state F] [--state F] <game.elf>\n");
		return 2;
	}

	FILE *wf = fopen(core, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", core); return 1; }

	/* The PS3 is a big machine: a flat 4 GiB guest view (an 8 GiB reserve
	 * for alignment), a 10 GiB execution table, a 2 GiB JIT arena, LLVM's
	 * code space and the emulator's large allocations all come out of the
	 * mmap arena; LLVM's compiles want a real heap. Windows backs the block
	 * lazily (miniBox spec v2), so the size is a reservation. */
	mb_memory_layout_template layout = { 1024u << 20, 16u << 20, 64u << 20, 256u << 20, (uintptr_t)40 << 30 };
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

	/* The packages and the licences, each under its own basename, plus the
	 * "slots" map that names them - exactly the shape a project mounts. The
	 * game keeps arriving as rom.name, so the map names no "game" slot and the
	 * guest falls back to it as it does for a rom opened directly. */
	if (npkgs || nraps) {
		static char slotsJson[8192];
		int at = snprintf(slotsJson, sizeof slotsJson, "{");
		for (int k = 0; k < 2; k++) {
			const char **list = k ? raps : pkgs;
			const int n = k ? nraps : npkgs;
			if (!n) continue;
			at += snprintf(slotsJson + at, sizeof slotsJson - at, "%s\"%s\":[", at > 1 ? "," : "", k ? "rap" : "pkg");
			for (int j = 0; j < n; j++) {
				const char *b = strrchr(list[j], '/');
				b = b ? b + 1 : list[j];
				wbx_mount_file_path(h, b, list[j], &r);
				if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", b, r.error_message); return 1; }
				at += snprintf(slotsJson + at, sizeof slotsJson - at, "%s\"%s\"", j ? "," : "", b);
			}
			at += snprintf(slotsJson + at, sizeof slotsJson - at, "]");
		}
		snprintf(slotsJson + at, sizeof slotsJson - at, "}");
		memreader sl = { (const uint8_t *)slotsJson, strlen(slotsJson), 0 };
		wbx_mount_file(h, "slots", mem_reader, (uintptr_t)&sl, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount slots: %s\n", r.error_message); return 1; }
	}

	/* the log channels to raise, as a file: a sandboxed guest is handed no
	 * environment, so CHIMERA_LOG_TRACE cannot reach it any other way */
	if (logTrace) {
		memreader lr = { (const uint8_t *)logTrace, strlen(logTrace), 0 };
		wbx_mount_file(h, "logtrace", mem_reader, (uintptr_t)&lr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount logtrace: %s\n", r.error_message); return 1; }
	}

	/* which file to copy off the disc once the machine is up, for the leg that
	 * asks what a game's own data install costs the machine (DiscCopyProbe) */
	if (discCopy) {
		static char spec[600];
		snprintf(spec, sizeof spec, "%s\n%s %s", discCopy,
			(discCopyFlags & 1) ? "raw" : "", (discCopyFlags & 2) ? "verify" : "");
		memreader dr = { (const uint8_t *)spec, strlen(spec), 0 };
		wbx_mount_file(h, "disccopy", mem_reader, (uintptr_t)&dr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount disccopy: %s\n", r.error_message); return 1; }
	}

	/* the settings channel, exactly as the frontend mounts it */
	if (settingsJson) {
		memreader sr = { (const uint8_t *)settingsJson, strlen(settingsJson), 0 };
		wbx_mount_file(h, "settings", mem_reader, (uintptr_t)&sr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount settings: %s\n", r.error_message); return 1; }
	}

	/* the disc key slot, under the frontend's canonical name */
	if (dkey) {
		wbx_mount_file_path(h, "dkey", dkey, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount dkey: %s\n", r.error_message); return 1; }
	}

	/* the firmware channel: Sony's PUP under its declared id, read lazily */
	if (firmware) {
		wbx_mount_file_path(h, "PS3UPDAT.PUP", firmware, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount firmware: %s\n", r.error_message); return 1; }
	}

	wbx_activate_host(h, &r);

	/* The compile cache (cache-bridge.h): a directory the host keeps the
	 * core's compiled objects in, across sessions. Handed over before Init. */
	if (cacheDir) {
		if (chimera_cache_host_init(cacheDir) != 0) {
			fprintf(stderr, "compile cache: cannot use %s\n", cacheDir);
		} else {
			mb_return cr;
			wbx_get_proc_addr(h, "SetCacheBridge", &cr);
			setfn_v set_cache = (setfn_v)cr.data;
			wbx_get_callback_addr(h, (mb_external_callback)chimera_cache_host_dispatch, 0, &cr);
			if (!cr.data || !set_cache) fprintf(stderr, "compile cache: could not register the callback\n");
			else set_cache((uint64_t)cr.data);
		}
	}

	/* The GPU bridge (see waterbox/gl-shim.cpp). Off unless CHIMERA_GPU=1
	 * asks, and a machine with no usable driver keeps the null renderer.
	 * Handed over BEFORE Init, where the renderer is chosen. */
#ifdef CHIMERA_GL_BRIDGE
	{
		const char *want = getenv("CHIMERA_GPU");
		if (want && strcmp(want, "0") != 0) {
			char glerr[256] = "";
			if (chimera_gl_host_init(glerr, sizeof glerr) != 0) {
				fprintf(stderr, "gpu bridge: no context (%s); the null renderer stays\n", glerr);
			} else {
				mb_return gr;
				wbx_get_proc_addr(h, "SetGpuBridge", &gr);
				setfn_v set_bridge = (setfn_v)gr.data;
				wbx_get_callback_addr(h, (mb_external_callback)chimera_gl_host_dispatch, 0, &gr);
				if (!gr.data || !set_bridge) {
					fprintf(stderr, "gpu bridge: could not register the callback\n");
				} else {
					fprintf(stderr, "gpu bridge: %s\n", chimera_gl_host_description());
					set_bridge((uint64_t)gr.data);
				}
			}
		}
	}
#endif

	/* a precompile session: worker INDEX of COUNT, "/game" for the game's
	 * directories only; set before Init, pumped until done below */
	int preIndex = -1, preCount = 0;
	if (preSpec) {
		if (sscanf(preSpec, "%d/%d", &preIndex, &preCount) != 2 || preIndex < 0 || preCount < 1 || preIndex >= preCount) {
			fprintf(stderr, "bad --precompile %s (want INDEX/COUNT)\n", preSpec); return 2;
		}
		prefn SetPrecompile = (prefn)proc(h, "SetPrecompile");
		if (!SetPrecompile) { fprintf(stderr, "this core has no precompile session\n"); return 2; }
		SetPrecompile(preIndex, preCount, strstr(preSpec, "/game") == NULL ? 1 : 0);
	}

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
	/* The core's OPTIONAL StateLoaded export, resolved without proc() because a
	 * core that does not have it is not an error. Called wherever chimera's
	 * session calls it (afterStateLoaded): once the machine's memory has been
	 * replaced and before it runs again. Without it this runner's loads are
	 * quieter than the frontend's, and the gate would stand behind the quieter
	 * of the two. */
	{
		mb_return sr;
		memset(&sr, 0, sizeof sr);
		wbx_get_proc_addr(h, "StateLoaded", &sr);
		g_coreStateLoaded = (sr.error_message[0] || sr.data == 0) ? NULL : (voidfn)sr.data;
	}
	btnfn SetButton = (btnfn)proc(h, "SetButton");
	intfn InputWasRead = (intfn)proc(h, "InputWasRead");
	ptrfn GetVideoBgra = (ptrfn)proc(h, "GetVideoBgra");
	intfn GetVideoWidth = (intfn)proc(h, "GetVideoWidth");
	intfn GetVideoHeight = (intfn)proc(h, "GetVideoHeight");
	ptrfn GetAudio = (ptrfn)proc(h, "GetAudio");
	intfn GetAudioSampleCount = (intfn)proc(h, "GetAudioSampleCount");

	/* seal: the post-boot machine is the savestate baseline */
	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	printf("booted; threads %d\n", GetThreadCount());
	fflush(stdout);

	if (stateIn) {
		/* the machine somebody else saved, in a session of its own */
		FILE *f = fopen(stateIn, "rb");
		if (!f) { fprintf(stderr, "state: cannot read %s\n", stateIn); return 1; }
		membuf st = {0};
		fseek(f, 0, SEEK_END); st.len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
		st.b = malloc(st.len);
		if (!st.b || fread(st.b, 1, st.len, f) != st.len) { fprintf(stderr, "state: short read\n"); return 1; }
		fclose(f);
		st.pos = 0;
		wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
		if (r.error_message[0]) { fprintf(stderr, "load: %s\n", r.error_message); return 1; }
		state_was_loaded();
		free(st.b);
		printf("loaded state %s\n", stateIn);
		fflush(stdout);
	}

	/* The copy BEFORE the frames, so that a --save-state at the last frame
	 * carries a machine that already holds the reference to the disc, and a
	 * --state --disc-verify run reads it back out of one that was loaded. */
	if (discCopy) {
		typedef int64_t (MB_GUEST_ABI *i64fn0)(void);
		i64fn0 DiscCopyProbe = (i64fn0)proc(h, "DiscCopyProbe");
		u64fn_i MemfsStat = (u64fn_i)proc(h, "GetMemfsStat");
		long long copied = (long long)DiscCopyProbe();
		printf("disc copy %s: %lld bytes, %llu held, %llu kept, %llu decrypted\n", discCopy, copied,
			(unsigned long long)MemfsStat(2), (unsigned long long)MemfsStat(3), (unsigned long long)MemfsStat(4));
		fflush(stdout);
	}

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
		state_was_loaded();
		for (long f = half + 1; f <= frames; f++) { FrameAdvance(0); uint64_t d = GetMainMemoryDigest(); pass2 = fnv(pass2, &d, sizeof d); }
		printf("rewind: pass1 %016" PRIx64 " pass2 %016" PRIx64 " -> %s (state %zu bytes)\n", pass1, pass2,
		       pass1 == pass2 ? "EQUAL" : "DIFFERENT", st.len);
		free(st.b);
		gl_report_unhandled();
		wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
		return pass1 == pass2 ? 0 : 1;
	}

	long lag = 0;
	if (preCount > 0) {
		intfn IsPrecompileDone = (intfn)proc(h, "IsPrecompileDone");
		u32fn GetDone = (u32fn)proc(h, "GetPrecompileDone");
		u32fn GetTotal = (u32fn)proc(h, "GetPrecompileTotal");
		uint32_t last = ~0u;
		while (!IsPrecompileDone()) {
			FrameAdvance(0);
			uint32_t done = GetDone(), total = GetTotal();
			if (done != last) { fprintf(stderr, "Precompiled %u/%u modules\n", done, total); last = done; }
		}
		printf("precompiled %u/%u modules (worker %d of %d)\n", GetDone(), GetTotal(), preIndex, preCount);
		frames = 0;
	}
	for (long f = 1; f <= frames; f++) {
		for (int pi = 0; pi < presses; pi++)
			SetButton(press[pi].index, f >= press[pi].first && f < press[pi].first + press[pi].count);
		if (rerecord) {
			membuf st = {0};
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "save@%ld: %s\n", f, r.error_message); return 1; }
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "load@%ld: %s\n", f, r.error_message); return 1; }
			state_was_loaded();
			free(st.b);
		}
		FrameAdvance(0);
		if (!InputWasRead()) lag++;
		if (debugAt == f) {
			voidfn DebugThreads = (voidfn)proc(h, "DebugThreads");
			if (DebugThreads) DebugThreads();
		}
		if (stateOut && f == frames) {
			/* the machine as it stands, for another process to pick up */
			membuf st = {0};
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "save: %s\n", r.error_message); return 1; }
			FILE *sf = fopen(stateOut, "wb");
			if (!sf || fwrite(st.b, 1, st.len, sf) != st.len) { fprintf(stderr, "state: cannot write %s\n", stateOut); return 1; }
			fclose(sf);
			free(st.b);
			printf("saved state %s (%zu bytes)\n", stateOut, st.len);
			fflush(stdout);
		}
		if (f % report == 0 || f == frames) {
			int64_t tn = GetTtySize();
			const uint8_t *tty = (const uint8_t *)GetTty();
			int vw = GetVideoWidth(), vh = GetVideoHeight(), an = GetAudioSampleCount();
			const uint8_t *vid = (const uint8_t *)GetVideoBgra();
			const uint8_t *aud = (const uint8_t *)GetAudio();
			printf("frame %5ld ram %016" PRIx64 " tty %" PRId64 " %016" PRIx64 " vid %dx%d %016" PRIx64 " aud %d %016" PRIx64 " lag %ld threads %d time %" PRIu64 " running %d\n",
			       f, GetMainMemoryDigest(), tn, fnv(0, tty, (size_t)tn), vw, vh, fnv(0, vid, (size_t)vw * vh * 4),
			       an, fnv(0, aud, (size_t)an * 4), lag, GetThreadCount(),
			       GetMachineTimeNs() / 1000, IsRunning());
			fflush(stdout);
		}
	}
	if (cacheDir) {
		u64fn GetCacheStored = (u64fn)proc(h, "GetCacheStored");
		u64fn GetCacheFetched = (u64fn)proc(h, "GetCacheFetched");
		fprintf(stderr, "compile cache: %llu stored, %llu fetched (%s)\n",
			(unsigned long long)(GetCacheStored ? GetCacheStored() : 0), (unsigned long long)(GetCacheFetched ? GetCacheFetched() : 0),
			chimera_cache_host_description());
	}
	{
		u64fn GetFaultCount = (u64fn)proc(h, "GetFaultCount");
		if (GetFaultCount && GetFaultCount())
			fprintf(stderr, "page faults served by the renderer: %llu\n", (unsigned long long)GetFaultCount());
		u64fn GetWindowCount = (u64fn)proc(h, "GetWindowCount");
		if (GetWindowCount && GetWindowCount())
			fprintf(stderr, "faults of the RSX's own served by opening the page: %llu\n", (unsigned long long)GetWindowCount());
		u64fn_i GetMemfsStat = (u64fn_i)proc(h, "GetMemfsStat");
		fprintf(stderr, "memory files: %llu bytes, %llu file(s) held as the disc's (%llu bytes), %llu bytes copied in, %llu bytes decrypted to tell\n",
			(unsigned long long)GetMemfsStat(0), (unsigned long long)GetMemfsStat(1),
			(unsigned long long)GetMemfsStat(2), (unsigned long long)GetMemfsStat(3),
			(unsigned long long)GetMemfsStat(4));
	}

	if (ramOut) {
		/* the memory domain the frontend sees: the main block, read through
		 * the host's view of the guest */
		ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
		i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");
		const uint8_t *ram = (const uint8_t *)GetMemoryDomainPtr(0);
		FILE *rf = fopen(ramOut, "wb");
		if (rf) { fwrite(ram, 1, (size_t)GetMemoryDomainSize(0), rf); fclose(rf); }
	}
	if (videoOut) {
		/* the last frame's picture, in run-native's format so one script reads
		 * either flavour: two little-endian u32 (width, height), then BGRA rows
		 * top-down. A digest says two pictures differ; this says HOW, which is
		 * what a report of graphical artifacts needs. */
		int vw = GetVideoWidth(), vh = GetVideoHeight();
		const uint8_t *vid = (const uint8_t *)GetVideoBgra();
		FILE *f = fopen(videoOut, "wb");
		if (f) {
			uint32_t hdr[2] = { (uint32_t)vw, (uint32_t)vh };
			fwrite(hdr, 4, 2, f);
			if (vid && vw > 0 && vh > 0) fwrite(vid, 4, (size_t)vw * vh, f);
			fclose(f);
		}
	}
	if (ttyOut) {
		int64_t tn = GetTtySize();
		const uint8_t *tty = (const uint8_t *)GetTty();
		FILE *f = fopen(ttyOut, "wb");
		if (f) { fwrite(tty, 1, (size_t)tn, f); fclose(f); }
	}
	gl_report_unhandled();
	wbx_deactivate_host(h, &r);
	wbx_destroy_host(h, &r);
	printf("done\n");
	return 0;
}
