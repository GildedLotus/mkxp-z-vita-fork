// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_fiber_arena.c — MRI fiber machine stacks inside the Ruby thread's own
 * registered stack. See vita/ruby/include/vita_fiber_arena.h
 * for the why and the picture.
 *
 * Linked into libruby-vita.a by vita/scripts/build-ruby-vita.sh and called from
 * cont.c and thread_pthread.c by the Ruby patches. Nothing here includes a
 * Ruby header, so it can be built on
 * the host with a fake thread-info provider.
 *
 * Deliberately not thread safe beyond what it needs to be: exactly one thread
 * may own the arena, every other thread is told so (VITA_FIBER_ARENA_FOREIGN)
 * and creates no fibers, and MRI only ever reaches this code under the GVL.
 */

#include "vita_fiber_arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

/* ------------------------------------------------------------------ */
/* Seams                                                              */
/* ------------------------------------------------------------------ */

#ifdef __vita__
static int
default_thread_info(int *thread_id, uintptr_t *stack_base, size_t *stack_size,
                    void *user)
{
	SceKernelThreadInfo info;
	int tid;
	int rc;

	(void)user;
	tid = sceKernelGetThreadId();
	if (tid < 0)
		return tid;

	/* sceKernelGetThreadInfo refuses a struct whose size field is not one
	 * the firmware knows; VitaSDK asserts sizeof == 0x80. A refusal here
	 * costs the whole fix, so the error code is returned rather than
	 * flattened, and it reaches the log. */
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	rc = sceKernelGetThreadInfo(tid, &info);
	if (rc < 0)
		return rc;
	if (info.stack == NULL || info.stackSize <= 0)
		return -1;

	*thread_id = tid;
	*stack_base = (uintptr_t)info.stack;
	*stack_size = (size_t)info.stackSize;
	return 0;
}
#else
static int
default_thread_info(int *thread_id, uintptr_t *stack_base, size_t *stack_size,
                    void *user)
{
	/* Host builds must supply a registered-stack substitute. */
	(void)thread_id;
	(void)stack_base;
	(void)stack_size;
	(void)user;
	return -1;
}
#endif

static void
default_log(const char *line, void *user)
{
	(void)user;
	/* vita_glue_boot has stderr unbuffered and pointed at
	 * ux0:/data/mkxp-z/logs/mkxp-z.log; the flush is for the host. */
	fprintf(stderr, "%s\n", line);
	fflush(stderr);
}

static vita_fiber_arena_thread_info_fn g_thread_info = default_thread_info;
static void *g_thread_info_user;
static vita_fiber_arena_log_fn g_log = default_log;
static void *g_log_user;

void
vita_fiber_arena_set_thread_info(vita_fiber_arena_thread_info_fn fn, void *user)
{
	g_thread_info = fn ? fn : default_thread_info;
	g_thread_info_user = fn ? user : NULL;
}

void
vita_fiber_arena_set_log(vita_fiber_arena_log_fn fn, void *user)
{
	g_log = fn ? fn : default_log;
	g_log_user = fn ? user : NULL;
}

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

/* Whatever the thread-info provider last said when it failed. On Vita that
 * is the sceKernelGetThreadId / sceKernelGetThreadInfo error code, which is
 * the only thing that would explain missing thread info on hardware. */
static int g_thread_info_error;

static struct {
	int active;           /* the arena has been handed out */
	int unavailable;      /* sticky failure, never use malloc stacks */
	char reason[192];
	int floor_logged;     /* the main-stack floor line has been written */
	int owner_thread;
	uintptr_t base;
	size_t size;
	uintptr_t main_stack_floor;
	size_t stride;
	size_t capacity;
	unsigned live;
} g_arena;

static int g_diagnostics;
static struct vita_fiber_slot_stats g_slots[VITA_FIBER_DIAGNOSTIC_SLOTS];

void
vita_fiber_arena_reset(void)
{
	memset(&g_arena, 0, sizeof(g_arena));
	memset(g_slots, 0, sizeof(g_slots));
	g_diagnostics = 0;
	g_thread_info = default_thread_info;
	g_thread_info_user = NULL;
	g_log = default_log;
	g_log_user = NULL;
}

static void
logf_line(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	line[sizeof(line) - 1] = '\0';
	g_log(line, g_log_user);
}

/* ------------------------------------------------------------------ */
/* Geometry                                                           */
/* ------------------------------------------------------------------ */

static uintptr_t
align_up(uintptr_t value, uintptr_t align)
{
	uintptr_t rounded = value + (align - 1u);

	if (rounded < value) /* wrap: keep the caller's value, it will fail the
	                      * range checks below */
		return value;
	return rounded & ~(align - 1u);
}

static size_t
align_down_size(size_t value, size_t align)
{
	return value & ~(align - 1u);
}

size_t
vita_fiber_arena_capacity(size_t arena_size, size_t stride)
{
	if (stride == 0)
		return 0;
	return arena_size / stride;
}

int
vita_fiber_arena_plan(struct vita_fiber_arena_plan *out)
{
	struct vita_fiber_arena_plan plan;
	uintptr_t stack_end;
	uintptr_t arena_base;
	size_t usable;
	size_t arena_size;
	const size_t overhead = (size_t)VITA_FIBER_ARENA_RESERVE_BYTES +
	                        (size_t)VITA_FIBER_ARENA_GUARD_BYTES;

	plan.ok = 0;
	plan.thread_id = 0;
	plan.stack_base = 0;
	plan.stack_size = 0;
	plan.arena_base = 0;
	plan.arena_size = 0;
	plan.main_stack_floor = 0;
	plan.reason = "no thread info";

	g_thread_info_error = g_thread_info(&plan.thread_id, &plan.stack_base,
	                                    &plan.stack_size, g_thread_info_user);
	if (g_thread_info_error != 0) {
		plan.thread_id = 0;
		plan.stack_base = 0;
		plan.stack_size = 0;
		if (out)
			*out = plan;
		return 0;
	}

	stack_end = plan.stack_base + (uintptr_t)plan.stack_size;
	if (plan.stack_size == 0 || stack_end < plan.stack_base) {
		plan.reason = "nonsense thread stack";
		if (out)
			*out = plan;
		return 0;
	}

	arena_base = align_up(plan.stack_base, VITA_FIBER_ARENA_ALIGN_BYTES);
	if (arena_base >= stack_end) {
		plan.reason = "thread stack shorter than one page";
		if (out)
			*out = plan;
		return 0;
	}
	usable = (size_t)(stack_end - arena_base);

	/* Leave the thread the stack it was built with, plus an empty guard,
	 * before anything is taken for fibers. */
	if (usable <= overhead) {
		plan.reason = "thread stack does not clear reserve+guard";
		if (out)
			*out = plan;
		return 0;
	}
	arena_size = usable - overhead;
	if (arena_size > (size_t)VITA_FIBER_ARENA_MAX_BYTES)
		arena_size = (size_t)VITA_FIBER_ARENA_MAX_BYTES;
	arena_size = align_down_size(arena_size, VITA_FIBER_ARENA_ALIGN_BYTES);
	if (arena_size < (size_t)VITA_FIBER_ARENA_MIN_BYTES) {
		plan.reason = "arena would be too small to be worth it";
		if (out)
			*out = plan;
		return 0;
	}

	plan.ok = 1;
	plan.arena_base = arena_base;
	plan.arena_size = arena_size;
	plan.main_stack_floor = arena_base + (uintptr_t)arena_size +
	                        (uintptr_t)VITA_FIBER_ARENA_GUARD_BYTES;
	plan.reason = "ok";
	if (out)
		*out = plan;
	return 1;
}

uintptr_t
vita_fiber_arena_main_stack_floor(void)
{
	struct vita_fiber_arena_plan plan;
	uintptr_t floor = 0;
	int log_now;

	if (vita_fiber_arena_plan(&plan)) {
		floor = plan.main_stack_floor;
	}
	else if (plan.stack_size > (size_t)VITA_FIBER_ARENA_GUARD_BYTES) {
		/* No arena, but the floor is still the bottom of the thread's
		 * own stack plus the guard. Reporting it is what turns a
		 * runaway main-context recursion into SystemStackError instead
		 * of a silent walk off the end of the stack, which is what
		 * this port does today. */
		floor = plan.stack_base + (uintptr_t)VITA_FIBER_ARENA_GUARD_BYTES;
	}

	log_now = !g_arena.floor_logged;
	g_arena.floor_logged = 1;
	if (log_now) {
		if (floor == 0)
			logf_line("vita-fiber-arena: no thread info (%s, "
			          "provider 0x%08x); Ruby main-stack overflow "
			          "detection stays off",
			          plan.reason, (unsigned)g_thread_info_error);
		else
			logf_line("vita-fiber-arena: main stack floor 0x%08x "
			          "(thread stack 0x%08x+%u, arena %u bytes, %s)",
			          (unsigned)floor, (unsigned)plan.stack_base,
			          (unsigned)plan.stack_size,
			          (unsigned)plan.arena_size, plan.reason);
	}
	return floor;
}

/* ------------------------------------------------------------------ */
/* The allocation                                                     */
/* ------------------------------------------------------------------ */

static int
current_thread_id(int *thread_id)
{
	uintptr_t base;
	size_t size;

	return g_thread_info(thread_id, &base, &size, g_thread_info_user);
}

int
vita_fiber_arena_acquire(void **base, size_t *count, size_t stride)
{
	struct vita_fiber_arena_plan plan;
	size_t capacity;
	int tid = 0;

	if (base)
		*base = NULL;
	if (base == NULL || count == NULL || stride == 0)
		return VITA_FIBER_ARENA_UNAVAILABLE;

	if (g_arena.active) {
		/* One allocation, never freed, never extended: a second one
		 * could only come from malloc, and a malloc'd fiber stack is
		 * the bug this file exists to remove. */
		if (current_thread_id(&tid) == 0 && tid == g_arena.owner_thread)
			return VITA_FIBER_ARENA_EXHAUSTED;
		return VITA_FIBER_ARENA_FOREIGN;
	}
	if (g_arena.unavailable)
		return VITA_FIBER_ARENA_UNAVAILABLE;

	if (!vita_fiber_arena_plan(&plan)) {
		g_arena.unavailable = 1;
		snprintf(g_arena.reason, sizeof(g_arena.reason),
		          "%s; thread stack %u bytes, need at least %u; provider 0x%08x",
		          plan.reason, (unsigned)plan.stack_size,
		          (unsigned)((unsigned)VITA_FIBER_ARENA_RESERVE_BYTES +
		                     (unsigned)VITA_FIBER_ARENA_GUARD_BYTES +
		                     (unsigned)VITA_FIBER_ARENA_MIN_BYTES),
		          (unsigned)g_thread_info_error);
		logf_line("vita-fiber-arena: unavailable: %s", g_arena.reason);
		return VITA_FIBER_ARENA_UNAVAILABLE;
	}

	capacity = vita_fiber_arena_capacity(plan.arena_size, stride);
	if (capacity == 0) {
		g_arena.unavailable = 1;
		snprintf(g_arena.reason, sizeof(g_arena.reason),
		          "arena %u bytes cannot hold one %u-byte fiber stack",
		          (unsigned)plan.arena_size, (unsigned)stride);
		logf_line("vita-fiber-arena: unavailable: %s", g_arena.reason);
		return VITA_FIBER_ARENA_UNAVAILABLE;
	}

	g_arena.active = 1;
	g_arena.owner_thread = plan.thread_id;
	g_arena.base = plan.arena_base;
	g_arena.size = plan.arena_size;
	g_arena.main_stack_floor = plan.main_stack_floor;
	g_arena.stride = stride;
	g_arena.capacity = capacity;

	logf_line("vita-fiber-arena: thread 0x%08x stack 0x%08x+%u; arena "
	          "0x%08x+%u; stride %u; capacity %u fibers; main stack floor "
	          "0x%08x",
	          (unsigned)plan.thread_id, (unsigned)plan.stack_base,
	          (unsigned)plan.stack_size, (unsigned)plan.arena_base,
	          (unsigned)plan.arena_size, (unsigned)stride,
	          (unsigned)capacity, (unsigned)plan.main_stack_floor);

	*base = (void *)plan.arena_base;
	*count = capacity;
	return VITA_FIBER_ARENA_OK;
}

int
vita_fiber_arena_active(void)
{
	return g_arena.active;
}

int
vita_fiber_arena_live(unsigned *live, unsigned *capacity)
{
	if (live) *live = g_arena.active ? g_arena.live : 0;
	if (capacity) *capacity = g_arena.active ? (unsigned)g_arena.capacity : 0;
	return !g_arena.active;
}

int
vita_fiber_arena_owns_current_thread(void)
{
	int tid = 0;

	if (!g_arena.active)
		return 0;
	if (current_thread_id(&tid) != 0)
		return 0;
	return tid == g_arena.owner_thread;
}

int
vita_fiber_arena_contains(const void *pointer)
{
	uintptr_t address = (uintptr_t)pointer;

	if (!g_arena.active)
		return 0;
	return address >= g_arena.base &&
	       address < g_arena.base + (uintptr_t)g_arena.size;
}

const char *
vita_fiber_arena_state(void)
{
	if (g_arena.active)
		return "arena";
	if (g_arena.unavailable)
		return "unavailable";
	return "unused";
}

const char *
vita_fiber_arena_failure_reason(void)
{
	return g_arena.unavailable ? g_arena.reason : "arena not initialized";
}

/* The unused page below each C stack already belongs to its 328 KiB slot.
 * Its top word detects a crossed boundary, without changing pool geometry
 * or protecting memory. A deep native call can corrupt a neighbour first. */
#define SLOT_CANARY UINT32_C(0xf1b3ca7e)
#define STACK_PAINT 0xa5

static unsigned char *
slot_base(size_t index)
{
	return (unsigned char *)(g_arena.base + index * g_arena.stride +
	                         VITA_FIBER_ARENA_ALIGN_BYTES);
}

static uint32_t *
slot_canary(size_t index)
{
	return (uint32_t *)(slot_base(index) - sizeof(uint32_t));
}

static void
sample_slot(size_t index)
{
	struct vita_fiber_slot_stats *slot = &g_slots[index];
	size_t untouched = 0, used;
	const unsigned char *base = slot_base(index);

	if (*slot_canary(index) != SLOT_CANARY && slot->canary_ok) {
		slot->canary_ok = 0;
		logf_line("vita-fiber-arena: CORRUPTION slot %u boundary canary "
		          "(detection only, not protection)", (unsigned)index);
	}
	if (!slot->active)
		return;
	while (untouched < slot->machine_bytes && base[untouched] == STACK_PAINT)
		untouched++;
	used = slot->machine_bytes - untouched;
	if (used > slot->high_water_bytes)
		slot->high_water_bytes = used;
}

int
vita_fiber_arena_diagnostics_enable(void)
{
	size_t i;
	if (!vita_fiber_arena_owns_current_thread() ||
	    g_arena.capacity > VITA_FIBER_DIAGNOSTIC_SLOTS ||
	    g_arena.stride % VITA_FIBER_ARENA_ALIGN_BYTES != 0 ||
	    g_arena.stride <= VITA_FIBER_ARENA_ALIGN_BYTES)
		return 0;
	for (i = 0; i < g_arena.capacity; ++i) {
		if (g_diagnostics) {
			sample_slot(i);
			if (g_slots[i].active || !g_slots[i].canary_ok)
				return 0;
		}
	}
	memset(g_slots, 0, sizeof(g_slots));
	for (i = 0; i < g_arena.capacity; ++i) {
		*slot_canary(i) = SLOT_CANARY;
		g_slots[i].canary_ok = 1;
	}
	g_diagnostics = 1;
	return 1;
}

static size_t
slot_index(void *base)
{
	uintptr_t address = (uintptr_t)base;
	uintptr_t first = g_arena.base + VITA_FIBER_ARENA_ALIGN_BYTES;
	if (address < first || (address - first) % g_arena.stride != 0)
		return g_arena.capacity;
	return (size_t)((address - first) / g_arena.stride);
}

void
vita_fiber_arena_slot_begin(void *base, size_t machine_bytes)
{
	size_t index;
	/* MRI pairs these hooks on the arena owner, even without diagnostics. */
	if (g_arena.active && g_arena.live < g_arena.capacity)
		++g_arena.live;
	if (!g_diagnostics)
		return;
	index = slot_index(base);
	if (index >= g_arena.capacity ||
	    machine_bytes > g_arena.stride - VITA_FIBER_ARENA_ALIGN_BYTES)
		return;
	sample_slot(index);
	g_slots[index].machine_bytes = machine_bytes;
	g_slots[index].uses++;
	g_slots[index].active = 1;
	memset(base, STACK_PAINT, machine_bytes);
}

void
vita_fiber_arena_slot_end(void *base)
{
	size_t index;
	if (g_arena.live)
		--g_arena.live;
	if (!g_diagnostics)
		return;
	index = slot_index(base);
	if (index >= g_arena.capacity)
		return;
	sample_slot(index);
	g_slots[index].active = 0;
}

size_t
vita_fiber_arena_snapshot(struct vita_fiber_slot_stats *out, size_t count)
{
	size_t i;
	if (!g_diagnostics || !vita_fiber_arena_owns_current_thread() ||
	    !out || count < g_arena.capacity)
		return 0;
	for (i = 0; i < g_arena.capacity; ++i) {
		sample_slot(i);
		out[i] = g_slots[i];
	}
	return g_arena.capacity;
}
