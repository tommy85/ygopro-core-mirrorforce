/* Implementation notes live in mfsnap.h.  The load-bearing constraints:
 *
 * - The registry, the arena descriptors and snapshot buffers live on the
 *   SYSTEM heap (raw malloc), never inside any arena: a rollback overwrites
 *   the arena wholesale and must not be able to eat its own bookkeeping.
 * - operator new must stay 16-byte aligned after the owner header, and the
 *   over-aligned C++17 forms are honoured through mspace_memalign.
 * - Everything here is single-threaded by ocgcore's own contract; the
 *   thread_local current pointer is correctness against the *host* having
 *   other threads, not a concession to using them here.
 */
#include "mfsnap.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>

#include <sys/mman.h>

/* mspace API from mfsnap_dlmalloc.c (compiled with ONLY_MSPACES=1,
 * HAVE_MMAP=0, HAVE_MORECORE=0: the region given at creation is the whole
 * heap, and exhaustion fails the allocation instead of escaping). */
extern "C" {
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
size_t destroy_mspace(mspace msp);
void* mspace_malloc(mspace msp, size_t bytes);
void  mspace_free(mspace msp, void* mem);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_used_extent(mspace msp);
}

namespace {

struct MfArena {
	void* base;
	size_t reserve;
	mspace ms;
};

/* header stamped in front of every block operator new hands out */
constexpr size_t kHeader = 16;

thread_local MfArena* g_current = nullptr;

/* raw-malloc'd containers only: a std::unordered_map would allocate its
 * nodes through our own operator new, which is exactly the tangle the
 * system-heap rule exists to avoid.  A tiny open-addressed table is enough
 * -- a process holds a handful of duels.
 *
 * Concurrency contract (the MirrorForce documentation
 * §3.8 and §7.1).  A duel is still single-threaded; what changes with the
 * batched environment is that N duels run on N threads, so `get` -- which
 * every ocgapi entry point calls through MfArenaGuard -- runs concurrently
 * with the `put` / `drop` of some *other* duel being created or destroyed.
 * The rule this table now keeps is single-writer, many-readers:
 *
 *   * writers (`put` / `drop`) are serialized by their only callers,
 *     create_duel / create_duel_v2 / end_duel, under the exclusive side of
 *     ocgapi.cpp's duel_set_mutex (before that lock the environment's own
 *     create/end mutex did it); nothing else writes.  Plain relaxed stores
 *     plus a release fence are therefore enough on the write side -- no CAS.
 *   * readers (`get`) are lock-free.  Acquire loads pair with the release
 *     stores, so a reader that sees a key sees the arena pointer that was
 *     published with it, on any thread.
 *   * a slot's key goes 0 -> k -> kTombstone -> k2 and never back to 0, so a
 *     reader that reached slot i while probing for a key inserted earlier can
 *     never observe 0 there and stop short.
 *
 * The tombstone is a distinct key value rather than the old "keep the dead
 * key" trick.  Keeping it made every slot permanently occupied by the first
 * 256 *distinct* duel pointers the process ever saw; because each duel is
 * constructed inside its own freshly mmap'd arena, those pointers do not
 * repeat once several duels are live at once, and `put` then silently did
 * nothing -- MfArenaGuard would hand out a null arena and the duel would
 * allocate on the system heap, outside the snapshot's reach.  Measured on the
 * unpatched core: with 260 duels alive at once the first failure is at
 * creation 256, and after one destroy/recreate round it moves to 255 and
 * keeps drifting down.  kSlots is no cap on duels per process lifetime.
 *
 * Live duels are capped at kLiveCap, a quarter of the slots, so probe chains stay short; a creation beyond the
 * cap is refused (create_duel returns 0) rather than run outside an arena.  A creation reserves its place under
 * duel_set_mutex before the duel is built (reserve), and registering it consumes the reservation, so concurrent
 * creations cannot pass the cap.
 */
struct Registry {
	static constexpr size_t kSlots = 16384;
	static constexpr size_t kLiveCap = 4096;
	static constexpr intptr_t kEmpty = 0;
	static constexpr intptr_t kTombstone = -1;
	std::atomic<intptr_t> keys[kSlots] = {};
	std::atomic<MfArena*> values[kSlots] = {};
	size_t live = 0;      /* registered duels (writers only, under duel_set_mutex) */
	size_t reserved = 0;  /* creations between reserve and put */

	static size_t slot_of(intptr_t k) {
		return (static_cast<size_t>(k) >> 4) % kSlots;
	}
	bool reserve() {
		if (live + reserved >= kLiveCap)
			return false;
		++reserved;
		return true;
	}
	void cancel() {
		if (reserved)
			--reserved;
	}
	bool put(intptr_t k, MfArena* v) {
		/* an existing entry for k wins over a free slot, so a pointer the
		 * allocator handed back out is re-registered in place */
		if (reserved)
			--reserved;
		size_t i = slot_of(k);
		size_t free_slot = kSlots;
		for (size_t n = 0; n < kSlots; ++n, i = (i + 1) % kSlots) {
			const intptr_t key = keys[i].load(std::memory_order_relaxed);
			if (key == k) {
				values[i].store(v, std::memory_order_release);
				return true;
			}
			if (key == kTombstone && free_slot == kSlots)
				free_slot = i;
			if (key == kEmpty) {
				if (free_slot == kSlots)
					free_slot = i;
				break;
			}
		}
		if (free_slot == kSlots)
			return false;  /* unreachable below kLiveCap live duels */
		/* publish the arena before the key, so a reader that sees the key
		 * sees a complete entry */
		values[free_slot].store(v, std::memory_order_release);
		keys[free_slot].store(k, std::memory_order_release);
		++live;
		return true;
	}
	MfArena* get(intptr_t k) {
		size_t i = slot_of(k);
		for (size_t n = 0; n < kSlots; ++n, i = (i + 1) % kSlots) {
			const intptr_t key = keys[i].load(std::memory_order_acquire);
			if (key == k)
				return values[i].load(std::memory_order_acquire);
			if (key == kEmpty)
				return nullptr;
			/* kTombstone: keep probing, the chain is intact */
		}
		return nullptr;
	}
	void drop(intptr_t k) {
		size_t i = slot_of(k);
		for (size_t n = 0; n < kSlots; ++n, i = (i + 1) % kSlots) {
			const intptr_t key = keys[i].load(std::memory_order_relaxed);
			if (key == k) {
				keys[i].store(kTombstone, std::memory_order_release);
				values[i].store(nullptr, std::memory_order_release);
				--live;
				return;
			}
			if (key == kEmpty)
				return;
		}
	}
};

Registry g_registry;

void* arena_alloc(size_t n) {
	if (n == 0)
		n = 1;
	MfArena* cur = g_current;
	void* raw = cur ? mspace_malloc(cur->ms, n + kHeader)
	                : std::malloc(n + kHeader);
	if (!raw)
		return nullptr;
	*static_cast<mspace*>(raw) = cur ? cur->ms : nullptr;
	return static_cast<char*>(raw) + kHeader;
}

void arena_free(void* p) noexcept {
	if (!p)
		return;
	void* raw = static_cast<char*>(p) - kHeader;
	mspace owner = *static_cast<mspace*>(raw);
	if (owner)
		mspace_free(owner, raw);
	else
		std::free(raw);
}

void* arena_alloc_aligned(size_t n, size_t align) {
	if (align <= kHeader)
		return arena_alloc(n);
	/* returned pointer = raw + align (aligned, with >= 32 bytes of stash
	 * below it); the stash holds two words: [-8] owner mspace, [-16] the
	 * raw base for the free path */
	MfArena* cur = g_current;
	void* raw = cur ? mspace_memalign(cur->ms, align, n + align)
	                : std::aligned_alloc(align, n + align);
	if (!raw)
		return nullptr;
	char* out = static_cast<char*>(raw) + align;
	*reinterpret_cast<mspace*>(out - sizeof(void*)) =
	    cur ? cur->ms : nullptr;
	*reinterpret_cast<void**>(out - 2 * sizeof(void*)) = raw;
	return out;
}

}  // namespace

/* -- the interposed allocator (binds library-locally via -Bsymbolic) ------ */

/* GCC exports replaceable allocation functions from the dynamic symbol
 * table even under -fvisibility=hidden.  Left exported, a library the host
 * process loads later (torch lazily dlopens its BLAS backend at the first
 * matmul) binds *its* operator delete to ours and feeds us pointers our
 * header rewind was never behind -- glibc's free(): invalid pointer, seen
 * live on the first server boot.  Hidden declarations ahead of the
 * definitions keep the interposition strictly inside this object. */
#define MF_HIDDEN __attribute__((visibility("hidden")))
void* operator new(std::size_t) MF_HIDDEN;
void* operator new[](std::size_t) MF_HIDDEN;
void* operator new(std::size_t, const std::nothrow_t&) noexcept MF_HIDDEN;
void* operator new[](std::size_t, const std::nothrow_t&) noexcept MF_HIDDEN;
void operator delete(void*) noexcept MF_HIDDEN;
void operator delete[](void*) noexcept MF_HIDDEN;
void operator delete(void*, std::size_t) noexcept MF_HIDDEN;
void operator delete[](void*, std::size_t) noexcept MF_HIDDEN;
void operator delete(void*, const std::nothrow_t&) noexcept MF_HIDDEN;
void operator delete[](void*, const std::nothrow_t&) noexcept MF_HIDDEN;
void* operator new(std::size_t, std::align_val_t) MF_HIDDEN;
void* operator new[](std::size_t, std::align_val_t) MF_HIDDEN;
void operator delete(void*, std::align_val_t) noexcept MF_HIDDEN;
void operator delete[](void*, std::align_val_t) noexcept MF_HIDDEN;
void operator delete(void*, std::size_t, std::align_val_t) noexcept MF_HIDDEN;
void operator delete[](void*, std::size_t, std::align_val_t) noexcept MF_HIDDEN;

void* operator new(std::size_t n) {
	void* p = arena_alloc(n);
	if (!p)
		throw std::bad_alloc();
	return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
	return arena_alloc(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
	return arena_alloc(n);
}
void operator delete(void* p) noexcept { arena_free(p); }
void operator delete[](void* p) noexcept { arena_free(p); }
void operator delete(void* p, std::size_t) noexcept { arena_free(p); }
void operator delete[](void* p, std::size_t) noexcept { arena_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { arena_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { arena_free(p); }

/* C++17 over-aligned forms.  ocgcore itself has no over-aligned types; STL
 * containers may still request them for exotic value types, so the forms
 * exist and stay correct rather than aborting. */
void* operator new(std::size_t n, std::align_val_t a) {
	void* p = arena_alloc_aligned(n, static_cast<size_t>(a));
	if (!p)
		throw std::bad_alloc();
	return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
	return ::operator new(n, a);
}
namespace {
void aligned_free(void* p) noexcept {
	if (!p)
		return;
	mspace owner =
	    *reinterpret_cast<mspace*>(static_cast<char*>(p) - sizeof(void*));
	void* raw =
	    *reinterpret_cast<void**>(static_cast<char*>(p) - 2 * sizeof(void*));
	if (owner)
		mspace_free(owner, raw);
	else
		std::free(raw);
}
}  // namespace
void operator delete(void* p, std::align_val_t) noexcept { aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
	aligned_free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
	aligned_free(p);
}

/* -- arena lifecycle ------------------------------------------------------ */

extern "C" void* mfsnap_arena_create(size_t reserve_bytes) {
	void* base = mmap(nullptr, reserve_bytes, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED)
		return nullptr;
	auto* arena = static_cast<MfArena*>(std::malloc(sizeof(MfArena)));
	if (!arena) {
		munmap(base, reserve_bytes);
		return nullptr;
	}
	arena->base = base;
	arena->reserve = reserve_bytes;
	arena->ms = create_mspace_with_base(base, reserve_bytes, 0);
	if (!arena->ms) {
		munmap(base, reserve_bytes);
		std::free(arena);
		return nullptr;
	}
	return arena;
}

extern "C" void mfsnap_arena_destroy(void* arena_) {
	auto* arena = static_cast<MfArena*>(arena_);
	if (!arena)
		return;
	/* no destroy_mspace: the whole heap is the mapping, and it goes away */
	munmap(arena->base, arena->reserve);
	std::free(arena);
}

extern "C" void* mfsnap_lua_alloc(void* ud, void* ptr, size_t osize,
                                  size_t nsize) {
	(void)osize;
	mspace ms = static_cast<MfArena*>(ud)->ms;
	if (nsize == 0) {
		if (ptr)
			mspace_free(ms, ptr);
		return nullptr;
	}
	return ptr ? mspace_realloc(ms, ptr, nsize) : mspace_malloc(ms, nsize);
}

/* -- registry and guard --------------------------------------------------- */

int32_t mfsnap_reserve_duel() {
	return g_registry.reserve() ? 0 : -1;
}

void mfsnap_cancel_duel() {
	g_registry.cancel();
}

int32_t mfsnap_register_duel(intptr_t pduel, void* arena) {
	return g_registry.put(pduel, static_cast<MfArena*>(arena)) ? 0 : -1;
}

int32_t mfsnap_live_duel_capacity() {
	return (int32_t)Registry::kLiveCap;
}

void mfsnap_unregister_duel(intptr_t pduel) {
	g_registry.drop(pduel);
}

void* mfsnap_arena_of(intptr_t pduel) {
	return g_registry.get(pduel);
}

void mfsnap_enter(void* arena) {
	g_current = static_cast<MfArena*>(arena);
}

void mfsnap_exit() {
	g_current = nullptr;
}

void* mfsnap_current() {
	return g_current;
}

/* guards nest: process() holds one while the message-handler callback
 * (itself an api entry) opens another -- restore, never null */
MfArenaGuard::MfArenaGuard(intptr_t pduel) : prev_(g_current) {
	g_current = g_registry.get(pduel);
}

MfArenaGuard::~MfArenaGuard() {
	g_current = static_cast<MfArena*>(prev_);
}

/* -- snapshot / rollback -------------------------------------------------- */

namespace {
struct MfSnap {
	size_t len;
	/* bytes follow */
};
}  // namespace

extern "C" void* mf_duel_snapshot(intptr_t pduel) {
	auto* arena = static_cast<MfArena*>(g_registry.get(pduel));
	if (!arena)
		return nullptr;
	size_t len = mspace_used_extent(arena->ms);
	if (!len || len > arena->reserve)
		return nullptr;
	auto* snap = static_cast<MfSnap*>(std::malloc(sizeof(MfSnap) + len));
	if (!snap)
		return nullptr;
	snap->len = len;
	std::memcpy(snap + 1, arena->base, len);
	return snap;
}

extern "C" int32_t mf_duel_rollback(intptr_t pduel, void* snap_) {
	auto* arena = static_cast<MfArena*>(g_registry.get(pduel));
	auto* snap = static_cast<MfSnap*>(snap_);
	if (!arena || !snap)
		return -1;
	/* Bound against the arena reservation, not the current extent: the old
	 * premise "the allocator's top only ever grows" is false -- freeing the
	 * chunk directly below top coalesces into it and the used extent
	 * shrinks (measured: 34 shrink events across 28 of 60 duels, enough to
	 * fail 13-28% of searched decisions).  Restoring a snapshot longer than
	 * the current extent is safe: the whole reservation stays mapped, and
	 * the copy restores the allocator's own top pointer along with the
	 * data.  The only impossible snapshot is one longer than the mapping. */
	if (snap->len > arena->reserve)
		return -2;
	std::memcpy(arena->base, snap + 1, snap->len);
	return 0;
}

extern "C" void mf_snapshot_free(void* snap) {
	std::free(snap);
}

extern "C" size_t mf_duel_arena_extent(intptr_t pduel) {
	auto* arena = static_cast<MfArena*>(g_registry.get(pduel));
	return arena ? mspace_used_extent(arena->ms) : 0;
}
