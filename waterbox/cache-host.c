/* The host's side of the compile-cache bridge: a directory of files named by
 * the guest (waterbox/cache-bridge.h from miniBox). Used by run-wbx and, in
 * the same process, by the native reference; the frontend brings its own.
 * SPDX-License-Identifier: MIT */
#include "cache-bridge.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char s_dir[4096];
static uint64_t s_fetched, s_stored;

int chimera_cache_host_init(const char *dir)
{
	if (!dir || strlen(dir) >= sizeof s_dir - 1) return -1;
	snprintf(s_dir, sizeof s_dir, "%s", dir);
	mkdir(s_dir, 0777);
	return 0;
}

/* a relative path with no absolute prefix and no '..' segment, or NULL */
static char *safe_path(const char *name, uint64_t len)
{
	if (!name || !len || len > 1024 || name[0] == '/') return NULL;
	static char full[6144];
	size_t base = strlen(s_dir);
	memcpy(full, s_dir, base);
	full[base] = '/';
	memcpy(full + base + 1, name, len);
	full[base + 1 + len] = 0;
	const char *p = full + base + 1;
	for (const char *seg = p; *seg; ) {
		const char *end = strchr(seg, '/');
		size_t n = end ? (size_t)(end - seg) : strlen(seg);
		if ((n == 2 && seg[0] == '.' && seg[1] == '.') || n == 0) return NULL;
		if (!end) break;
		seg = end + 1;
	}
	return full;
}

static void make_dirs(char *full)
{
	for (char *p = full + strlen(s_dir) + 1; *p; p++) {
		if (*p == '/') { *p = 0; mkdir(full, 0777); *p = '/'; }
	}
}

#if defined(_WIN32) && defined(__GNUC__)
#define BRIDGE_ABI __attribute__((sysv_abi))
#else
#define BRIDGE_ABI
#endif

uintptr_t BRIDGE_ABI chimera_cache_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b,
                                                 uintptr_t c, uintptr_t d, uintptr_t e)
{
	(void)b; (void)c; (void)d; (void)e;
	switch (op) {
	case CACHE_OP_FETCH: {
		struct CacheFetchArgs *args = (struct CacheFetchArgs *)a;
		char *full = safe_path((const char *)(uintptr_t)args->name, args->name_len);
		if (!full) return 0;
		FILE *f = fopen(full, "rb");
		if (!f) return 0;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		if (size <= 0) { fclose(f); return 0; }
		if (args->cap >= (uint64_t)size && args->dst) {
			fseek(f, 0, SEEK_SET);
			if (fread((void *)(uintptr_t)args->dst, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 0; }
			s_fetched++;
		}
		fclose(f);
		return (uintptr_t)size;
	}
	case CACHE_OP_STORE: {
		struct CacheStoreArgs *args = (struct CacheStoreArgs *)a;
		char *full = safe_path((const char *)(uintptr_t)args->name, args->name_len);
		if (!full || !args->size) return 0;
		make_dirs(full);
		/* write beside, then rename: a reader never sees a half file */
		char tmp[6200];
		snprintf(tmp, sizeof tmp, "%s.part", full);
		FILE *f = fopen(tmp, "wb");
		if (!f) return 0;
		int ok = fwrite((const void *)(uintptr_t)args->data, 1, (size_t)args->size, f) == (size_t)args->size;
		ok = fclose(f) == 0 && ok;
		if (!ok || rename(tmp, full) != 0) { remove(tmp); return 0; }
		s_stored++;
		return 1;
	}
	}
	return 0;
}

const char *chimera_cache_host_description(void)
{
	static char text[256];
	snprintf(text, sizeof text, "%llu stored, %llu fetched from %s",
	         (unsigned long long)s_stored, (unsigned long long)s_fetched, s_dir);
	return text;
}
