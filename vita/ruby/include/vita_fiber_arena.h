// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_fiber_arena.h — where MRI's Fiber machine stacks live on PS Vita.
 *
 * On Vita a thread registers a stack with the kernel
 * (sceKernelCreateThread; SceKernelThreadInfo.stack / .stackSize report it
 * back). A thread whose SP is outside that range when it enters a syscall is
 * stopped with 0x10006 — deterministically, at the first syscall, with no
 * exception the runtime can catch. Ruby's fiber pool originally took its stacks
 * from malloc on this port, which puts every Fiber's SP on
 * the libc heap, so the first File/Bitmap#draw_text/sleep inside a Fiber kills
 * the thread. RGSS3 runs every event interpreter and the message window inside
 * a Fiber.
 *
 * The fix is to carve the fiber pool's backing memory out of the low end of
 * the registered stack of the thread that runs Ruby, so a fiber SP is still
 * "inside the thread's stack" as far as the kernel is concerned:
 *
 *   stack_base                                         stack_base+stack_size
 *   |<-------- arena (fiber stacks) -------->|<-guard->|<--- thread's own --->|
 *   |                                        |         |      stack (SP)      |
 *   arena_base                        arena top   main_stack_floor       initial SP
 *
 * Ruby's idea of the main machine stack must stop at main_stack_floor so a
 * runaway main-context recursion becomes SystemStackError instead of walking
 * down into the arena and corrupting a suspended fiber.
 *
 * This header is deliberately free of Ruby and of psp2 headers: the allocator
 * in vita/ruby/vita_fiber_arena.c can be built on the host
 * through the thread-info seam below.
 */
#ifndef VITA_FIBER_ARENA_H
#define VITA_FIBER_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- geometry ----------------------------------------------------------
 *
 * The thread that runs Ruby asks for RESERVE + GUARD + MAX bytes of stack
 * (for the player). The allocator hands the top RESERVE bytes back to the
 * thread, keeps GUARD bytes empty between the two, and uses the rest — capped
 * at MAX — as the arena.
 *
 * RESERVE is the 8 MiB the rgss thread has always had,
 * so nothing about the thread's own stack
 * changes.
 *
 * MAX is 16 MiB because MRI 3.1's 32-bit defaults make one fiber slot
 * (RUBY_VM_FIBER_MACHINE_STACK_SIZE 256 KiB + RUBY_VM_FIBER_VM_STACK_SIZE
 * 64 KiB, rounded up to a page by fiber_pool_initialize, plus one page of
 * would-be guard) 335,872 bytes, and 16 MiB / 335,872 = 49 live fibers. The capacity
 * diagnostic holds all 49 and verifies that the 50th is refused.
 */
#define VITA_FIBER_ARENA_RESERVE_BYTES (8u * 1024u * 1024u)
#define VITA_FIBER_ARENA_MAX_BYTES     (16u * 1024u * 1024u)
#define VITA_FIBER_ARENA_MIN_BYTES     (1u * 1024u * 1024u)
#define VITA_FIBER_ARENA_GUARD_BYTES   (128u * 1024u)
#define VITA_FIBER_ARENA_ALIGN_BYTES   4096u

/* What a thread that wants an arena must ask sceKernelCreateThread for. */
#define VITA_FIBER_ARENA_THREAD_STACK_BYTES \
	(VITA_FIBER_ARENA_RESERVE_BYTES + VITA_FIBER_ARENA_GUARD_BYTES + \
	 VITA_FIBER_ARENA_MAX_BYTES)

/* ---- results ----------------------------------------------------------- */

enum {
	/* *base and *count are set; this memory is inside the thread stack. */
	VITA_FIBER_ARENA_OK = 0,
	/* No safe arena. Keep initialization alive, then raise FiberError at
	 * first stack acquisition with vita_fiber_arena_failure_reason(). */
	VITA_FIBER_ARENA_UNAVAILABLE = 1,
	/* The arena exists and is already handed out: the caller must fail
	 * cleanly (FiberError), never quietly malloc a stack the kernel will
	 * refuse to make syscalls on. */
	VITA_FIBER_ARENA_EXHAUSTED = 2,
	/* Another thread owns the arena. Same rule. */
	VITA_FIBER_ARENA_FOREIGN = 3
};

struct vita_fiber_arena_plan {
	int ok;                     /* 1: an arena fits on this thread */
	int thread_id;              /* the thread the plan was computed for */
	uintptr_t stack_base;       /* registered stack, lowest address */
	size_t stack_size;
	uintptr_t arena_base;
	size_t arena_size;          /* 0 when !ok */
	uintptr_t main_stack_floor; /* lowest address Ruby's main stack may use */
	const char *reason;         /* "ok", or why not */
};

/* ---- seams ------------------------------------------------------------- */

/* Fills the registered stack of the CALLING thread. 0 on success.
 * Vita builds default to sceKernelGetThreadInfo(sceKernelGetThreadId());
 * host builds install a fake. */
typedef int (*vita_fiber_arena_thread_info_fn)(int *thread_id,
                                               uintptr_t *stack_base,
                                               size_t *stack_size,
                                               void *user);

/* One line, no trailing newline. Defaults to stderr, which vita_glue_boot has
 * already redirected (unbuffered, and synced when log-sync.enabled exists) to
 * ux0:/data/mkxp-z/logs/mkxp-z.log. */
typedef void (*vita_fiber_arena_log_fn)(const char *line, void *user);

void vita_fiber_arena_set_thread_info(vita_fiber_arena_thread_info_fn fn, void *user);
void vita_fiber_arena_set_log(vita_fiber_arena_log_fn fn, void *user);

/* Forgets the arena and both seams. Tests only — there is no way to give a
 * thread's stack back once fibers are living in it. */
void vita_fiber_arena_reset(void);

/* ---- the allocator ----------------------------------------------------- */

/* Geometry for the calling thread. Pure function of the thread info: calling
 * it twice on one thread gives the same answer, and it never takes ownership.
 * Returns plan->ok. */
int vita_fiber_arena_plan(struct vita_fiber_arena_plan *out);

/* How many `stride`-byte fiber slots fit in `arena_size`. */
size_t vita_fiber_arena_capacity(size_t arena_size, size_t stride);

/* The one and only allocation. On VITA_FIBER_ARENA_OK, *base is the arena and
 * *count is how many slots fit (which may be more than the caller asked for —
 * MRI's fiber_pool_expand uses the returned count). Every later call returns
 * EXHAUSTED or FOREIGN. Planning failure is sticky UNAVAILABLE, with a
 * retained reason and no allocation. */
int vita_fiber_arena_acquire(void **base, size_t *count, size_t stride);

int vita_fiber_arena_active(void);
/* Owner-thread read, independent of stack diagnostics; no thread query.
 * Returns 0 with live/capacity counts, 1 with zeroes before arena acquisition
 * or after setup failure. Output pointers may be NULL. */
int vita_fiber_arena_live(unsigned *live, unsigned *capacity);
int vita_fiber_arena_owns_current_thread(void);
int vita_fiber_arena_contains(const void *pointer);

/* Lowest address Ruby's main machine stack may touch on the calling thread,
 * or 0 when that is unknown (no thread info). Safe to call before
 * vita_fiber_arena_acquire: it answers with the same geometry the arena will
 * be carved from. */
uintptr_t vita_fiber_arena_main_stack_floor(void);

/* "unused", "arena", or "unavailable". For logs and tests. */
const char *vita_fiber_arena_state(void);
const char *vita_fiber_arena_failure_reason(void);

/* Opt-in diagnostics for the downward-growing Vita stacks. Canaries detect
 * corruption; they do not prevent it. Enable/reset only with no live slots.
 * Painting measures written bytes, a lower bound on actual stack depth.
 * Fixed storage covers the shipped 49 slots; larger custom pools are refused. */
#define VITA_FIBER_DIAGNOSTIC_SLOTS 49u
struct vita_fiber_slot_stats {
	size_t machine_bytes;
	size_t high_water_bytes;
	size_t uses;
	int active;
	int canary_ok;
};
int vita_fiber_arena_diagnostics_enable(void);
void vita_fiber_arena_slot_begin(void *base, size_t machine_bytes);
void vita_fiber_arena_slot_end(void *base);
size_t vita_fiber_arena_snapshot(struct vita_fiber_slot_stats *out, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* VITA_FIBER_ARENA_H */
