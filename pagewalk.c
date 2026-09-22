/*
 * pagewalk.c - deterministic page-access benchmark for kernel memory
 *              instrumentation validation.
 *
 * Every phase has an exactly predictable number of page accesses, page
 * allocations, and faults. No auxiliary data structures are touched during
 * measured phases, so a kernel-side counter filtered to the benchmark region
 * should match the printed "expected" values exactly.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o pagewalk pagewalk.c
 *
 * Run (see ENVIRONMENT below for required sysctl state):
 *   setarch -R ./pagewalk --log2n 19 --passes 3
 *
 * ---------------------------------------------------------------------------
 * PHASES (each is announced via prctl(PR_SET_NAME) as "ph:<name>")
 *
 *   ph:idle       mmap done, nothing touched.   0 faults, 0 pages, 0 accesses
 *   ph:populate   build the pointer cycle.      n minor faults, n new pages,
 *                                               n accesses (1 per page)
 *   ph:traverse   one pass per --passes.        n accesses/pass, 0 faults,
 *                                               0 new pages
 *   ph:evict      madvise(MADV_PAGEOUT).        0 accesses, e pages evicted
 *   ph:refault    one pass after eviction.      n accesses, e major faults,
 *                                               e refaults, 0 new pages
 *
 * "n accesses" means n distinct 4 KiB pages touched, once each, in a
 * permutation order, with exactly one cacheline touched per page. If your
 * patch counts hardware A-bit samples, expect <= n per scan window. If it
 * counts software-visible accesses via PTE-clearing, expect exactly n.
 *
 * ---------------------------------------------------------------------------
 * ENVIRONMENT (check all of these; each one silently breaks determinism)
 *
 *   echo never > /sys/kernel/mm/transparent_hugepage/enabled
 *   echo 0 > /proc/sys/kernel/numa_balancing     # NUMA hinting faults
 *   echo 0 > /proc/sys/vm/page-cluster           # swap readahead off
 *   echo 0 > /sys/kernel/mm/ksm/run              # KSM off
 *   cat /sys/kernel/mm/lru_gen/enabled           # note MGLRU state
 *   swapoff -a                                   # for the no-swap runs
 *   setarch -R                                   # ASLR off, stable addresses
 *   taskset -c <cpu>                             # no migration
 *
 * MADV_PAGEOUT on anonymous memory requires swap. Without swap, dirty anon
 * pages are unevictable and --evict is a no-op. Use zram or a swapfile with
 * zswap disabled for reproducible eviction.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1UL << PAGE_SHIFT)

/* Fixed base so the kernel side can filter on a hardcoded address range and
 * so addresses are identical across runs (with ASLR off). */
#define REGION_BASE ((void *)0x200000000000UL)

/* Only the first cacheline of each page is ever touched. The rest of the page
 * exists solely to make the stride one page. */
struct node {
	struct node *next;	/* offset  0 */
	uint64_t visits;	/* offset  8 */
	uint64_t index;		/* offset 16 */
	uint64_t pad[5];	/* pad to 64 bytes */
};

_Static_assert(sizeof(struct node) == 64, "node must be one cacheline");

/* Volatile sink: keeps read-only traversals from being optimized away. */
static volatile uint64_t sink;

/* ------------------------------------------------------------------------- */
/* Permutation: 4-round balanced Feistel over 2^m (m even) with cycle-walking
 * down to [0, n). Bijective by construction, seeded, and computed entirely in
 * registers -- no memory is touched to produce the traversal order.          */

static inline uint32_t round_fn(uint32_t x, uint32_t key)
{
	x ^= key;
	x *= 0x9e3779b1u;
	x ^= x >> 15;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	return x;
}

static inline uint64_t feistel(uint64_t x, unsigned m, uint64_t seed)
{
	unsigned h = m / 2;
	uint64_t mask = (m == 0) ? 0 : ((1UL << h) - 1);
	uint64_t l = (x >> h) & mask;
	uint64_t r = x & mask;
	int i;

	for (i = 0; i < 4; i++) {
		uint64_t f = round_fn((uint32_t)r,
				      (uint32_t)(seed + 0x9e37u * (unsigned)i)) & mask;
		uint64_t nl = r;
		uint64_t nr = l ^ f;
		l = nl;
		r = nr;
	}
	return (l << h) | r;
}

struct perm {
	unsigned m;		/* even, 2^m >= n */
	uint64_t n;
	uint64_t seed;
};

static void perm_init(struct perm *p, uint64_t n, uint64_t seed)
{
	unsigned m = 0;

	while ((1UL << m) < n)
		m++;
	if (m & 1)
		m++;		/* balanced Feistel needs an even width */
	if (m < 2)
		m = 2;
	p->m = m;
	p->n = n;
	p->seed = seed ? seed : 0x243f6a8885a308d3UL;
}

static inline uint64_t perm_at(const struct perm *p, uint64_t i)
{
	uint64_t v = feistel(i, p->m, p->seed);

	while (v >= p->n)	/* cycle-walking preserves bijectivity */
		v = feistel(v, p->m, p->seed);
	return v;
}

/* Identity, for the sequential calibration mode. */
static inline uint64_t perm_seq(const struct perm *p, uint64_t i)
{
	(void)p;
	return i;
}

/* ------------------------------------------------------------------------- */

static void phase(const char *name)
{
	char buf[16];

	fflush(stdout);		/* never let stdio bleed into a measured phase */
	snprintf(buf, sizeof(buf), "ph:%s", name);
	if (prctl(PR_SET_NAME, buf, 0, 0, 0) != 0)
		perror("prctl");
}

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline struct node *node_at(void *base, uint64_t idx)
{
	return (struct node *)((char *)base + idx * PAGE_SIZE);
}

/* ------------------------------------------------------------------------- */

struct opts {
	unsigned log2n;
	uint64_t passes;
	uint64_t seed;
	int sequential;		/* identity order, for calibration */
	int readonly;		/* traversal does not dirty pages */
	double evict_frac;	/* 0.0 = no eviction phase */
	int fixed_addr;
};

static void usage(const char *argv0)
{
	fprintf(stderr,
"Usage: %s [options]\n"
"  --log2n N        log2 of node count, one 4KiB page each (default 19 = 2GiB)\n"
"  --passes K       traversal passes after populate (default 1)\n"
"  --seed S         permutation seed (default fixed)\n"
"  --sequential     traverse in address order instead of permuted\n"
"  --readonly       traversal only reads; do not dirty pages\n"
"  --evict F        madvise(MADV_PAGEOUT) the first fraction F of pages,\n"
"                   then run one refault pass (default 0 = skip)\n"
"  --no-fixed-addr  let the kernel choose the mapping address\n", argv0);
	exit(2);
}

static void parse_opts(int argc, char **argv, struct opts *o)
{
	int i;

	o->log2n = 19;
	o->passes = 1;
	o->seed = 0;
	o->sequential = 0;
	o->readonly = 0;
	o->evict_frac = 0.0;
	o->fixed_addr = 1;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int has_val = (i + 1 < argc);

		if (!strcmp(a, "--log2n") && has_val)
			o->log2n = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(a, "--passes") && has_val)
			o->passes = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(a, "--seed") && has_val)
			o->seed = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(a, "--evict") && has_val)
			o->evict_frac = strtod(argv[++i], NULL);
		else if (!strcmp(a, "--sequential"))
			o->sequential = 1;
		else if (!strcmp(a, "--readonly"))
			o->readonly = 1;
		else if (!strcmp(a, "--no-fixed-addr"))
			o->fixed_addr = 0;
		else
			usage(argv[0]);
	}
	if (o->log2n < 1 || o->log2n > 40)
		usage(argv[0]);
	if (o->evict_frac < 0.0 || o->evict_frac > 1.0)
		usage(argv[0]);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	struct opts o;
	struct perm perm;
	uint64_t n, bytes, i, k, evict_pages;
	void *base;
	struct node *cur;
	double t0, t1;

	parse_opts(argc, argv, &o);

	n = 1UL << o.log2n;
	bytes = n * PAGE_SIZE;
	perm_init(&perm, n, o.seed);
	evict_pages = (uint64_t)(o.evict_frac * (double)n);

	/* Warm stdio before any measured phase so its buffers are already
	 * faulted in and cannot pollute later counts. */
	printf("# pagewalk: n=%lu pages, %.3f GiB, passes=%lu, order=%s, mode=%s\n",
	       (unsigned long)n, (double)bytes / (1024.0 * 1024.0 * 1024.0),
	       (unsigned long)o.passes,
	       o.sequential ? "sequential" : "permuted",
	       o.readonly ? "read-only" : "read-write");
	fflush(stdout);

	base = mmap(o.fixed_addr ? REGION_BASE : NULL, bytes,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
		    (o.fixed_addr ? MAP_FIXED_NOREPLACE : 0),
		    -1, 0);
	if (base == MAP_FAILED) {
		fprintf(stderr, "mmap(%.3f GiB) failed: %s\n",
			(double)bytes / (1024.0 * 1024.0 * 1024.0),
			strerror(errno));
		return 1;
	}

	/* Belt and braces even if THP is set to never system-wide. */
	if (madvise(base, bytes, MADV_NOHUGEPAGE) != 0)
		fprintf(stderr, "# warning: MADV_NOHUGEPAGE failed: %s\n",
			strerror(errno));

	printf("# region: [0x%lx, 0x%lx)  <-- filter kernel counters on this\n",
	       (unsigned long)base, (unsigned long)base + bytes);
	printf("# expected: populate=%lu faults/%lu new pages/%lu accesses\n",
	       (unsigned long)n, (unsigned long)n, (unsigned long)n);
	printf("# expected: traverse=%lu accesses total, 0 faults, 0 new pages\n",
	       (unsigned long)(n * o.passes));
	if (evict_pages)
		printf("# expected: evict=%lu pages out, refault=%lu accesses "
		       "with %lu major faults\n",
		       (unsigned long)evict_pages, (unsigned long)n,
		       (unsigned long)evict_pages);
	fflush(stdout);

	/* --- ph:idle ---------------------------------------------------- */
	phase("idle");
	sleep(1);

	/* --- ph:populate: exactly one store per page, in permuted order,
	 *     wiring a Hamiltonian cycle through all n nodes. ------------- */
	phase("populate");
	t0 = now_sec();
	for (i = 0; i < n; i++) {
		uint64_t a = o.sequential ? perm_seq(&perm, i) : perm_at(&perm, i);
		uint64_t b = o.sequential ? perm_seq(&perm, (i + 1) % n)
					  : perm_at(&perm, (i + 1) % n);
		struct node *na = node_at(base, a);

		na->next = node_at(base, b);
		na->index = a;
		na->visits = 0;
	}
	t1 = now_sec();
	phase("idle");
	printf("populate: %.3f s  (%lu pages)\n", t1 - t0, (unsigned long)n);
	fflush(stdout);

	/* --- ph:traverse: n page visits per pass, one cacheline each ----- */
	cur = node_at(base, o.sequential ? 0 : perm_at(&perm, 0));
	for (k = 0; k < o.passes; k++) {
		uint64_t acc = 0;

		phase("traverse");
		t0 = now_sec();
		if (o.readonly) {
			for (i = 0; i < n; i++) {
				acc += cur->index;
				cur = cur->next;
			}
		} else {
			for (i = 0; i < n; i++) {
				cur->visits++;
				acc += cur->index;
				cur = cur->next;
			}
		}
		t1 = now_sec();
		phase("idle");
		sink = acc;
		printf("traverse[%lu]: %.3f s  %lu visits  %.1f ns/visit\n",
		       (unsigned long)k, t1 - t0, (unsigned long)n,
		       (t1 - t0) * 1e9 / (double)n);
		fflush(stdout);
	}

	/* --- ph:evict + ph:refault: deterministic reclaim --------------- */
	if (evict_pages) {
		phase("evict");
		t0 = now_sec();
		if (madvise(base, evict_pages * PAGE_SIZE, MADV_PAGEOUT) != 0)
			fprintf(stderr, "# MADV_PAGEOUT failed: %s "
					"(is swap enabled?)\n", strerror(errno));
		t1 = now_sec();
		phase("idle");
		printf("evict: %.3f s  (%lu pages requested out)\n",
		       t1 - t0, (unsigned long)evict_pages);
		fflush(stdout);

		{
			uint64_t acc = 0;

			phase("refault");
			t0 = now_sec();
			for (i = 0; i < n; i++) {
				cur->visits++;
				acc += cur->index;
				cur = cur->next;
			}
			t1 = now_sec();
			phase("idle");
			sink = acc;
			printf("refault: %.3f s  %lu visits, expected %lu major "
			       "faults\n", t1 - t0, (unsigned long)n,
			       (unsigned long)evict_pages);
			fflush(stdout);
		}
	}

	/* Self-check: the cycle must have returned to its start, and every
	 * node must have been visited the same number of times. */
	{
		uint64_t expect = o.passes + (evict_pages ? 1 : 0);
		uint64_t bad = 0;

		if (!o.readonly) {
			for (i = 0; i < n; i++)
				if (node_at(base, i)->visits != expect)
					bad++;
		}
		printf("verify: cycle_closed=%s mismatched_nodes=%lu "
		       "expected_visits=%lu\n",
		       cur == node_at(base, o.sequential ? 0 : perm_at(&perm, 0))
			     ? "yes" : "NO",
		       (unsigned long)bad, (unsigned long)expect);
	}

	munmap(base, bytes);
	return 0;
}