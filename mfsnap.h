/*
 * mfsnap.h -- per-duel memory arenas with memcpy snapshot/rollback.
 *
 * Search needs to branch a live duel.  Forking the process was the old way
 * to copy an opaque C++/Lua object graph; this is the direct way: every
 * allocation a duel makes -- C++ objects, STL nodes, the whole lua_State --
 * lands in that duel's own dlmalloc mspace sitting in one contiguous
 * mmap'd region.  A snapshot is a memcpy of the used extent; a rollback
 * copies it back to the same addresses, so every pointer inside is valid
 * again by construction.  Nothing is serialized, least of all coroutines.
 *
 * Routing works through three funnels, all defined in mfsnap.cpp:
 *   - global operator new/delete for this shared object (the build already
 *     links -Bsymbolic + hidden visibility, so these bind library-locally
 *     and the host process's allocator is untouched);
 *   - a thread-local "current arena", set by MfArenaGuard at every ocgapi
 *     entry point that carries a duel pointer;
 *   - lua_newstate's allocator parameter for the duel's Lua heap.
 * Each C++ block carries a 16-byte owner header, so frees route to the
 * right heap no matter which duel (or none) is current at free time.
 *
 * Single-threaded by contract, like the rest of ocgcore.
 */
#ifndef MFSNAP_H_
#define MFSNAP_H_

#include <cstddef>
#include <cstdint>

extern "C" {

/* arena lifecycle (used by ocgapi create/end duel) */
void* mfsnap_arena_create(size_t reserve_bytes);
void  mfsnap_arena_destroy(void* arena);

/* the lua allocator for this duel's interpreter; ud = the arena */
void* mfsnap_lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

/* exported snapshot API (reaches the .so surface via ocgapi.cpp) */
void*   mf_duel_snapshot(intptr_t pduel);
int32_t mf_duel_rollback(intptr_t pduel, void* snap);
void    mf_snapshot_free(void* snap);
/* observability: current used extent of the duel's arena, 0 if unknown */
size_t  mf_duel_arena_extent(intptr_t pduel);

}

/* registry: ocgapi records which arena serves which duel.  A creation reserves its place first (0, or -1 when
 * mfsnap_live_duel_capacity() duels are live or reserved) and registering consumes the reservation; all three run
 * under ocgapi's exclusive duel_set_mutex. */
int32_t mfsnap_reserve_duel();
void  mfsnap_cancel_duel();
int32_t mfsnap_register_duel(intptr_t pduel, void* arena);
int32_t mfsnap_live_duel_capacity();
void  mfsnap_unregister_duel(intptr_t pduel);
void* mfsnap_arena_of(intptr_t pduel);

/* make `arena` the target of every allocation on this thread */
void mfsnap_enter(void* arena);
void mfsnap_exit();
/* the arena currently entered on this thread (null outside any guard);
 * the interpreter constructor runs inside create_duel's enter window,
 * before the duel is registered, so it takes the arena from here */
void* mfsnap_current();

/* RAII guard for ocgapi entry points */
struct MfArenaGuard {
	explicit MfArenaGuard(intptr_t pduel);
	~MfArenaGuard();
	MfArenaGuard(const MfArenaGuard&) = delete;
	void operator=(const MfArenaGuard&) = delete;
private:
	void* prev_;
};

#endif /* MFSNAP_H_ */
