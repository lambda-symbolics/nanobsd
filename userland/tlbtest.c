/*
 * tlbtest: check that a CPU which idled with our pmap loaded (TLBSTATE_LAZY)
 * never uses a stale translation after another CPU changed the mapping.
 *
 * A reader thread pinned to CPU R touches address A, then blocks so CPU R
 * idles with this process's pmap still loaded.  A writer pinned to CPU W then
 * remaps A to a different page (both pages stay allocated in memfds, so a
 * stale TLB entry keeps pointing at readable memory with the old contents)
 * or revokes access, and wakes the reader.  The reader must see the new page
 * or take the fault.  Any other outcome is a stale translation.
 *
 * usage: tlbtest ITERATIONS [READER_CPU ...]   (writer runs on CPU 0)
 * TLBTEST_SPIN=N busy-waits N loop turns before each change, so the reader's
 * CPU has certainly gone idle with the pmap lazily loaded.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sched.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PG 4096

static int fd_x, fd_y;
static volatile unsigned *A;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int turn;		/* 0: writer's turn, 1: reader's turn */
static int expect;		/* 'X', 'Y' or 'F' (must fault) */
static volatile int done;
static unsigned long stale, faults_ok, reads_ok;
static sigjmp_buf jb;
static volatile sig_atomic_t in_probe;

static void
pin(int cpu)
{
	cpuset_t *cs = cpuset_create();
	cpuset_zero(cs);
	cpuset_set(cpu, cs);
	if (pthread_setaffinity_np(pthread_self(), cpuset_size(cs), cs) != 0) {
		perror("pthread_setaffinity_np");
		exit(2);
	}
	cpuset_destroy(cs);
}

static void
onsegv(int sig)
{
	(void)sig;
	if (in_probe)
		siglongjmp(jb, 1);
	_exit(3);
}

static void
wait_turn(int want)
{
	pthread_mutex_lock(&mu);
	while (turn != want && !done)
		pthread_cond_wait(&cv, &mu);
	pthread_mutex_unlock(&mu);
}

static void
give_turn(int to)
{
	pthread_mutex_lock(&mu);
	turn = to;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

static void *
reader(void *arg)
{
	pin((int)(long)arg);
	for (;;) {
		wait_turn(1);
		if (done)
			break;
		in_probe = 1;
		if (sigsetjmp(jb, 1) == 0) {
			unsigned v = *A;
			in_probe = 0;
			if (expect == 'F' || v != (unsigned)expect * 0x01010101u) {
				stale++;
				if (stale < 10)
					fprintf(stderr, "STALE: expected %c, read 0x%08x\n",
					    expect, v);
			} else
				reads_ok++;
		} else {
			in_probe = 0;
			if (expect == 'F')
				faults_ok++;
			else {
				stale++;
				fprintf(stderr, "unexpected fault, expected %c\n", expect);
			}
		}
		give_turn(0);	/* block again: this CPU goes idle, pmap lazy */
	}
	return NULL;
}

static void
map_at(int fd)
{
	void *p = mmap((void *)A, PG, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
	if (p != (void *)A) {
		perror("mmap");
		exit(2);
	}
}

static int
mkpage(int c)
{
	char name[] = "/tmp/tlbtest.XXXXXX";
	int fd = mkstemp(name);
	unsigned buf[PG / 4];
	unlink(name);
	for (int i = 0; i < PG / 4; i++)
		buf[i] = (unsigned)c * 0x01010101u;
	if (fd < 0 || write(fd, buf, PG) != PG) {
		perror("page");
		exit(2);
	}
	return fd;
}

int
main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 100000;
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	int first = 2, last = argc;
	struct sigaction sa;
	long spin = getenv("TLBTEST_SPIN") ? atol(getenv("TLBTEST_SPIN")) : 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onsegv;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	fd_x = mkpage('X');
	fd_y = mkpage('Y');
	A = mmap(NULL, PG, PROT_READ, MAP_SHARED, fd_x, 0);
	pin(0);

	if (argc <= 2) {	/* default: every CPU but 0 as the reader, in turn */
		first = 0;
		last = ncpu - 1;
	}
	for (int k = first; k < last; k++) {
		int rcpu = argc <= 2 ? k + 1 : atoi(argv[k]);
		pthread_t t;
		unsigned long s0 = stale;

		done = 0;
		turn = 0;
		pthread_create(&t, NULL, reader, (void *)(long)rcpu);
		for (long i = 0; i < iters; i++) {
			wait_turn(0);
			/* optionally let the reader's CPU reach idle first */
			for (volatile long w = 0; w < spin; w++)
				;
			switch (i % 3) {
			case 0: map_at(fd_x); expect = 'X'; break;
			case 1: map_at(fd_y); expect = 'Y'; break;
			case 2: mprotect((void *)A, PG, PROT_NONE); expect = 'F'; break;
			}
			/* let the reader's CPU settle into idle before the change is seen */
			give_turn(1);
		}
		wait_turn(0);
		done = 1;
		give_turn(1);
		pthread_join(t, NULL);
		printf("reader cpu %d: %ld iterations, stale %lu\n", rcpu, iters,
		    stale - s0);
	}
	printf("total: reads ok %lu, faults ok %lu, STALE %lu\n", reads_ok,
	    faults_ok, stale);
	return stale != 0;
}
