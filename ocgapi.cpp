/*
 * interface.cpp
 *
 *  Created on: 2010-5-2
 *      Author: Argon
 */
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <vector>
#include "ocgapi.h"
#include "mfsnap.h"
#include "duel.h"
#include "card.h"
#include "group.h"
#include "effect.h"
#include "field.h"
#include "interpreter.h"
#include "buffer.h"

static uint32_t default_card_reader(uint32_t code, card_data* data) {
	return 0;
}
static uint32_t default_message_handler(intptr_t pduel, uint32_t message_type) {
	MfArenaGuard mf_guard_(pduel);
	return 0;
}
/* The consumer's callbacks: set once, read by every duel's thread. */
static std::atomic<script_reader> sreader{default_script_reader};
static std::atomic<card_reader> creader{default_card_reader};
static std::atomic<message_handler> mhandler{default_message_handler};
/* default_script_reader's result buffer: a consumer that runs duels on several threads installs its own
 * reader before creating any duel (upstream contract). */
static byte buffer[0x100000];
/* The live duels.  Several threads call the read entry points on their own duels while other threads create
 * and end duels, so every access holds duel_set_mutex: shared to look a handle up, exclusive to insert or
 * erase one (a std::set lookup racing an insert or erase is undefined behavior; it reported live duels
 * dead).  The exclusive side also covers the arena registry's writers (mfsnap.cpp Registry: one writer at a
 * time, lock-free readers) and arena creation (dlmalloc's global parameters initialize, without locks, on
 * the first create_mspace_with_base), so none of the core's global state relies on its consumer serializing
 * create_duel and end_duel. */
static std::set<duel*> duel_set;
static std::shared_mutex duel_set_mutex;
static bool live_duel(intptr_t pduel) {
	std::shared_lock<std::shared_mutex> lock(duel_set_mutex);
	return duel_set.find((duel*)pduel) != duel_set.end();
}

void set_script_reader(script_reader f) {
	sreader.store(f, std::memory_order_release);
}
void set_card_reader(card_reader f) {
	creader.store(f, std::memory_order_release);
}
void set_message_handler(message_handler f) {
	mhandler.store(f, std::memory_order_release);
}
byte* read_script(const char* script_name, int* len) {
	return sreader.load(std::memory_order_acquire)(script_name, len);
}
uint32_t read_card(uint32_t code, card_data* data) {
	if (code == TEMP_CARD_ID) {
		data->clear();
		return 0;
	}
	return creader.load(std::memory_order_acquire)(code, data);
}
uint32_t handle_message(void* pduel, uint32_t message_type) {
	return mhandler.load(std::memory_order_acquire)((intptr_t)pduel, message_type);
}
byte* default_script_reader(const char* script_name, int* slen) {
	FILE *fp;
	fp = std::fopen(script_name, "rb");
	if (!fp)
		return nullptr;
	size_t len = std::fread(buffer, 1, sizeof buffer, fp);
	std::fclose(fp);
	if (len >= sizeof buffer)
		return nullptr;
	*slen = (int)len;
	return buffer;
}
/* Every allocation the duel ever makes -- C++ objects, STL nodes, the
 * whole lua_State -- must land in the duel's own arena, so the arena is
 * made first, entered for the construction, and exited before the global
 * registries (system heap) are touched.  256MB of NORESERVE address space
 * costs pages only as they are touched. */
static constexpr size_t MF_ARENA_RESERVE = size_t(256) << 20;
/* A creation beyond the registry's live-duel capacity is refused (null): a duel outside an arena would allocate
 * on the system heap, beyond its snapshots' reach, and every arena query would call it invalid. */
static void* new_duel_arena() {
	std::unique_lock<std::shared_mutex> lock(duel_set_mutex);
	if(mfsnap_reserve_duel() != 0)
		return nullptr;
	void* arena = mfsnap_arena_create(MF_ARENA_RESERVE);
	if(!arena)
		mfsnap_cancel_duel();
	return arena;
}
static void add_live_duel(duel* pduel, void* arena) {
	std::unique_lock<std::shared_mutex> lock(duel_set_mutex);
	duel_set.insert(pduel);
	if(mfsnap_register_duel((intptr_t)pduel, arena) != 0)
		std::abort();  /* a reserved place is always there */
}
int32_t duel_live_capacity() {
	return mfsnap_live_duel_capacity();
}

intptr_t create_duel(uint_fast32_t seed) {
	void* arena = new_duel_arena();
	if (!arena)
		return 0;
	mfsnap_enter(arena);
	duel* pduel = new duel();
	mfsnap_exit();
	add_live_duel(pduel, arena);
	MfArenaGuard guard((intptr_t)pduel);
	pduel->random.seed(seed);
	pduel->rng_version = 1;
	return (intptr_t)pduel;
}
intptr_t create_duel_v2(uint32_t seed_sequence[]) {
	void* arena = new_duel_arena();
	if (!arena)
		return 0;
	mfsnap_enter(arena);
	duel* pduel = new duel();
	mfsnap_exit();
	add_live_duel(pduel, arena);
	MfArenaGuard guard((intptr_t)pduel);
	pduel->random.seed(seed_sequence, SEED_COUNT);
	pduel->rng_version = 2;
	return (intptr_t)pduel;
}
int32_t duel_set_future_seed(intptr_t pduel, const uint32_t seed_sequence[]) {
	if(!pduel || !seed_sequence)
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	pd->random.seed(seed_sequence, SEED_COUNT);
	pd->rng_version = 2;
	return 0;
}
int32_t query_local_entity_map(intptr_t pduel, uint32_t version, byte* buf, int32_t buf_size) {
	// Do not enter the arena, allocate an STL scratch container, or call any
	// card getter. Even allocator bookkeeping would violate read-only snapshots.
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel)
	   || !live_duel(pduel))
		return -1;
	if(version != LOCAL_ENTITY_MAP_VERSION)
		return -2;
	const duel* pd = (const duel*)pduel;
	// call_depth also counts SUSPENDED coroutines; LUA_YIELD leaves it nonzero
	// after current_state has returned to the main thread. Those host boundaries
	// are safe to inspect. An executing native/Lua callback was rejected above.
	if(pd->lua->current_state != pd->lua->lua_state || pd->lua->no_action || !pd->lua->params.empty())
		return -1;
	if(pd->cards.size() > (size_t(INT32_MAX) - LOCAL_ENTITY_MAP_HEADER_LEN) / LOCAL_ENTITY_MAP_RECORD_LEN)
		return -4;
	const int32_t required = LOCAL_ENTITY_MAP_HEADER_LEN
		+ int32_t(pd->cards.size()) * LOCAL_ENTITY_MAP_RECORD_LEN;
	if(buf_size < 0 || (!buf && buf_size != 0) || (buf && buf_size < required))
		return -3;
	// Validate everything before touching the output, including stale overlay
	// pointers and vector membership. Iterating all allocated cards includes the
	// location-0 temp card and not-yet-placed objects without inventing a slot.
	for(const card* pc : pd->cards) {
		if(!pc || !pc->cardid || pc->pduel != pd
		   || (pc->owner > 1 && pc->owner != PLAYER_NONE))
			return -4;
		for(const card* other : pd->cards)
			if(other != pc && other->cardid == pc->cardid)
				return -4;
		if(pc->overlay_target) {
			if(!pd->cards.count(pc->overlay_target) || pc->current.location != LOCATION_OVERLAY
			   || pc->current.controler != PLAYER_NONE)
				return -4;
			const auto& materials = pc->overlay_target->xyz_materials;
			if(pc->current.sequence >= materials.size() || materials[pc->current.sequence] != pc)
				return -4;
		} else if(pc->current.location) {
			if(pc->current.controler > 1)
				return -4;
			const auto* zone = pd->game_field->get_field_vector(pc->current.controler, pc->current.location);
			if(!zone || pc->current.sequence >= zone->size() || (*zone)[pc->current.sequence] != pc)
				return -4;
		} else if(pc->current.controler > 1 && pc->current.controler != PLAYER_NONE) {
			return -4;
		}
	}
	if(!buf)
		return required;
	byte* output = buf;
	auto write = [&output](uint64_t value, unsigned width) {
		for(unsigned i = 0; i < width; ++i)
			*output++ = byte(value >> (8 * i));
	};
	write(0x314d454c, 4); // LEM1
	write(LOCAL_ENTITY_MAP_VERSION, 2);
	write(LOCAL_ENTITY_MAP_RECORD_LEN, 2);
	write(pd->cards.size(), 4);
	write(required, 4);
	uint64_t previous = 0;
	for(size_t row = 0; row < pd->cards.size(); ++row) {
		const card* pc = nullptr;
		for(const card* candidate : pd->cards)
			if(candidate->cardid > previous && (!pc || candidate->cardid < pc->cardid))
				pc = candidate;
		previous = pc->cardid;
		write(pc->cardid, 8);
		write(pc->overlay_target ? pc->overlay_target->cardid : 0, 8);
		write(pc->owner, 1);
		write(pc->current.controler, 1);
		write(pc->current.location, 1);
		const uint32_t code = pc->data.code;
		write(code >= 999000001u && code <= 999000004u ? code - 999000000u : 0, 1);
		write(pc->current.location ? pc->current.sequence : UINT32_MAX, 4);
		write(pc->overlay_target ? pc->current.sequence : UINT32_MAX, 4);
		write(0, 4);
	}
	return required;
}
int32_t duel_reorder_zone_uids(intptr_t pduel, uint8_t playerid, uint8_t location,
    const uint64_t uids[], int32_t count) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel) || playerid > 1
	   || (location != LOCATION_DECK && location != LOCATION_HAND)
	   || !live_duel(pduel))
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	interpreter* lua = pd->lua;
	if(lua->current_state != lua->lua_state || lua->no_action || !lua->params.empty())
		return -1;
	auto& zone = location == LOCATION_DECK ? pd->game_field->player[playerid].list_main
	                                       : pd->game_field->player[playerid].list_hand;
	if(count < 0 || (size_t)count != zone.size() || (count && !uids))
		return -2;
	if(location == LOCATION_DECK && pd->game_field->core.deck_reversed)
		return -4;
	// Resolve the whole plan first; n is a zone size, so quadratic lookups
	// need no scratch allocation.
	for(int32_t i = 0; i < count; ++i) {
		for(int32_t k = 0; k < i; ++k)
			if(uids[k] == uids[i])
				return -3;
		card* found = nullptr;
		for(auto* pcard : zone)
			if(pcard->cardid == uids[i])
				found = pcard;
		if(!found || found->current.controler != playerid || found->current.location != location)
			return -3;
		if(found->current.position & POS_FACEUP)
			return -4;
	}
	for(int32_t i = 0; i < count; ++i) {
		int32_t j = i;
		while(zone[j]->cardid != uids[i])
			++j;
		std::swap(zone[i], zone[j]);
	}
	pd->game_field->reset_sequence(playerid, location);
	return 0;
}
namespace {
// Sort a card_set again after sort positions changed: iterating walks the tree, not the comparator.
void mf_resort(card_set& cset) {
	if(cset.size() < 2)
		return;
	card_set sorted(cset.begin(), cset.end());
	cset.swap(sorted);
}
bool mf_sorted(const card_set& cset) {
	return std::is_sorted(cset.begin(), cset.end(), card_sort()) && std::adjacent_find(cset.begin(), cset.end(),
	    [](card* a, card* b) { return !card_sort()(a, b); }) == cset.end();
}
void mf_resort_group(group* pgroup) {
	// A clean iterator (GetFirst ran and the group did not change since) keeps its card; any other is reset, as
	// GetNext refuses it anyway.
	card* current = !pgroup->is_iterator_dirty && pgroup->it != pgroup->container.end() ? *pgroup->it : nullptr;
	const bool at_end = !pgroup->is_iterator_dirty && pgroup->it == pgroup->container.end();
	mf_resort(pgroup->container);
	pgroup->it = current ? pgroup->container.find(current) : pgroup->container.begin();
	if(at_end)
		pgroup->it = pgroup->container.end();
}
// A pending unit's own card set, allocated by its operation and kept in the unit: PROCESSOR_SSET_G holds the
// cards still to place in ptarget during steps 1 and 2 (step 0 has the script's group there, and the set is
// freed on leaving step 2). Other units that keep such a set are refused by duel_order_cards (see below).
card_set* mf_unit_card_set(const processor_unit& unit) {
	if(unit.type == PROCESSOR_SSET_G && (unit.step == 1 || unit.step == 2))
		return (card_set*)unit.ptarget;
	return nullptr;
}
// Units whose operation may keep a private card set whose lifetime this file does not follow.
bool mf_unit_holds_unfollowed_set(const processor_unit& unit) {
	switch(unit.type) {
	case PROCESSOR_DRAW:
	case PROCESSOR_GET_CONTROL:
	case PROCESSOR_TRAP_MONSTER_ADJUST:
	case PROCESSOR_SUMMON_RULE:
	case PROCESSOR_MSET:
	case PROCESSOR_CHANGEPOS:
		return true;
	default:
		return false;
	}
}
// Every card_set of the duel but the groups' (a group also carries an iterator into its set).
template<typename F> void mf_each_loose_card_set(duel* pd, F&& visit) {
	for(auto* pcard : pd->cards) {
		visit(pcard->equiping_cards);
		visit(pcard->material_cards);
		visit(pcard->effect_target_owner);
		visit(pcard->effect_target_cards);
	}
	auto& core = pd->game_field->core;
	for(const auto* list : {&core.subunits, &core.units})
		for(const auto& unit : *list)
			if(card_set* cset = mf_unit_card_set(unit))
				visit(*cset);
	for(auto* cset : {&core.leave_confirmed, &core.special_summoning, &core.unable_tofield_set, &core.equiping_cards,
	                  &core.control_adjust_set[0], &core.control_adjust_set[1], &core.unique_destroy_set,
	                  &core.self_destroy_set, &core.self_tograve_set, &core.trap_monster_adjust_set[0],
	                  &core.trap_monster_adjust_set[1], &core.release_cards, &core.release_cards_ex,
	                  &core.release_cards_ex_oneof, &core.battle_destroy_rep, &core.fusion_materials,
	                  &core.synchro_materials, &core.operated_set, &core.discarded_set, &core.destroy_canceled,
	                  &core.indestructable_count_set, &core.delayed_enable_set, &core.set_group_pre_set,
	                  &core.set_group_set})
		visit(*cset);
}
}
int32_t duel_order_cards(intptr_t pduel, const uint64_t uids[], int32_t count) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel) || !live_duel(pduel))
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	interpreter* lua = pd->lua;
	// Suspended coroutines are permitted (a group they iterate keeps its card); an executing callback is not.
	if(lua->current_state != lua->lua_state || lua->no_action || !lua->params.empty())
		return -1;
	if(count < 2 || !uids)
		return -2;
	for(const auto* list : {&pd->game_field->core.subunits, &pd->game_field->core.units})
		for(const auto& unit : *list)
			if(mf_unit_holds_unfollowed_set(unit))
				return -4;
	std::vector<card*> listed;
	std::vector<uint64_t> slots;
	for(int32_t i = 0; i < count; ++i) {
		for(int32_t k = 0; k < i; ++k)
			if(uids[k] == uids[i])
				return -3;
		card* found = nullptr;
		for(auto* pcard : pd->cards)
			if(pcard->cardid == uids[i])
				found = pcard;
		if(!found)
			return -3;
		listed.push_back(found);
		slots.push_back(found->sortid);
	}
	std::sort(slots.begin(), slots.end());
	bool unchanged = true;
	for(int32_t i = 0; i < count; ++i)
		unchanged = unchanged && listed[i]->sortid == slots[i];
	if(unchanged)
		return 0;
	// A pending group set takes its cards one by one in their order and gives each its zone in that order (the
	// zone list is indexed by position, sset_g steps 2 and 7): the cards it took, and at step 2 the one whose zone
	// the seat just answered, keep their order among all its cards; only the cards still to take may change theirs.
	auto after = [&](const card* pcard) {
		auto at = std::find(listed.begin(), listed.end(), pcard);
		return at == listed.end() ? pcard->sortid : slots[at - listed.begin()];
	};
	for(const auto* list : {&pd->game_field->core.subunits, &pd->game_field->core.units})
		for(const auto& unit : *list) {
			if(unit.type != PROCESSOR_SSET_G || unit.step == 0)
				continue;
			const auto& core = pd->game_field->core;
			std::vector<const card*> taken(core.set_group_pre_set.begin(), core.set_group_pre_set.end());
			taken.insert(taken.end(), core.set_group_set.begin(), core.set_group_set.end());
			std::vector<const card*> all(taken);
			if(const card_set* rest = mf_unit_card_set(unit)) {
				if(unit.step == 2 && !rest->empty())
					taken.push_back(*rest->begin());
				all.insert(all.end(), rest->begin(), rest->end());
			}
			for(const auto* first : taken)
				for(const auto* other : all)
					if(first != other && (first->sortid < other->sortid) != (after(first) < after(other)))
						return -6;
		}
	for(int32_t i = 0; i < count; ++i)
		listed[i]->sortid = slots[i];
	for(auto* pgroup : pd->groups)
		mf_resort_group(pgroup);
	for(auto* pgroup : pd->sgroups)
		mf_resort_group(pgroup);
	mf_each_loose_card_set(pd, [](card_set& cset) { mf_resort(cset); });
	bool sorted = true;
	for(auto* pgroup : pd->groups)
		sorted = sorted && mf_sorted(pgroup->container);
	for(auto* pgroup : pd->sgroups)
		sorted = sorted && mf_sorted(pgroup->container);
	mf_each_loose_card_set(pd, [&sorted](card_set& cset) { sorted = sorted && mf_sorted(cset); });
	return sorted ? 0 : -5;
}
int32_t query_operation_groups(intptr_t pduel, byte* buf, int32_t buf_size) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel) || !live_duel(pduel))
		return -1;
	const duel* pd = (const duel*)pduel;
	// A unit's target is often a card or an effect cast to group*: only a live group is read.
	std::vector<const group*> found;
	auto visit = [&](const processor_list& list) {
		for(const auto& unit : list) {
			const group* pgroup = unit.ptarget;
			if(pgroup && (pd->groups.count(unit.ptarget) || pd->sgroups.count(unit.ptarget))
			   && std::find(found.begin(), found.end(), pgroup) == found.end())
				found.push_back(pgroup);
		}
	};
	visit(pd->game_field->core.subunits);
	visit(pd->game_field->core.units);
	std::vector<const card_set*> sets;
	for(const auto* pgroup : found)
		sets.push_back(&pgroup->container);
	for(const auto* list : {&pd->game_field->core.subunits, &pd->game_field->core.units})
		for(const auto& unit : *list)
			if(const card_set* cset = mf_unit_card_set(unit))
				sets.push_back(cset);
	int32_t required = 4;
	for(const auto* cset : sets)
		required += 4 + 12 * (int32_t)cset->size();
	if(buf_size < 0 || (!buf && buf_size != 0) || (buf && buf_size < required))
		return -3;
	if(!buf)
		return required;
	byte* output = buf;
	auto write = [&output](uint64_t value, unsigned width) {
		for(unsigned i = 0; i < width; ++i) *output++ = byte(value >> (8 * i));
	};
	write(sets.size(), 4);
	for(const auto* cset : sets) {
		write(cset->size(), 4);
		for(const auto* pcard : *cset) {
			write(pcard->cardid, 8);
			write(pcard->data.code, 4);
		}
	}
	return required;
}
int32_t duel_reorder_deck_uids(intptr_t pduel, uint8_t playerid, const uint64_t uids[], int32_t count) {
	return duel_reorder_zone_uids(pduel, playerid, LOCATION_DECK, uids, count);
}
int32_t duel_set_phase_pass_audit(intptr_t pduel, uint32_t version, uint8_t enabled) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel)
	   || !live_duel(pduel)) return -1;
	if(version != 1 || enabled > 1) return -2;
	duel* pd = (duel*)pduel;
	if(pd->lua->current_state != pd->lua->lua_state || pd->lua->no_action || !pd->lua->params.empty()
	   || pd->phase_pass_audit_process_seen || pd->game_field->infos.turn_id || pd->phase_pass_audit_batch) return -1;
	pd->phase_pass_audit_enabled = enabled != 0;
	return 0;
}
int32_t query_phase_pass_audit(intptr_t pduel, uint32_t version, byte* buf, int32_t buf_size) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel)
	   || !live_duel(pduel)) return -1;
	if(version != 1) return -2;
	const duel* pd = (const duel*)pduel;
	if(pd->lua->current_state != pd->lua->lua_state || pd->lua->no_action || !pd->lua->params.empty()) return -1;
	if(pd->phase_pass_audit_count > duel::PHASE_PASS_AUDIT_CAPACITY) return -4;
	const int32_t required = 32 + 12 * pd->phase_pass_audit_count;
	if(buf_size < 0 || (!buf && buf_size != 0) || (buf && buf_size < required)) return -3;
	if(!buf) return required;
	byte* output = buf;
	auto write = [&output](uint64_t value, unsigned width) {
		for(unsigned i = 0; i < width; ++i) *output++ = byte(value >> (8 * i));
	};
	write(0x31575050, 4); // PPW1
	write(1, 2); write(12, 2); write(required, 4);
	write(pd->phase_pass_audit_count, 4); write(pd->phase_pass_audit_batch, 8);
	write(pd->phase_pass_audit_enabled, 1); write(pd->phase_pass_audit_overflow, 1); write(0, 6);
	for(uint32_t i = 0; i < pd->phase_pass_audit_count; ++i) {
		const auto& r = pd->phase_pass_audit[i];
		write(r.message_offset, 4); write(r.turn, 4); write(r.phase, 2);
		write(r.player, 1); write(r.priority_passed, 1);
	}
	return required;
}
int32_t duel_hydrate_card(intptr_t pduel, uint8_t playerid, uint8_t location,
    uint8_t sequence, uint32_t expected_placeholder, uint32_t code) {
	if(!mfsnap_arena_of(pduel) || playerid > 1)
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	interpreter* lua = pd->lua;
	// Suspended target/operation coroutines are permitted. An executing Lua
	// callback is not: restoring its heap below an active stack is unsafe.
	if(lua->current_state != lua->lua_state || lua->no_action || !lua->params.empty())
		return -1;
	if(expected_placeholder < 999000001u || expected_placeholder > 999000004u)
		return -2;
	card* pc = nullptr;
	if(location == LOCATION_MZONE || location == LOCATION_SZONE)
		pc = pd->game_field->get_field_card(playerid, location, sequence);
	else if(location == LOCATION_DECK || location == LOCATION_HAND || location == LOCATION_EXTRA
	        || location == LOCATION_REMOVED) {
		auto* cards = pd->game_field->get_field_vector(playerid, location);
		if(cards && sequence < cards->size())
			pc = (*cards)[sequence];
	}
	if(!pc || pc->data.code != expected_placeholder || interpreter::is_load_script(pc->data)
	   || ((location & (LOCATION_ONFIELD | LOCATION_REMOVED | LOCATION_EXTRA))
	       && (pc->current.position & POS_FACEUP)))
		return -2;
	// A true blank has no own script effects. External effects/flags/relations
	// stay attached to the same card object and are never cancelled or removed.
	for(const auto& item : pc->indexer)
		if(item.first->owner == pc && item.first->is_flag(EFFECT_FLAG_INITIAL))
			return -2;
	card_data data;
	::read_card(code, &data);
	// A main-deck blank may become a set proxy: 999000003 for the spell and trap zones, 999000004 (a field
	// spell) for the field zone.
	const bool spell_shape = expected_placeholder == 999000001u
	    && (code == 999000003u && data.type == (TYPE_SPELL | TYPE_NORMAL)
	        || code == 999000004u && data.type == (TYPE_SPELL | TYPE_FIELD));
	if(!code || !data.type || ((code >= 999000001u && code <= 999000004u) && !spell_shape)
	   || bool(data.type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK))
	      != bool(pc->data.type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK)))
		return -3;
	void* saved = mf_duel_snapshot(pduel);
	if(!saved)
		return -4;
	bool ok = true;
	try {
		pc->data = data;
		pc->data.code = code;
		ok = lua->rebind_card(pc);
		if(ok && interpreter::is_load_script(pc->data)) {
			pc->set_status(STATUS_INITIALIZING, TRUE);
			card* outer = pd->initializing;
			pd->initializing = pc;
			lua->add_param(pc, PARAM_TYPE_CARD);
			ok = lua->call_card_function(pc, "initial_effect", 1, 0) != 0;
			pd->initializing = outer;
			pc->set_status(STATUS_INITIALIZING, FALSE);
		}
		if(ok) {
			// apply_field_effect() also zeros this card's summon counters; these
			// belong to its physical history and must survive hydration.
			const auto first = pc->spsummon_counter[0], second = pc->spsummon_counter[1];
			pc->apply_field_effect();
			pc->spsummon_counter[0] = first;
			pc->spsummon_counter[1] = second;
		}
		// A monster set in the spell and trap zone is, while it stays set, the type its EFFECT_MONSTER_SSET names
		// (a Spell): field::sset gives it this change when it is set there, and a hydrated one gets the same.
		effect* monster_sset = ok && location == LOCATION_SZONE && (pc->data.type & TYPE_MONSTER)
		    ? pc->is_affected_by_effect(EFFECT_MONSTER_SSET) : nullptr;
		if(monster_sset) {
			effect* peffect = pd->new_effect();
			peffect->owner = pc;
			peffect->type = EFFECT_TYPE_SINGLE;
			peffect->code = EFFECT_CHANGE_TYPE;
			peffect->reset_flag = RESET_EVENT + 0x1fe0000;
			peffect->value = monster_sset->get_value();
			pc->add_effect(peffect);
		}
	} catch(...) {
		ok = false;
	}
	const int result = ok ? 0 : (mf_duel_rollback(pduel, saved) == 0 ? -5 : -6);
	mf_snapshot_free(saved);
	return result;
}
// ---- belief-world search: dormant identities and in-place replacement of hidden identities -------------------
namespace {
constexpr uint32_t kWatcherEventCode = 1000;  // codes from here on are events (EVENT_*), below rules (EFFECT_*)
size_t activity_counters(const field* pfield) {
	const auto& core = pfield->core;
	return core.summon_counter.size() + core.normalsummon_counter.size() + core.spsummon_counter.size()
	       + core.flipsummon_counter.size() + core.attack_counter.size() + core.chain_counter.size();
}
// What pending state holds a card (0 none): 1 a chain link or trigger (its effect, or the card among a link's
// targets), 4 a pending operation's group.
uint32_t held_by_pending(duel* pd, card* pc) {
	auto& core = pd->game_field->core;
	uint32_t bits = 0;
	auto held = [pc](const chain& link) {
		return (link.triggering_effect && link.triggering_effect->get_handler() == pc)
		       || (link.target_cards && link.target_cards->container.count(pc));
	};
	for(const chain_array* links : { &core.select_chains, &core.current_chain, &core.ignition_priority_chains })
		for(const auto& link : *links)
			if(held(link))
				bits |= 1;
	for(const chain_list* links : { &core.continuous_chain, &core.solving_continuous, &core.sub_solving_continuous,
	                                &core.delayed_continuous_tp, &core.delayed_continuous_ntp, &core.desrep_chain,
	                                &core.new_fchain, &core.new_fchain_s, &core.new_ochain, &core.new_ochain_s,
	                                &core.new_fchain_b, &core.new_ochain_b, &core.new_ochain_h, &core.new_chains })
		for(const auto& link : *links)
			if(held(link))
				bits |= 1;
	for(effect* peffect : core.select_effects)
		if(peffect && peffect->get_handler() == pc)
			bits |= 1;
	for(const auto* delayed : { &core.delayed_quick_tmp, &core.delayed_quick })
		for(const auto& item : *delayed)
			if(item.first && item.first->get_handler() == pc)
				bits |= 1;
	for(const auto& item : core.quick_f_chain)
		if(item.first && item.first->get_handler() == pc)
			bits |= 1;
	// The selection lists (core.select_cards and the like) are not read: they keep the last selection's cards after it
	// is answered. A live selection's cards are its prompt's menu, whose places the caller keeps (a rewrite keeps the
	// card object, so a menu place whose code stays names the same card).
	for(const auto* list : { &core.subunits, &core.units })
		for(const auto& unit : *list) {
			const group* pgroup = unit.ptarget;
			if(pgroup && (pd->groups.count(unit.ptarget) || pd->sgroups.count(unit.ptarget))
			   && pgroup->container.count(pc))
				bits |= 4;
			if(const card_set* cset = mf_unit_card_set(unit))
				if(cset->count(pc))
					bits |= 4;
		}
	return bits;
}
// Transient status bits: an operation on the card is in progress.
constexpr uint32_t kTransientStatus = STATUS_TO_ENABLE | STATUS_TO_DISABLE | STATUS_BATTLE_RESULT | STATUS_SPSUMMON_STEP
    | STATUS_SUMMONING | STATUS_DESTROY_CONFIRMED | STATUS_LEAVE_CONFIRMED | STATUS_BATTLE_DESTROYED
    | STATUS_COPYING_EFFECT | STATUS_CHAINING | STATUS_SUMMON_DISABLED | STATUS_ACTIVATE_DISABLED
    | STATUS_EFFECT_REPLACED | STATUS_FLIP_SUMMONING | STATUS_ATTACK_CANCELED | STATUS_INITIALIZING
    | STATUS_FLIP_SUMMON_DISABLED;
// What keeps a hidden card from changing identity in place (0: nothing): it must carry nothing but its own
// identity's initial effects and its own flag effects (its private state), and no operation, choice, chain or other
// card may hold it. The bits name the reasons (duel_hidden_blockers).
uint32_t replace_blockers(duel* pd, card* pc) {
	uint32_t bits = 0;
	if(!pc->counters.empty())
		bits |= 1;
	if(!pc->relations.empty() || !pc->relate_effect.empty())
		bits |= 2;
	if(!pc->announced_cards.empty() || !pc->attacked_cards.empty() || !pc->battled_cards.empty())
		bits |= 4;
	if(pc->equiping_target || pc->pre_equip_target || pc->overlay_target || !pc->equiping_cards.empty()
	   || !pc->effect_target_owner.empty() || !pc->effect_target_cards.empty() || !pc->xyz_materials.empty())
		bits |= 8;
	for(const auto& item : pc->indexer) {
		const effect* peffect = item.first;
		if(peffect->owner != pc)
			bits |= 16;
		else if(!peffect->is_flag(EFFECT_FLAG_INITIAL) && !(peffect->code & EFFECT_FLAG_EFFECT))
			bits |= 32;
	}
	if(pc->status & kTransientStatus)
		bits |= 64;
	if(pc->unique_code && (pc->current.location & pc->unique_location))
		bits |= 128;
	bits |= held_by_pending(pd, pc) << 8;  // 256 chain or trigger, 512 selection list, 1024 operation group
	if(pc->turn_counter || pc->assume_type)
		bits |= 2048;
	if(!pc->indestructable_effects.empty() || !pc->owning_effect.empty())
		bits |= 4096;
	return bits;
}
bool replaceable(duel* pd, card* pc) {
	return replace_blockers(pd, pc) == 0;
}
}  // namespace
namespace {
// One inert card object of ``code`` (location 0, no controller), its initial_effect run; what its load did at duel
// level: 1 event watchers registered (continuous effects on events), 2 activity counters added, 4 global flags
// enabled, 8 other duel-level effects (rules) registered, 16 an effect given to another card, 32 a registration not
// owned by the loading card (Effect.GlobalEffect), which a later replacement cannot attribute to it.
uint32_t load_inert(duel* pd, uint32_t code) {
	field* pfield = pd->game_field;
	const size_t counters = activity_counters(pfield);
	const uint32_t flags = pfield->core.global_flag;
	std::set<effect*> before;
	for(const auto& item : pfield->effects.indexer)
		before.insert(item.first);
	card* pc = pd->new_card(code);  // runs its initial_effect at location 0, no controller: inert
	uint32_t bits = 0;
	if(activity_counters(pfield) != counters)
		bits |= 2;
	if(pfield->core.global_flag != flags)
		bits |= 4;
	for(const auto& item : pfield->effects.indexer) {
		effect* peffect = item.first;
		if(before.count(peffect))
			continue;
		bits |= ((peffect->type & EFFECT_TYPE_CONTINUOUS) && peffect->code >= kWatcherEventCode) ? 1 : 8;
		if(peffect->owner != pc)
			bits |= 32;
	}
	for(effect* peffect : pd->effects)
		if(peffect->owner == pc && peffect->handler && peffect->handler != pc)
			bits |= 16;
	return bits;
}
bool idle_boundary(duel* pd) {
	interpreter* lua = pd->lua;
	return lua->current_state == lua->lua_state && !lua->no_action && lua->params.empty();
}
}  // namespace
int32_t duel_create_dormant(intptr_t pduel, const uint32_t codes[], int32_t count, uint32_t report[]) {
	if(!mfsnap_arena_of(pduel) || count < 0 || (count && (!codes || !report)))
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	if(!idle_boundary(pd))
		return -1;
	// before any card but the core's own temporary one, once
	if(pd->dormant_identities || pd->cards.size() != 1 || pd->game_field->infos.turn_id)
		return -2;
	for(int32_t i = 1; i < count; ++i)
		if(codes[i] <= codes[i - 1])
			return -3;
	for(int32_t i = 0; i < count; ++i) {
		card_data data;
		::read_card(codes[i], &data);
		if(!codes[i] || !data.type || (codes[i] >= 999000001u && codes[i] <= 999000004u))
			return -3;
	}
	pd->dormant_codes.assign(codes, codes + count);
	for(int32_t i = 0; i < count; ++i)
		report[i] = load_inert(pd, codes[i]);
	pd->dormant_end = pd->game_field->infos.card_id;
	pd->dormant_identities = true;
	return 0;
}
int32_t duel_probe_load(intptr_t pduel, uint32_t code, uint32_t* report) {
	if(!mfsnap_arena_of(pduel) || !report)
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	if(!idle_boundary(pd) || pd->dormant_identities || pd->game_field->infos.turn_id)
		return -1;
	card_data data;
	::read_card(code, &data);
	if(!code || !data.type || (code >= 999000001u && code <= 999000004u))
		return -3;
	*report = load_inert(pd, code);
	return 0;
}
namespace {
// duel_replace_hidden's work, every container it allocates (in the duel's arena) local to it, so that the caller can
// roll the arena back after they are gone.
int32_t replace_hidden_body(duel* pd, uint8_t playerid, const uint32_t hand[], int32_t n_hand, const uint32_t deck[],
                            int32_t n_deck, const uint32_t facedown[], int32_t n_facedown, const uint32_t extra[],
                            int32_t n_extra) {
	interpreter* lua = pd->lua;
	field* pfield = pd->game_field;
	auto& pl = pfield->player[playerid];
	if((size_t)n_hand != pl.list_hand.size() || (size_t)n_deck != pl.list_main.size())
		return -2;
	struct place {
		card* pc;
		uint32_t code;
		bool extra_kind;
	};
	std::vector<place> places;
	for(int32_t i = 0; i < n_hand; ++i)
		places.push_back({ pl.list_hand[i], hand[i], false });
	for(int32_t i = 0; i < n_deck; ++i)
		places.push_back({ pl.list_main[i], deck[i], false });
	for(int32_t i = 0; i < n_facedown; i += 3) {
		const uint32_t location = facedown[i], sequence = facedown[i + 1];
		card* pc = nullptr;
		if(location == LOCATION_MZONE || location == LOCATION_SZONE)
			pc = pfield->get_field_card(playerid, location, (uint8_t)sequence);
		else if(location == LOCATION_REMOVED && sequence < pl.list_remove.size())
			pc = pl.list_remove[sequence];
		if(!pc || (pc->current.position & POS_FACEUP))
			return -2;
		places.push_back({ pc, facedown[i + 2], pc->is_extra_deck_monster() });
	}
	if(n_extra >= 0) {
		const size_t count = pl.list_extra.size() - pl.extra_p_count;
		if((size_t)n_extra != count)
			return -2;
		for(size_t i = 0; i < count; ++i) {
			card* pc = pl.list_extra[i];
			if(!pc || (pc->current.position & POS_FACEUP))
				return -2;
			places.push_back({ pc, extra[i], true });
		}
	}
	std::set<card*> named;
	for(const auto& p : places)
		if(!named.insert(p.pc).second)
			return -2;  // a place named twice
	std::vector<card_data> datas(places.size());
	for(size_t i = 0; i < places.size(); ++i) {
		::read_card(places[i].code, &datas[i]);
		if(!places[i].code || !datas[i].type || (places[i].code >= 999000001u && places[i].code <= 999000004u)
		   || bool(datas[i].type & TYPES_EXTRA_DECK) != places[i].extra_kind)
			return -3;
	}
	for(const auto& p : places)
		if(!replaceable(pd, p.pc))
			return -4;
	// each place's own duel-level registrations (unguarded per-instance ones; guarded watchers are dormant objects')
	std::map<card*, std::vector<effect*>> duel_level;
	for(effect* peffect : pd->effects)
		if(named.count(peffect->owner) && !peffect->owner->indexer.count(peffect) && !peffect->handler
		   && pfield->effects.indexer.count(peffect))
			duel_level[peffect->owner].push_back(peffect);
	const size_t messages = pd->message_buffer.size();
	for(size_t i = 0; i < places.size(); ++i) {
		card* pc = places[i].pc;
		// the old identity leaves: its own effects (initial ones and its flags), its duel-level registrations and the
		// fields its script set, in registration order (removal can write hints)
		std::vector<effect*> own;
		for(const auto& item : pc->indexer)
			own.push_back(item.first);
		std::sort(own.begin(), own.end(), effect_sort_id);
		for(effect* peffect : own)
			pc->remove_effect(peffect);
		auto& registered = duel_level[pc];
		std::sort(registered.begin(), registered.end(), effect_sort_id);
		for(effect* peffect : registered)
			pfield->remove_effect(peffect);
		pc->unique_code = 0;
		pc->unique_location = 0;
		pc->unique_effect = nullptr;
		pc->spsummon_code = 0;
		pc->q_cache.clear_cache();
		// the new identity comes in where the old one was: same object, place, position and history
		pc->data = datas[i];
		pc->data.code = places[i].code;
		if(!lua->rebind_card(pc))
			return -6;
		if(interpreter::is_load_script(pc->data)) {
			pc->set_status(STATUS_INITIALIZING, TRUE);
			card* outer = pd->initializing;
			pd->initializing = pc;
			lua->add_param(pc, PARAM_TYPE_CARD);
			const bool ok = lua->call_card_function(pc, "initial_effect", 1, 0) != 0;
			pd->initializing = outer;
			pc->set_status(STATUS_INITIALIZING, FALSE);
			if(!ok)
				return -6;
		}
		const auto first = pc->spsummon_counter[0], second = pc->spsummon_counter[1];
		pc->apply_field_effect();
		pc->spsummon_counter[0] = first;
		pc->spsummon_counter[1] = second;
		// a monster set in the spell and trap zone stays the type its EFFECT_MONSTER_SSET names (as hydrate)
		effect* monster_sset = pc->current.location == LOCATION_SZONE && (pc->data.type & TYPE_MONSTER)
		    ? pc->is_affected_by_effect(EFFECT_MONSTER_SSET) : nullptr;
		if(monster_sset) {
			effect* peffect = pd->new_effect();
			peffect->owner = pc;
			peffect->type = EFFECT_TYPE_SINGLE;
			peffect->code = EFFECT_CHANGE_TYPE;
			peffect->reset_flag = RESET_EVENT + 0x1fe0000;
			peffect->value = monster_sset->get_value();
			pc->add_effect(peffect);
		}
	}
	// nothing reaches the players: a replacement that would write a message is refused
	return pd->message_buffer.size() == messages ? 0 : -6;
}
}  // namespace
int32_t duel_replace_hidden(intptr_t pduel, uint8_t playerid, const uint32_t hand[], int32_t n_hand,
    const uint32_t deck[], int32_t n_deck, const uint32_t facedown[], int32_t n_facedown, const uint32_t extra[],
    int32_t n_extra) {
	if(!mfsnap_arena_of(pduel) || playerid > 1 || n_hand < 0 || n_deck < 0 || n_facedown < 0 || n_facedown % 3)
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	interpreter* lua = pd->lua;
	if(!pd->dormant_identities || lua->current_state != lua->lua_state || lua->no_action || !lua->params.empty())
		return -1;
	// the snapshot comes first: even a refusal's checks allocate (in the arena), and every refusal leaves the arena
	// byte-identical
	void* saved = mf_duel_snapshot(pduel);
	if(!saved)
		return -5;
	int32_t result;
	try {
		result = replace_hidden_body(pd, playerid, hand, n_hand, deck, n_deck, facedown, n_facedown, extra, n_extra);
	} catch(...) {
		result = -6;
	}
	if(result != 0 && mf_duel_rollback(pduel, saved) != 0)
		result = -7;
	mf_snapshot_free(saved);
	return result;
}
int32_t duel_hidden_blockers(intptr_t pduel, uint8_t playerid, uint32_t out[], int32_t cap) {
	if(!mfsnap_arena_of(pduel) || playerid > 1 || cap < 0 || (cap && !out))
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	field* pfield = pd->game_field;
	auto& pl = pfield->player[playerid];
	std::vector<card*> cards(pl.list_hand.begin(), pl.list_hand.end());
	cards.insert(cards.end(), pl.list_main.begin(), pl.list_main.end());
	for(auto* zone : { &pl.list_mzone, &pl.list_szone, &pl.list_remove })
		for(card* pc : *zone)
			if(pc && (pc->current.position & POS_FACEDOWN))
				cards.push_back(pc);
	for(size_t i = 0; i + pl.extra_p_count < pl.list_extra.size(); ++i)
		cards.push_back(pl.list_extra[i]);
	if(cards.size() > (size_t)cap)
		return -3;
	for(size_t i = 0; i < cards.size(); ++i)
		out[i] = replace_blockers(pd, cards[i]);
	return (int32_t)cards.size();
}
int32_t query_overlay_owners(intptr_t pduel, uint8_t playerid, uint32_t out[], int32_t cap) {
	if(!mfsnap_arena_of(pduel) || playerid > 1 || cap < 0 || (cap && !out))
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	auto& zone = pd->game_field->player[playerid].list_mzone;
	int32_t n = 0;
	for(size_t sequence = 0; sequence < zone.size(); ++sequence) {
		card* host = zone[sequence];
		if(!host)
			continue;
		for(size_t index = 0; index < host->xyz_materials.size(); ++index) {
			if(4 * (n + 1) > cap)
				return -3;
			card* material = host->xyz_materials[index];
			out[4 * n] = (uint32_t)sequence;
			out[4 * n + 1] = (uint32_t)index;
			out[4 * n + 2] = material->data.code;
			out[4 * n + 3] = material->owner;
			++n;
		}
	}
	return n;
}
int32_t query_target_shortfall(intptr_t pduel, uint32_t* code) {
	if(!mfsnap_arena_of(pduel) || !code)
		return -1;
	duel* pd = (duel*)pduel;
	*code = pd->game_field->core.target_shortfall_code;
	return (int32_t)pd->game_field->core.target_shortfalls;
}
int32_t query_hand_limit(intptr_t pduel, uint8_t playerid) {
	if(!mfsnap_arena_of(pduel) || playerid > 1)
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	int32_t limit = 6;
	effect_set eset;
	pd->game_field->filter_player_effect(playerid, EFFECT_HAND_LIMIT, &eset);
	if(eset.size())
		limit = eset.back()->get_value();
	return limit;
}
int32_t query_activation_flags(intptr_t pduel, int32_t index, uint32_t out[2]) {
	if(!mfsnap_arena_of(pduel) || !out)
		return -1;
	duel* pd = (duel*)pduel;
	auto& core = pd->game_field->core;
	out[0] = out[1] = 0;
	if(core.units.empty() || index < 0 || (size_t)index >= core.select_chains.size())
		return 0;
	const processor_unit& unit = core.units.front();
	bool asks = unit.type == PROCESSOR_SELECT_IDLECMD || unit.type == PROCESSOR_SELECT_BATTLECMD
		|| unit.type == PROCESSOR_SELECT_CHAIN;
	// the single optional trigger (processor.cpp: the trigger timing's and the chain window's yes/no, descriptions
	// 0 and 221), whose effect is select_chains[0]
	if(unit.type == PROCESSOR_SELECT_EFFECTYN && index == 0 && core.select_chains.size() == 1
		&& (unit.arg2 == 0 || unit.arg2 == 221)
		&& (card*)unit.ptarget == core.select_chains[0].triggering_effect->get_handler())
		asks = true;
	if(!asks)
		return 0;
	effect* peffect = core.select_chains[index].triggering_effect;
	out[0] = peffect->get_handler()->data.code;
	out[1] = peffect->description;
	return 1 | (peffect->is_flag(EFFECT_FLAG_CARD_TARGET) ? 2 : 0) | (peffect->cost ? 4 : 0) | (peffect->target ? 8 : 0)
		| (peffect->reads_cost_check ? 16 : 0);
}
int32_t duel_collect_garbage(intptr_t pduel) {
	if(!mfsnap_arena_of(pduel))
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	interpreter* lua = pd->lua;
	if(lua->current_state != lua->lua_state || lua->no_action || !lua->params.empty() || pd->releasing_groups)
		return -1;
	lua_gc(lua->lua_state, LUA_GCCOLLECT, 0);
	return 0;
}
int32_t duel_arena_digest(intptr_t pduel, uint64_t* digest) {
	const size_t extent = mf_duel_arena_extent(pduel);
	void* snap = extent ? mf_duel_snapshot(pduel) : nullptr;
	if(!snap || !digest)
		return -1;
	// FNV-1a over the used extent (the snapshot's bytes follow its length word)
	const byte* bytes = static_cast<const byte*>(snap) + sizeof(size_t);
	const size_t len = *static_cast<const size_t*>(snap);
	uint64_t h = 1469598103934665603ull;
	for(size_t i = 0; i < len; ++i) {
		h ^= bytes[i];
		h *= 1099511628211ull;
	}
	mf_snapshot_free(snap);
	*digest = h;
	return 0;
}
int32_t duel_blank_card(intptr_t pduel, uint8_t playerid, uint8_t location,
    uint8_t sequence, uint32_t expected_code, uint32_t placeholder) {
	if(!mfsnap_arena_of(pduel) || playerid > 1)
		return -1;
	MfArenaGuard guard(pduel);
	duel* pd = (duel*)pduel;
	interpreter* lua = pd->lua;
	if(lua->current_state != lua->lua_state || lua->no_action || !lua->params.empty())
		return -1;
	// A set proxy (999000003, 999000004) stands for a card set face down in the spell and trap zones only: back
	// in the deck or the hand it becomes the main-deck blank again. Any other placeholder is already one.
	const bool proxy = expected_code == 999000003u || expected_code == 999000004u;
	if(placeholder < 999000001u || placeholder > 999000004u
	   || (expected_code >= 999000001u && expected_code <= 999000004u
	       && !(proxy && (location == LOCATION_DECK || location == LOCATION_HAND) && placeholder == 999000001u)))
		return -2;
	card* pc = nullptr;
	if(location == LOCATION_MZONE || location == LOCATION_SZONE)
		pc = pd->game_field->get_field_card(playerid, location, sequence);
	else if(location == LOCATION_DECK || location == LOCATION_HAND || location == LOCATION_EXTRA
	        || location == LOCATION_REMOVED) {
		auto* cards = pd->game_field->get_field_vector(playerid, location);
		if(cards && sequence < cards->size())
			pc = (*cards)[sequence];
	}
	if(!pc || pc->data.code != expected_code
	   || ((location & (LOCATION_ONFIELD | LOCATION_REMOVED | LOCATION_EXTRA))
	       && (pc->current.position & POS_FACEUP)))
		return -2;
	// The placeholder keeps the card's pool: the extra deck blank for an extra-deck monster, the set proxies
	// for the spell/trap and field zones, the main-deck blank everywhere else.
	const bool extra = pc->data.type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK);
	const uint32_t wanted = extra ? 999000002u
	    : location == LOCATION_SZONE ? (sequence == 5 ? 999000004u : 999000003u) : 999000001u;
	if(placeholder != wanted)
		return -2;
	if(!pc->counters.empty() || (pc->unique_code && (pc->current.location & pc->unique_location)))
		return -2;
	// No pending choice, chain or trigger may still hold one of this card's effects.
	auto& core = pd->game_field->core;
	auto held = [pc](const chain& link) { return link.triggering_effect && link.triggering_effect->get_handler() == pc; };
	for(const chain_array* links : { &core.select_chains, &core.current_chain, &core.ignition_priority_chains })
		for(const auto& link : *links)
			if(held(link))
				return -2;
	for(const chain_list* links : { &core.continuous_chain, &core.solving_continuous, &core.sub_solving_continuous,
	                                &core.delayed_continuous_tp, &core.delayed_continuous_ntp, &core.desrep_chain,
	                                &core.new_fchain, &core.new_fchain_s, &core.new_ochain, &core.new_ochain_s,
	                                &core.new_fchain_b, &core.new_ochain_b, &core.new_ochain_h, &core.new_chains })
		for(const auto& link : *links)
			if(held(link))
				return -2;
	for(effect* peffect : core.select_effects)
		if(peffect && peffect->get_handler() == pc)
			return -2;
	for(const auto* delayed : { &core.delayed_quick_tmp, &core.delayed_quick })
		for(const auto& item : *delayed)
			if(item.first && item.first->get_handler() == pc)
				return -2;
	for(const auto& item : core.quick_f_chain)
		if(item.first && item.first->get_handler() == pc)
			return -2;
	card_data data;
	::read_card(placeholder, &data);
	if(!data.type)
		return -3;
	void* saved = mf_duel_snapshot(pduel);
	if(!saved)
		return -4;
	bool ok = true;
	try {
		std::vector<effect*> own;
		for(const auto& item : pc->indexer)
			if(item.first->owner == pc && item.first->is_flag(EFFECT_FLAG_INITIAL))
				own.push_back(item.first);
		std::sort(own.begin(), own.end(), effect_sort_id);  // removal writes hints: registration order
		// Removed like card::replace_effect removes a card's initial effects: the objects stay allocated,
		// since other records may still refer to them.
		for(effect* peffect : own)
			pc->remove_effect(peffect);
		pc->unique_code = 0;
		pc->unique_location = 0;
		pc->unique_effect = nullptr;
		// Set by the identity's script (SetSPSummonOnce), not by an effect: another identity hydrated later must
		// not count its special summons under this one's name.
		pc->spsummon_code = 0;
		pc->data = data;
		pc->data.code = placeholder;
		ok = lua->rebind_card(pc);
	} catch(...) {
		ok = false;
	}
	const int result = ok ? 0 : (mf_duel_rollback(pduel, saved) == 0 ? -5 : -6);
	mf_snapshot_free(saved);
	return result;
}
static duel* forced_random_duel(intptr_t pduel) {
	if(!mfsnap_arena_of(pduel))
		return nullptr;
	duel* pd = (duel*)pduel;
	interpreter* lua = pd->lua;
	if(lua->current_state != lua->lua_state || lua->no_action || !lua->params.empty())
		return nullptr;
	return pd;
}
int32_t duel_tactical_audit_begin(intptr_t pduel, const uint64_t* uids, int32_t count) {
	if(!live_duel(pduel))
		return -1;
	duel* pd = forced_random_duel(pduel);
	if(!pd)
		return -1;
	if(count < 0 || count > (int32_t)mf_tactical_audit::CAPACITY || (count && !uids))
		return -2;
	if(pd->tactical_audit.enabled)
		return -3;
	mf_tactical_audit next;
	next.count = (uint32_t)count;
	if(count)
		std::copy(uids, uids + count, next.uids);
	std::sort(next.uids, next.uids + count);
	for(int32_t i = 0; i < count; ++i) {
		if(!next.uids[i] || (i && next.uids[i] == next.uids[i - 1]))
			return -2;
		bool found = false;
		for(const card* value : pd->cards)
			if(value->cardid == next.uids[i]) {
				found = true;
				break;
			}
		if(!found)
			return -4;
	}
	next.enabled = true;
	pd->tactical_audit = next;
	return 0;
}
int32_t duel_tactical_audit_state(intptr_t pduel, uint64_t* out, int32_t capacity) {
	if(!live_duel(pduel))
		return -1;
	duel* pd = forced_random_duel(pduel);
	if(!pd)
		return -1;
	if(!out || capacity < 8)
		return -2;
	const auto& value = pd->tactical_audit;
	out[0] = 1;
	out[1] = value.enabled ? 1 : 0;
	out[2] = value.count;
	std::copy(value.hits, value.hits + mf_tactical_audit::COUNTERS, out + 3);
	return 0;
}
int32_t duel_tactical_audit_end(intptr_t pduel) {
	if(!live_duel(pduel))
		return -1;
	duel* pd = forced_random_duel(pduel);
	if(!pd)
		return -1;
	if(!pd->tactical_audit.enabled)
		return -3;
	pd->tactical_audit.enabled = false;
	return 0;
}
int32_t duel_tactical_audit_begin_v2(intptr_t pduel, const uint64_t* uids, int32_t count, int32_t order_player) {
	if(order_player != 0 && order_player != 1)
		return -2;
	const int32_t status = duel_tactical_audit_begin(pduel, uids, count);
	if(status)
		return status;
	((duel*)pduel)->tactical_audit.order_player = order_player;
	return 0;
}
int32_t duel_tactical_audit_state_v2(intptr_t pduel, uint64_t* out, int32_t capacity) {
	if(!live_duel(pduel))
		return -1;
	duel* pd = forced_random_duel(pduel);
	if(!pd)
		return -1;
	if(!out || capacity < 12)
		return -2;
	const auto& value = pd->tactical_audit;
	out[0] = 2;
	out[1] = value.enabled ? 1 : 0;
	out[2] = value.count;
	out[3] = (uint64_t)(value.order_player + 1);
	std::copy(value.hits, value.hits + mf_tactical_audit::COUNTERS, out + 4);
	out[9] = value.order_reads;
	out[10] = value.own_deck_shuffles;
	out[11] = value.other_random;
	return 0;
}
int32_t duel_force_random_outcomes(intptr_t pduel, int32_t count, const uint8_t* values) {
	duel* pd = forced_random_duel(pduel);
	if(!pd || count < 0 || (count && !values))
		return -1;
	MfArenaGuard guard(pduel);
	if(pd->forced_outcome_count + (uint32_t)count > duel::FORCED_OUTCOME_CAPACITY)
		return -2;
	for(int32_t i = 0; i < count; ++i)
		pd->forced_outcomes[pd->forced_outcome_count++] = values[i];
	return 0;
}
int32_t duel_force_random_select(intptr_t pduel, int32_t count, const uint32_t* locations, const uint32_t* codes) {
	duel* pd = forced_random_duel(pduel);
	if(!pd || count <= 0 || count > 8 || !locations)
		return -1;
	MfArenaGuard guard(pduel);
	if(pd->forced_select_count >= duel::FORCED_SELECT_CAPACITY)
		return -2;
	duel::forced_select& entry = pd->forced_selects[pd->forced_select_count++];
	entry.count = (uint32_t)count;
	for(int32_t i = 0; i < count; ++i) {
		entry.locations[i] = locations[i];
		entry.codes[i] = codes ? codes[i] : 0;
	}
	return 0;
}
int32_t duel_forced_random_state(intptr_t pduel, int32_t* state) {
	duel* pd = forced_random_duel(pduel);
	if(!pd || !state)
		return -1;
	state[0] = (int32_t)(pd->forced_outcome_count - pd->forced_outcome_head);
	state[1] = (int32_t)(pd->forced_select_count - pd->forced_select_head);
	state[2] = (int32_t)pd->forced_random_misses;
	return 0;
}
int32_t duel_force_activation(intptr_t pduel, uint8_t playerid, uint32_t code, uint32_t description) {
	duel* pd = forced_random_duel(pduel);
	if(!pd || playerid > 1)
		return -1;
	MfArenaGuard guard(pduel);
	for(uint32_t i = 0; i < pd->forced_activation_count; ++i) {
		const duel::forced_activation& forced = pd->forced_activations[i];
		if(forced.code == code && forced.description == description && forced.player == playerid)
			return 0;  // already pending: one mark names the activation
	}
	if(pd->forced_activation_count >= duel::FORCED_ACTIVATION_CAPACITY)
		return -3;
	pd->forced_activations[pd->forced_activation_count++] = { code, description, playerid };
	return 0;
}
int32_t duel_forced_activation_state(intptr_t pduel, int32_t* state) {
	duel* pd = forced_random_duel(pduel);
	if(!pd || !state)
		return -1;
	MfArenaGuard guard(pduel);
	state[0] = (int32_t)pd->forced_activation_count;
	state[1] = (int32_t)pd->forced_link_count;
	return 0;
}
int32_t duel_clear_forced_activations(intptr_t pduel) {
	duel* pd = forced_random_duel(pduel);
	if(!pd)
		return -1;
	MfArenaGuard guard(pduel);
	const int32_t pending = (int32_t)pd->forced_activation_count;
	for(uint32_t i = 0; i < pd->forced_activation_count; ++i)
		pd->forced_activations[i] = {};
	pd->forced_activation_count = 0;
	return pending;
}
int32_t duel_clear_forced_random(intptr_t pduel) {
	duel* pd = forced_random_duel(pduel);
	if(!pd)
		return -1;
	MfArenaGuard guard(pduel);
	pd->forced_outcome_head = pd->forced_outcome_count = 0;
	pd->forced_select_head = pd->forced_select_count = 0;
	pd->forced_random_misses = 0;
	return 0;
}
void start_duel(intptr_t pduel, uint32_t options) {
	MfArenaGuard mf_guard_(pduel);
	duel* pd = (duel*)pduel;
	uint16_t duel_rule = options >> 16;
	uint16_t duel_options = options & 0xffff;
	pd->game_field->core.duel_options |= duel_options;
	if (duel_rule >= 1 && duel_rule <= CURRENT_RULE)
		pd->game_field->core.duel_rule = duel_rule;
	else if(options & DUEL_OBSOLETE_RULING)		//provide backward compatibility with replay
		pd->game_field->core.duel_rule = 1;
	if (pd->game_field->core.duel_rule < 1 || pd->game_field->core.duel_rule > CURRENT_RULE)
		pd->game_field->core.duel_rule = CURRENT_RULE;
	if (pd->game_field->core.duel_rule == MASTER_RULE3) {
		pd->game_field->player[0].szone_size = 8;
		pd->game_field->player[1].szone_size = 8;
	}
	pd->game_field->core.shuffle_hand_check[0] = FALSE;
	pd->game_field->core.shuffle_hand_check[1] = FALSE;
	pd->game_field->core.shuffle_deck_check[0] = FALSE;
	pd->game_field->core.shuffle_deck_check[1] = FALSE;
	if(pd->game_field->player[0].start_count > 0)
		pd->game_field->draw(0, REASON_RULE, PLAYER_NONE, 0, pd->game_field->player[0].start_count);
	if(pd->game_field->player[1].start_count > 0)
		pd->game_field->draw(0, REASON_RULE, PLAYER_NONE, 1, pd->game_field->player[1].start_count);
	if(options & DUEL_TAG_MODE) {
		for(int i = 0; i < pd->game_field->player[0].start_count && pd->game_field->player[0].tag_list_main.size(); ++i) {
			card* pcard = pd->game_field->player[0].tag_list_main.back();
			pd->game_field->player[0].tag_list_main.pop_back();
			pd->game_field->player[0].tag_list_hand.push_back(pcard);
			pcard->current.controler = 0;
			pcard->current.location = LOCATION_HAND;
			pcard->current.sequence = (uint8_t)pd->game_field->player[0].tag_list_hand.size() - 1;
			pcard->current.position = POS_FACEDOWN;
		}
		for(int i = 0; i < pd->game_field->player[1].start_count && pd->game_field->player[1].tag_list_main.size(); ++i) {
			card* pcard = pd->game_field->player[1].tag_list_main.back();
			pd->game_field->player[1].tag_list_main.pop_back();
			pd->game_field->player[1].tag_list_hand.push_back(pcard);
			pcard->current.controler = 1;
			pcard->current.location = LOCATION_HAND;
			pcard->current.sequence = (uint8_t)pd->game_field->player[1].tag_list_hand.size() - 1;
			pcard->current.position = POS_FACEDOWN;
		}
	}
	pd->game_field->add_process(PROCESSOR_TURN, 0, 0, 0, 0, 0);
}
#ifdef MF_GROUP_AUDIT
// Diagnostic builds: one line per ended duel, appended to MF_CORE_AUDIT_REPORT when set -- how often the holds
// audit ran, the full collections the weak-group debt forced, groups marked and deleted, weak slots issued.
static void mf_group_audit_report(const duel* pd) {
	const char* path = std::getenv("MF_CORE_AUDIT_REPORT");
	if(!path)
		return;
	char line[256];
	const int n = std::snprintf(line, sizeof line,
	    "MFGROUPAUDIT audits=%llu full_collections=%llu marked=%llu deleted=%llu weak_slots=%d pressure=%s\n",
	    (unsigned long long)pd->group_audits, (unsigned long long)pd->group_full_collections,
	    (unsigned long long)pd->groups_marked, (unsigned long long)pd->groups_collected,
	    (int)(pd->lua->weak_group_next - 1), std::getenv("MF_CORE_GC_PRESSURE") ? std::getenv("MF_CORE_GC_PRESSURE") : "none");
	const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);  // one short write: whole lines across threads
	if(fd < 0 || n <= 0 || ::write(fd, line, (size_t)n) != n)
		mf_core_refusal("group_audit", "MF_CORE_AUDIT_REPORT could not be appended");
	::close(fd);
}
#endif
void end_duel(intptr_t pduel) {
	duel* pd = (duel*)pduel;
	{
		std::unique_lock<std::shared_mutex> lock(duel_set_mutex);
		if(!duel_set.erase(pd))
			return;
	}
	{
		MfArenaGuard guard(pduel);
#ifdef MF_GROUP_AUDIT
		mf_group_audit_report(pd);
#endif
		delete pd;
	}
	void* arena = mfsnap_arena_of(pduel);
	{
		std::unique_lock<std::shared_mutex> lock(duel_set_mutex);
		mfsnap_unregister_duel(pduel);
	}
	mfsnap_arena_destroy(arena);
}
void set_player_info(intptr_t pduel, int32_t playerid, int32_t lp, int32_t startcount, int32_t drawcount) {
	MfArenaGuard mf_guard_(pduel);
	if (!check_playerid(playerid))
		return;
	duel* pd = (duel*)pduel;
	if(lp > 0)
		pd->game_field->player[playerid].lp = lp;
	if(startcount >= 0)
		pd->game_field->player[playerid].start_count = startcount;
	if(drawcount >= 0)
		pd->game_field->player[playerid].draw_count = drawcount;
}
void get_log_message(intptr_t pduel, char* buf) {
	MfArenaGuard mf_guard_(pduel);
	duel* pd = (duel*)pduel;
	std::strncpy(buf, pd->strbuffer, sizeof pd->strbuffer - 1);
	buf[sizeof pd->strbuffer - 1] = 0;
}
int32_t get_message(intptr_t pduel, byte* buf) {
	MfArenaGuard mf_guard_(pduel);
	int32_t len = ((duel*)pduel)->read_buffer(buf);
	((duel*)pduel)->clear_buffer();
	return len;
}
uint32_t process(intptr_t pduel) {
	MfArenaGuard mf_guard_(pduel);
	duel* pd = (duel*)pduel;
	if(pd->shuffle_plan.fault)
		return PROCESSOR_REPLAY_ERROR;
	pd->phase_pass_audit_process_seen = true;
	if(pd->phase_pass_audit_enabled) {
		++pd->phase_pass_audit_batch;
		pd->phase_pass_audit_count = 0;
		pd->phase_pass_audit_overflow = false;
	}
	uint32_t result = 0; 
	do {
		result = pd->game_field->process();
		if(pd->shuffle_plan.fault)
			return PROCESSOR_REPLAY_ERROR;  // partial messages are never a successful batch
	} while ((result & PROCESSOR_BUFFER_LEN) == 0 && (result & PROCESSOR_FLAG) == 0);
#ifdef MF_GROUP_AUDIT
	char what[160];
	++pd->group_audits;
	if(pd->audit_group_holds(what, sizeof what))
		mf_core_refusal("group_audit", what);
#endif
	return result;
}
void new_card(intptr_t pduel, uint32_t code, uint8_t owner, uint8_t playerid, uint8_t location, uint8_t sequence, uint8_t position) {
	MfArenaGuard mf_guard_(pduel);
	if (!check_playerid(owner) || !check_playerid(playerid))
		return;
	duel* ptduel = (duel*)pduel;
	if(ptduel->game_field->is_location_useable(playerid, location, sequence)) {
		card* pcard = ptduel->new_card(code);
		pcard->owner = owner;
		ptduel->game_field->add_card(playerid, pcard, location, sequence);
		pcard->current.position = position;
		if(!(location & LOCATION_ONFIELD) || (position & POS_FACEUP)) {
			pcard->enable_field_effect(true);
			ptduel->game_field->adjust_instant();
		} if(location & LOCATION_ONFIELD) {
			if(location == LOCATION_MZONE)
				pcard->set_status(STATUS_PROC_COMPLETE, TRUE);
		}
	}
}
void new_tag_card(intptr_t pduel, uint32_t code, uint8_t owner, uint8_t location) {
	MfArenaGuard mf_guard_(pduel);
	duel* ptduel = (duel*)pduel;
	if(owner > 1 || !(location & (LOCATION_DECK | LOCATION_EXTRA)))
		return;
	card* pcard = ptduel->new_card(code);
	switch(location) {
	case LOCATION_DECK:
		ptduel->game_field->player[owner].tag_list_main.push_back(pcard);
		pcard->owner = owner;
		pcard->current.controler = owner;
		pcard->current.location = LOCATION_DECK;
		pcard->current.sequence = (uint8_t)ptduel->game_field->player[owner].tag_list_main.size() - 1;
		pcard->current.position = POS_FACEDOWN_DEFENSE;
		break;
	case LOCATION_EXTRA:
		ptduel->game_field->player[owner].tag_list_extra.push_back(pcard);
		pcard->owner = owner;
		pcard->current.controler = owner;
		pcard->current.location = LOCATION_EXTRA;
		pcard->current.sequence = (uint8_t)ptduel->game_field->player[owner].tag_list_extra.size() - 1;
		pcard->current.position = POS_FACEDOWN_DEFENSE;
		break;
	}
}
/**
* @brief Get card information.
* @param buf int32_t array
* @return buffer length in bytes
*/
int32_t query_card(intptr_t pduel, uint8_t playerid, uint8_t location, uint8_t sequence, uint32_t query_flag, byte* buf, int32_t use_cache) {
	MfArenaGuard mf_guard_(pduel);
	if (!check_playerid(playerid))
		return LEN_FAIL;
	duel* ptduel = (duel*)pduel;
	card* pcard = nullptr;
	location &= 0x7f;
	if (location == LOCATION_MZONE || location == LOCATION_SZONE)
		pcard = ptduel->game_field->get_field_card(playerid, location, sequence);
	else {
		auto ptr = ptduel->game_field->get_field_vector(playerid, location);
		if(!ptr)
			return LEN_FAIL;
		auto& lst = *ptr;
		if (sequence >= lst.size())
			return LEN_FAIL;
		pcard = lst[sequence];
	}
	if (pcard) {
		return pcard->get_infos(buf, query_flag, use_cache);
	}
	else {
		buffer_write<int32_t>(buf, LEN_EMPTY);
		return LEN_EMPTY;
	}
}
int32_t query_field_count(intptr_t pduel, uint8_t playerid, uint8_t location) {
	MfArenaGuard mf_guard_(pduel);
	duel* ptduel = (duel*)pduel;
	if (!check_playerid(playerid))
		return 0;
	auto& player = ptduel->game_field->player[playerid];
	if(location == LOCATION_HAND)
		return (int32_t)player.list_hand.size();
	if(location == LOCATION_GRAVE)
		return (int32_t)player.list_grave.size();
	if(location == LOCATION_REMOVED)
		return (int32_t)player.list_remove.size();
	if(location == LOCATION_EXTRA)
		return (int32_t)player.list_extra.size();
	if(location == LOCATION_DECK)
		return (int32_t)player.list_main.size();
	if(location == LOCATION_MZONE) {
		int32_t count = 0;
		for(auto& pcard : player.list_mzone)
			if(pcard)
				++count;
		return count;
	}
	if(location == LOCATION_SZONE) {
		int32_t count = 0;
		for(auto& pcard : player.list_szone)
			if(pcard)
				++count;
		return count;
	}
	return 0;
}
int32_t query_field_card(intptr_t pduel, uint8_t playerid, uint8_t location, uint32_t query_flag, byte* buf, int32_t use_cache) {
	MfArenaGuard mf_guard_(pduel);
	if (!check_playerid(playerid))
		return LEN_FAIL;
	duel* ptduel = (duel*)pduel;
	auto& player = ptduel->game_field->player[playerid];
	byte* p = buf;
	if(location == LOCATION_MZONE) {
		for(auto& pcard : player.list_mzone) {
			if(pcard) {
				int32_t clen = pcard->get_infos(p, query_flag, use_cache);
				p += clen;
			} else {
				buffer_write<int32_t>(p, LEN_EMPTY);
			}
		}
	}
	else if(location == LOCATION_SZONE) {
		for(auto& pcard : player.list_szone) {
			if(pcard) {
				int32_t clen = pcard->get_infos(p, query_flag, use_cache);
				p += clen;
			} else {
				buffer_write<int32_t>(p, LEN_EMPTY);
			}
		}
	}
	else {
		card_vector* lst = nullptr;
		if(location == LOCATION_HAND)
			lst = &player.list_hand;
		else if(location == LOCATION_GRAVE)
			lst = &player.list_grave;
		else if(location == LOCATION_REMOVED)
			lst = &player.list_remove;
		else if(location == LOCATION_EXTRA)
			lst = &player.list_extra;
		else if(location == LOCATION_DECK)
			lst = &player.list_main;
		else
			return LEN_FAIL;
		for(auto& pcard : *lst) {
			int32_t clen = pcard->get_infos(p, query_flag, use_cache);
			p += clen;
		}
	}
	return (int32_t)(p - buf);
}
int32_t query_field_info(intptr_t pduel, byte* buf) {
	MfArenaGuard mf_guard_(pduel);
	duel* ptduel = (duel*)pduel;
	byte* p = buf;
	*p++ = MSG_RELOAD_FIELD;
	*p++ = (uint8_t)ptduel->game_field->core.duel_rule;
	for(int playerid = 0; playerid < 2; ++playerid) {
		auto& player = ptduel->game_field->player[playerid];
		buffer_write<int32_t>(p, player.lp);
		for(auto& pcard : player.list_mzone) {
			if(pcard) {
				*p++ = 1;
				*p++ = pcard->current.position;
				*p++ = (uint8_t)pcard->xyz_materials.size();
			} else {
				*p++ = 0;
			}
		}
		for(auto& pcard : player.list_szone) {
			if(pcard) {
				*p++ = 1;
				*p++ = pcard->current.position;
			} else {
				*p++ = 0;
			}
		}
		*p++ = (uint8_t)player.list_main.size();
		*p++ = (uint8_t)player.list_hand.size();
		*p++ = (uint8_t)player.list_grave.size();
		*p++ = (uint8_t)player.list_remove.size();
		*p++ = (uint8_t)player.list_extra.size();
		*p++ = (uint8_t)player.extra_p_count;
	}
	*p++ = (uint8_t)ptduel->game_field->core.current_chain.size();
	for(const auto& ch : ptduel->game_field->core.current_chain) {
		effect* peffect = ch.triggering_effect;
		buffer_write<uint32_t>(p, peffect->get_handler()->data.code);
		buffer_write<uint32_t>(p, peffect->get_handler()->get_info_location());
		*p++ = ch.triggering_controler;
		*p++ = (uint8_t)ch.triggering_location;
		*p++ = ch.triggering_sequence;
		buffer_write<uint32_t>(p, peffect->description);
	}
	return (int32_t)(p - buf);
}
namespace {

struct effect_info_entry {
	effect* peffect;
	uint8_t container;
};

// Every variable-length section declares its own record width, and readers
// step by that width rather than by a compiled-in constant.  The declared
// width is measured from what was actually written, so adding a field to a
// record cannot leave the two disagreeing.
void effect_info_patch_record_len(std::vector<byte>& out, size_t body_start, size_t count) {
	if(!count)
		return;
	uint16_t measured = (uint16_t)((out.size() - body_start) / count);
	std::memcpy(out.data() + body_start - sizeof(uint16_t), &measured, sizeof(measured));
}

void effect_info_collect_field(const effect_container& src, uint8_t container,
			std::vector<effect_info_entry>& out, std::set<effect*>& seen) {
	for(const auto& it : src) {
		effect* peffect = it.second;
		// EFFECT_FLAG_FIELD_ONLY marks the registered effects with no on-field
		// carrier -- the player-scope state the world model cannot otherwise
		// see.  Field effects whose source card is on the board are skipped:
		// the board already carries that truth.
		if(peffect && peffect->owner && peffect->is_flag(EFFECT_FLAG_FIELD_ONLY)
				&& seen.insert(peffect).second)
			out.push_back(effect_info_entry{ peffect, container });
	}
}

void effect_info_collect_card(const effect_container& src, uint8_t container,
			std::vector<effect_info_entry>& out, std::set<effect*>& seen) {
	for(const auto& it : src) {
		effect* peffect = it.second;
		// Initial effects are the card script itself, already represented by the
		// card/effect embedding.  Non-initial rows are runtime state: temporary
		// disables, grants, copies and other effects registered by resolution.
		if(peffect && peffect->owner && !peffect->is_flag(EFFECT_FLAG_INITIAL)
				&& seen.insert(peffect).second)
			out.push_back(effect_info_entry{ peffect, container });
	}
}

void effect_info_write_effects(std::vector<byte>& out, field* pfield,
			const std::vector<card*>& cards) {
	std::vector<effect_info_entry> entries;
	std::set<effect*> seen;
	effect_info_collect_field(pfield->effects.aura_effect, EFFECT_INFO_CONTAINER_AURA, entries, seen);
	effect_info_collect_field(pfield->effects.ignition_effect, EFFECT_INFO_CONTAINER_IGNITION, entries, seen);
	effect_info_collect_field(pfield->effects.activate_effect, EFFECT_INFO_CONTAINER_ACTIVATE, entries, seen);
	effect_info_collect_field(pfield->effects.trigger_o_effect, EFFECT_INFO_CONTAINER_TRIGGER_O, entries, seen);
	effect_info_collect_field(pfield->effects.trigger_f_effect, EFFECT_INFO_CONTAINER_TRIGGER_F, entries, seen);
	effect_info_collect_field(pfield->effects.quick_o_effect, EFFECT_INFO_CONTAINER_QUICK_O, entries, seen);
	effect_info_collect_field(pfield->effects.quick_f_effect, EFFECT_INFO_CONTAINER_QUICK_F, entries, seen);
	effect_info_collect_field(pfield->effects.continuous_effect, EFFECT_INFO_CONTAINER_CONTINUOUS, entries, seen);
	for(card* pcard : cards) {
		effect_info_collect_card(pcard->single_effect, EFFECT_INFO_CONTAINER_CARD_SINGLE, entries, seen);
		effect_info_collect_card(pcard->field_effect, EFFECT_INFO_CONTAINER_CARD_FIELD, entries, seen);
		effect_info_collect_card(pcard->equip_effect, EFFECT_INFO_CONTAINER_CARD_EQUIP, entries, seen);
		effect_info_collect_card(pcard->target_effect, EFFECT_INFO_CONTAINER_CARD_TARGET, entries, seen);
		effect_info_collect_card(pcard->xmaterial_effect, EFFECT_INFO_CONTAINER_CARD_XMATERIAL, entries, seen);
	}
	// registration order: effect::id comes from infos.field_id++ in
	// field::add_effect, so it is unique, monotonic and reproducible across
	// re-runs of the same duel -- unlike the container's own order, which is
	// by effect code
	std::sort(entries.begin(), entries.end(), [](const effect_info_entry& a, const effect_info_entry& b) {
		if(a.peffect->id != b.peffect->id)
			return a.peffect->id < b.peffect->id;
		return a.container < b.container;
	});
	vector_write<uint16_t>(out, (uint16_t)entries.size());
	vector_write<uint16_t>(out, (uint16_t)EFFECT_INFO_RECORD_LEN);
	size_t body_start = out.size();
	for(const auto& entry : entries) {
		effect* peffect = entry.peffect;
		card* handler = peffect->get_handler();
		uint32_t label_object_type = 0;
		uint32_t label_object_info = 0;
		uint32_t label_object_code = 0;
		if(peffect->label_object && peffect->object_type == PARAM_TYPE_CARD) {
			for(card* object : cards) {
				if(object->ref_handle == peffect->label_object) {
					label_object_type = PARAM_TYPE_CARD;
					label_object_info = object->get_info_location();
					label_object_code = object->data.code;
					break;
				}
			}
		}
		uint8_t target_player = 0;
		if(peffect->is_target_player(0))
			target_player |= 0x1;
		if(peffect->is_target_player(1))
			target_player |= 0x2;
		vector_write<uint32_t>(out, peffect->id);
		vector_write<uint32_t>(out, peffect->code);
		vector_write<uint32_t>(out, peffect->type);
		vector_write<uint32_t>(out, peffect->owner->data.code);
		vector_write<uint32_t>(out, peffect->owner->get_info_location());
		vector_write<uint32_t>(out, peffect->description);
		vector_write<uint32_t>(out, peffect->reset_flag);
		vector_write<int32_t>(out, peffect->reset_count);
		vector_write<uint32_t>(out, peffect->count_code);
		vector_write<uint32_t>(out, peffect->active_type);
		vector_write<uint64_t>(out, peffect->flag[0]);
		vector_write<uint64_t>(out, peffect->flag[1]);
		vector_write<uint16_t>(out, peffect->s_range);
		vector_write<uint16_t>(out, peffect->o_range);
		vector_write<uint16_t>(out, peffect->range);
		vector_write<uint16_t>(out, peffect->status);
		vector_write<uint8_t>(out, entry.container);
		vector_write<uint8_t>(out, peffect->effect_owner);
		vector_write<uint8_t>(out, target_player);
		vector_write<uint8_t>(out, peffect->count_limit);
		vector_write<uint8_t>(out, peffect->count_limit_max);
		vector_write<uint8_t>(out, (uint8_t)std::min<size_t>(peffect->label.size(), 255));
		vector_write<uint16_t>(out, 0);
		vector_write<uint32_t>(out, handler ? handler->get_info_location() : 0);
		vector_write<uint32_t>(out, handler ? handler->data.code : 0);
		vector_write<uint32_t>(out, label_object_type);
		vector_write<uint32_t>(out, label_object_info);
		vector_write<uint32_t>(out, label_object_code);
	}
	effect_info_patch_record_len(out, body_start, entries.size());
}

void effect_info_write_count_code(std::vector<byte>& out, field* pfield) {
	struct count_entry {
		uint8_t table;
		uint8_t slot;
		uint32_t code;
		int32_t value;
	};
	std::vector<count_entry> entries;
	const std::unordered_map<uint32_t, int32_t>* tables[3][3] = {
		{ &pfield->core.effect_count_code[0], &pfield->core.effect_count_code[1], &pfield->core.effect_count_code[2] },
		{ &pfield->core.effect_count_code_duel[0], &pfield->core.effect_count_code_duel[1], &pfield->core.effect_count_code_duel[2] },
		{ &pfield->core.effect_count_code_chain[0], &pfield->core.effect_count_code_chain[1], &pfield->core.effect_count_code_chain[2] },
	};
	for(uint8_t table = 0; table < 3; ++table)
		for(uint8_t slot = 0; slot < 3; ++slot)
			for(const auto& it : *tables[table][slot])
				entries.push_back(count_entry{ table, slot, it.first, it.second });
	// the tables are unordered_maps; sort so the dump is reproducible
	std::sort(entries.begin(), entries.end(), [](const count_entry& a, const count_entry& b) {
		if(a.table != b.table)
			return a.table < b.table;
		if(a.slot != b.slot)
			return a.slot < b.slot;
		return a.code < b.code;
	});
	vector_write<uint16_t>(out, (uint16_t)entries.size());
	vector_write<uint16_t>(out, 10);
	size_t body_start = out.size();
	for(const auto& entry : entries) {
		vector_write<uint8_t>(out, entry.table);
		vector_write<uint8_t>(out, entry.slot);
		vector_write<uint32_t>(out, entry.code);
		vector_write<int32_t>(out, entry.value);
	}
	effect_info_patch_record_len(out, body_start, entries.size());
}

void effect_info_write_turn(std::vector<byte>& out, field* pfield) {
	vector_write<uint16_t>(out, pfield->infos.turn_id);
	vector_write<uint16_t>(out, pfield->infos.phase);
	vector_write<uint8_t>(out, pfield->infos.turn_player);
	vector_write<uint8_t>(out, pfield->core.current_player);
	vector_write<uint8_t>(out, (uint8_t)pfield->core.duel_rule);
	vector_write<uint8_t>(out, pfield->core.deck_reversed);
	vector_write<uint8_t>(out, pfield->core.remove_brainwashing);
	vector_write<uint8_t>(out, 0);
	vector_write<uint16_t>(out, pfield->infos.turn_id_by_player[0]);
	vector_write<uint16_t>(out, pfield->infos.turn_id_by_player[1]);
	for(int playerid = 0; playerid < 2; ++playerid) {
		vector_write<int32_t>(out, pfield->core.summon_count[playerid]);
		vector_write<uint8_t>(out, pfield->core.extra_summon[playerid]);
		vector_write<uint8_t>(out, pfield->core.summon_state_count[playerid]);
		vector_write<uint8_t>(out, pfield->core.normalsummon_state_count[playerid]);
		vector_write<uint8_t>(out, pfield->core.flipsummon_state_count[playerid]);
		vector_write<uint8_t>(out, pfield->core.spsummon_state_count[playerid]);
		vector_write<uint8_t>(out, pfield->core.attack_state_count[playerid]);
		vector_write<uint8_t>(out, pfield->core.battle_phase_count[playerid]);
		vector_write<uint8_t>(out, pfield->core.battled_count[playerid]);
	}
}

void effect_info_write_spsummon_once(std::vector<byte>& out, field* pfield) {
	struct once_entry {
		uint8_t playerid;
		uint32_t code;
		uint32_t count;
	};
	std::vector<once_entry> entries;
	for(uint8_t playerid = 0; playerid < 2; ++playerid)
		for(const auto& it : pfield->core.spsummon_once_map[playerid])
			entries.push_back(once_entry{ playerid, it.first, it.second });
	std::sort(entries.begin(), entries.end(), [](const once_entry& a, const once_entry& b) {
		if(a.playerid != b.playerid)
			return a.playerid < b.playerid;
		return a.code < b.code;
	});
	vector_write<uint16_t>(out, (uint16_t)entries.size());
	vector_write<uint16_t>(out, 9);
	size_t body_start = out.size();
	for(const auto& entry : entries) {
		vector_write<uint8_t>(out, entry.playerid);
		vector_write<uint32_t>(out, entry.code);
		vector_write<uint32_t>(out, entry.count);
	}
	effect_info_patch_record_len(out, body_start, entries.size());
}

void effect_info_write_activity(std::vector<byte>& out, field* pfield) {
	struct activity_entry {
		uint8_t kind;
		int32_t counter_id;
		uint16_t count[2];
	};
	std::vector<activity_entry> entries;
	const std::pair<uint8_t, const activity_map*> maps[] = {
		{ ACTIVITY_SUMMON, &pfield->core.summon_counter },
		{ ACTIVITY_NORMALSUMMON, &pfield->core.normalsummon_counter },
		{ ACTIVITY_SPSUMMON, &pfield->core.spsummon_counter },
		{ ACTIVITY_FLIPSUMMON, &pfield->core.flipsummon_counter },
		{ ACTIVITY_ATTACK, &pfield->core.attack_counter },
		{ ACTIVITY_CHAIN, &pfield->core.chain_counter },
	};
	for(const auto& entry : maps) {
		for(const auto& it : *entry.second) {
			// field::check_card_counter packs player 1's count in the high half
			uint32_t packed = it.second.second;
			entries.push_back(activity_entry{ entry.first, it.first,
				{ (uint16_t)(packed & 0xffff), (uint16_t)((packed >> 16) & 0xffff) } });
		}
	}
	std::sort(entries.begin(), entries.end(), [](const activity_entry& a, const activity_entry& b) {
		if(a.kind != b.kind)
			return a.kind < b.kind;
		return a.counter_id < b.counter_id;
	});
	vector_write<uint16_t>(out, (uint16_t)entries.size());
	vector_write<uint16_t>(out, 9);
	size_t body_start = out.size();
	for(const auto& entry : entries) {
		vector_write<uint8_t>(out, entry.kind);
		vector_write<int32_t>(out, entry.counter_id);
		vector_write<uint16_t>(out, entry.count[0]);
		vector_write<uint16_t>(out, entry.count[1]);
	}
	effect_info_patch_record_len(out, body_start, entries.size());
}

void effect_info_collect_cards(field* pfield, std::vector<card*>& out) {
	std::set<card*> seen;
	for(uint8_t playerid = 0; playerid < 2; ++playerid) {
		const card_vector* lists[] = {
			&pfield->player[playerid].list_main,
			&pfield->player[playerid].list_hand,
			&pfield->player[playerid].list_mzone,
			&pfield->player[playerid].list_szone,
			&pfield->player[playerid].list_grave,
			&pfield->player[playerid].list_remove,
			&pfield->player[playerid].list_extra,
		};
		for(const auto* list : lists)
			for(card* pcard : *list)
				if(pcard && seen.insert(pcard).second)
					out.push_back(pcard);
	}
	std::sort(out.begin(), out.end(), [](card* a, card* b) {
		return a->get_info_location() < b->get_info_location();
	});
}

void effect_info_write_player_state(std::vector<byte>& out, field* pfield) {
	vector_write<uint16_t>(out, 2);
	vector_write<uint16_t>(out, EFFECT_INFO_PLAYER_RECORD_LEN);
	for(uint8_t playerid = 0; playerid < 2; ++playerid) {
		const auto& player = pfield->player[playerid];
		vector_write<uint32_t>(out, playerid);
		vector_write<uint32_t>(out, player.used_location);
		vector_write<uint32_t>(out, player.disabled_location);
		vector_write<uint32_t>(out, player.extra_p_count);
		vector_write<uint32_t>(out, player.szone_size);
	}
}

void effect_info_write_card_state(std::vector<byte>& out, field* pfield,
			const std::vector<card*>& cards) {
	vector_write<uint16_t>(out, (uint16_t)cards.size());
	vector_write<uint16_t>(out, EFFECT_INFO_CARD_RECORD_LEN);
	for(card* pcard : cards) {
		vector_write<uint32_t>(out, pcard->get_info_location());
		vector_write<uint32_t>(out, pcard->data.code);
		vector_write<uint32_t>(out, pcard->owner);
		vector_write<uint32_t>(out, pcard->summon_player);
		vector_write<uint32_t>(out, pcard->summon_info);
		vector_write<uint32_t>(out, pcard->status);
		vector_write<uint32_t>(out, pcard->attack_announce_count);
		vector_write<uint32_t>(out, pcard->direct_attackable);
		vector_write<uint32_t>(out, pcard->announce_count);
		vector_write<uint32_t>(out, pcard->attacked_count);
		vector_write<uint32_t>(out, pcard->attack_all_target);
		vector_write<uint32_t>(out, pcard->attack_controler);
		vector_write<uint32_t>(out, (uint32_t)pcard->material_cards.size());
		vector_write<uint32_t>(out, (uint32_t)pcard->relations.size());
		vector_write<uint32_t>(out, (uint32_t)pcard->indestructable_effects.size());
	}
}

void effect_info_write_relations(std::vector<byte>& out,
			const std::vector<card*>& cards) {
	struct relation_entry {
		uint32_t kind;
		uint32_t source_info;
		uint32_t source_code;
		uint32_t target_info;
		uint32_t target_code;
		uint32_t reset;
		uint32_t reserved;
	};
	std::vector<relation_entry> entries;
	for(card* source : cards) {
		for(card* target : source->material_cards) {
			if(target)
				entries.push_back({ EFFECT_INFO_REL_MATERIAL,
					source->get_info_location(), source->data.code,
					target->get_info_location(), target->data.code, 0, 0 });
		}
		for(const auto& relation : source->relations) {
			card* target = relation.first;
			if(target)
				entries.push_back({ EFFECT_INFO_REL_GENERIC,
					source->get_info_location(), source->data.code,
					target->get_info_location(), target->data.code,
					relation.second, 0 });
		}
	}
	std::sort(entries.begin(), entries.end(), [](const relation_entry& a,
			const relation_entry& b) {
		if(a.kind != b.kind)
			return a.kind < b.kind;
		if(a.source_info != b.source_info)
			return a.source_info < b.source_info;
		return a.target_info < b.target_info;
	});
	vector_write<uint16_t>(out, (uint16_t)entries.size());
	vector_write<uint16_t>(out, EFFECT_INFO_RELATION_RECORD_LEN);
	for(const auto& entry : entries) {
		vector_write<uint32_t>(out, entry.kind);
		vector_write<uint32_t>(out, entry.source_info);
		vector_write<uint32_t>(out, entry.source_code);
		vector_write<uint32_t>(out, entry.target_info);
		vector_write<uint32_t>(out, entry.target_code);
		vector_write<uint32_t>(out, entry.reset);
		vector_write<uint32_t>(out, entry.reserved);
	}
}

void effect_info_write_setcode(std::vector<byte>& out,
			const std::vector<card*>& cards) {
	struct setcode_entry {
		uint32_t info;
		uint32_t code;
		uint32_t setcode;
	};
	std::vector<setcode_entry> entries;
	for(card* pcard : cards)
		for(uint16_t setcode : pcard->current.setcode)
			if(setcode)
				entries.push_back({ pcard->get_info_location(), pcard->data.code, setcode });
	std::sort(entries.begin(), entries.end(), [](const setcode_entry& a,
			const setcode_entry& b) {
		if(a.info != b.info)
			return a.info < b.info;
		return a.setcode < b.setcode;
	});
	vector_write<uint16_t>(out, (uint16_t)entries.size());
	vector_write<uint16_t>(out, EFFECT_INFO_SETCODE_RECORD_LEN);
	for(const auto& entry : entries) {
		vector_write<uint32_t>(out, entry.info);
		vector_write<uint32_t>(out, entry.code);
		vector_write<uint32_t>(out, entry.setcode);
	}
}

} // namespace

/**
* @brief Dump the "already applied" duel state that no QUERY_* flag reaches.
*
* Purely read-only: it walks existing public members, calls no Lua and changes
* nothing.  Format: the MirrorForce documentation.
* @param buf byte array supplied by the caller
* @param buf_size its capacity in bytes
* @return bytes written, or LEN_FAIL if the dump does not fit
*/
int32_t query_effect_info(intptr_t pduel, byte* buf, int32_t buf_size) {
	MfArenaGuard mf_guard_(pduel);
	if(!pduel || !buf || buf_size <= 0)
		return LEN_FAIL;
	field* pfield = ((duel*)pduel)->game_field;
	std::vector<card*> cards;
	effect_info_collect_cards(pfield, cards);
	std::vector<byte> out;
	out.reserve(2048);
	vector_write<uint8_t>(out, EFFECT_INFO_VERSION);
	vector_write<uint8_t>(out, EFFECT_INFO_HEADER_LEN);
	vector_write<uint16_t>(out, 0);
	vector_write<uint32_t>(out, 0);		// total length, back-patched below

	auto begin_section = [&out](uint8_t id) {
		vector_write<uint8_t>(out, id);
		vector_write<uint32_t>(out, 0);
		return out.size();
	};
	auto end_section = [&out](size_t payload_start) {
		uint32_t len = (uint32_t)(out.size() - payload_start);
		std::memcpy(out.data() + payload_start - sizeof(uint32_t), &len, sizeof(len));
	};

	size_t mark = begin_section(EFFECT_INFO_SECTION_EFFECTS);
	effect_info_write_effects(out, pfield, cards);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_COUNT_CODE);
	effect_info_write_count_code(out, pfield);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_TURN);
	effect_info_write_turn(out, pfield);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_SPSUMMON_ONCE);
	effect_info_write_spsummon_once(out, pfield);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_ACTIVITY);
	effect_info_write_activity(out, pfield);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_PLAYER_STATE);
	effect_info_write_player_state(out, pfield);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_CARD_STATE);
	effect_info_write_card_state(out, pfield, cards);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_RELATIONS);
	effect_info_write_relations(out, cards);
	end_section(mark);
	mark = begin_section(EFFECT_INFO_SECTION_SETCODE);
	effect_info_write_setcode(out, cards);
	end_section(mark);
	vector_write<uint8_t>(out, EFFECT_INFO_SECTION_END);
	vector_write<uint32_t>(out, 0);

	uint32_t total = (uint32_t)out.size();
	std::memcpy(out.data() + 4, &total, sizeof(total));
	if((int32_t)out.size() > buf_size)
		return LEN_FAIL;
	std::memcpy(buf, out.data(), out.size());
	return (int32_t)out.size();
}
void set_responsei(intptr_t pduel, int32_t value) {
	MfArenaGuard mf_guard_(pduel);
	((duel*)pduel)->set_responsei(value);
}
void set_responseb(intptr_t pduel, byte* buf) {
	MfArenaGuard mf_guard_(pduel);
	((duel*)pduel)->set_responseb(buf);
}
int32_t preload_script(intptr_t pduel, const char* script_name) {
	MfArenaGuard mf_guard_(pduel);
	return ((duel*)pduel)->lua->load_script(script_name);
}

/* MirrorForce snapshot surface: thin re-exports so the ctypes wrapper can
 * reach the arena machinery through the same ocgapi symbol set.  The heavy
 * lifting and the invariants live in mfsnap.cpp. */
extern "C" OCGCORE_API void* duel_snapshot(intptr_t pduel) {
	return mf_duel_snapshot(pduel);
}
extern "C" OCGCORE_API int32_t duel_rollback(intptr_t pduel, void* snap) {
	return mf_duel_rollback(pduel, snap);
}
extern "C" OCGCORE_API void duel_snapshot_free(void* snap) {
	mf_snapshot_free(snap);
}
extern "C" OCGCORE_API int64_t duel_arena_extent(intptr_t pduel) {
	return (int64_t)mf_duel_arena_extent(pduel);
}
/* Weak script-temporary groups (read-only diagnostics, host boundary only).
 * duel_group_stats writes up to `count` of: live groups, groups of the current
 * script call, weak groups, collected groups deleted so far, collected groups
 * waiting for deletion, weak-table slots ever issued, Lua heap bytes (this one
 * follows Lua's own allocation pattern, which its per-process string hash seed
 * shapes); returns how many it wrote.  duel_group_audit
 * runs the holds audit (a weak group kept by a C++ structure, an anchor that does
 * not match its group) and returns the number of violations, the first described
 * in `what`. */
extern "C" OCGCORE_API int32_t duel_group_stats(intptr_t pduel, uint64_t* out, int32_t count) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel) || !live_duel(pduel) || !out || count < 0)
		return -1;
	MfArenaGuard mf_guard_(pduel);
	const duel* pd = (const duel*)pduel;
	uint64_t weak = 0;
	for(const group* pgroup : pd->groups)
		weak += pgroup->weak;
	lua_State* L = pd->lua->lua_state;
	const uint64_t stats[] = {pd->groups.size(), pd->sgroups.size(), weak, pd->groups_collected,
	                          pd->collected_groups.size(), (uint64_t)(pd->lua->weak_group_next - 1),
	                          ((uint64_t)lua_gc(L, LUA_GCCOUNT, 0) << 10) + (uint64_t)lua_gc(L, LUA_GCCOUNTB, 0)};
	const int32_t n = count < (int32_t)(sizeof stats / sizeof stats[0]) ? count : (int32_t)(sizeof stats / sizeof stats[0]);
	for(int32_t i = 0; i < n; ++i)
		out[i] = stats[i];
	return n;
}
extern "C" OCGCORE_API int32_t duel_group_audit(intptr_t pduel, char* what, int32_t size) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel) || !live_duel(pduel) || size < 0 || (size && !what))
		return -1;
	MfArenaGuard mf_guard_(pduel);
	duel* pd = (duel*)pduel;
	if(pd->lua->current_state != pd->lua->lua_state || pd->lua->no_action || !pd->lua->params.empty())
		return -1;
	return pd->audit_group_holds(what, (size_t)size);
}
