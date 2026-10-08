/*
 * duel.cpp
 *
 *  Created on: 2010-5-2
 *      Author: Argon
 */

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include "duel.h"
#include "interpreter.h"
#include "field.h"
#include "card.h"
#include "effect.h"
#include "group.h"
#include "ocgapi.h"
#include "buffer.h"

duel::duel() {
	lua = new interpreter(this, false);
	game_field = new field(this);
	game_field->temp_card = new_card(TEMP_CARD_ID);
	message_buffer.reserve(SIZE_MESSAGE_BUFFER);
#ifdef _WIN32
	_set_error_mode(_OUT_TO_MSGBOX);
#endif // _WIN32
}
duel::~duel() {
	for(auto& pcard : cards)
		delete pcard;
	// lua_close below runs every group userdata's finalizer: none may reach a deleted group
	for(auto& pgroup : groups) {
		if(pgroup->ud)
			*pgroup->ud = nullptr;
		delete pgroup;
	}
	for(auto& peffect : effects)
		delete peffect;
	delete game_field;
	delete lua;
}
void duel::clear() {
	tactical_audit = mf_tactical_audit{};
	if(shuffle_plan.mode)
		shuffle_plan.fail(mf_shuffle_plan::DUEL_CLEAR);
	for(auto& pcard : cards) {
		lua->unregister_card(pcard);
		delete pcard;
	}
	std::vector<int32_t> released_slots;
	for(auto& pgroup : groups) {
		lua->unregister_group(pgroup, &released_slots);
		delete pgroup;
	}
	std::sort(released_slots.begin(), released_slots.end(), std::greater<int32_t>());
	lua->weak_group_free.insert(lua->weak_group_free.end(), released_slots.begin(), released_slots.end());
	lua->weak_group_bytes = 0;
	collected_groups.clear();
	for(auto& peffect : effects) {
		lua->unregister_effect(peffect);
		delete peffect;
	}
	delete game_field;
	cards.clear();
	groups.clear();
	effects.clear();
	assumes.clear();
	sgroups.clear();
	uncopy.clear();
	game_field = new field(this);
	game_field->temp_card = new_card(TEMP_CARD_ID);
}
bool duel::dormant_code(uint32_t code) const {
	return std::binary_search(dormant_codes.begin(), dormant_codes.end(), code);
}
bool duel::dormant_allows_registration() const {
	if(!dormant_identities || !initializing || initializing->cardid < dormant_end)
		return true;
	return dormant_code(initializing->data.code);
}
card* duel::new_card(uint32_t code) {
	card* pcard = new card(this);
	cards.insert(pcard);
	if (code != TEMP_CARD_ID)
		::read_card(code, &(pcard->data));
	pcard->data.code = code;
	lua->register_card(pcard);
	return pcard;
}
group* duel::register_group(group* pgroup) {
	groups.insert(pgroup);
	if(lua->call_depth)
		sgroups.insert(pgroup);
	lua->register_group(pgroup);
	return pgroup;
}
group* duel::new_group() {
	group* pgroup = new group(this);
	return register_group(pgroup);
}
group* duel::new_group(card* pcard) {
	group* pgroup = new group(this, pcard);
	return register_group(pgroup);
}
group* duel::new_group(const card_set& cset) {
	group* pgroup = new group(this, cset);
	return register_group(pgroup);
}
effect* duel::new_effect() {
	effect* peffect = new effect(this);
	effects.insert(peffect);
	lua->register_effect(peffect);
	return peffect;
}
void duel::delete_card(card* pcard) {
	lua->unregister_card(pcard);
	cards.erase(pcard);
	delete pcard;
}
void duel::delete_group(group* pgroup) {
	lua->unregister_group(pgroup);
	groups.erase(pgroup);
	sgroups.erase(pgroup);
	delete pgroup;
}
static bool drop_from(effect** list, uint32_t& count, const effect* peffect) {
	for(uint32_t i = 0; i < count; ++i) {
		if(list[i] == peffect) {
			list[i] = list[--count];
			list[count] = nullptr;
			return true;
		}
	}
	return false;
}
static bool names(const duel::forced_activation& forced, const effect* peffect) {
	const card* handler = peffect->get_handler();
	return (peffect->type & EFFECT_TYPE_ACTIONS) && handler && handler->data.code == forced.code
	    && peffect->description == forced.description && handler->current.controler == forced.player;
}
bool duel::is_forced_activation(const effect* peffect) const {
	for(uint32_t i = 0; i < forced_activation_count; ++i)
		if(names(forced_activations[i], peffect))
			return true;
	return false;
}
bool duel::is_forced_link(const effect* peffect) const {
	for(uint32_t i = 0; i < forced_link_count; ++i)
		if(forced_links[i] == peffect)
			return true;
	return false;
}
bool duel::blank_passes_filter(const card* pcard) const {
	// Inside a forced link only: a blank placeholder in the acting player's own hidden zones stands for the card
	// the opponent took there, which the stream never shows.
	const effect* reason = game_field->core.reason_effect;
	return forced_link_count && reason && is_forced_link(reason) && pcard
	    && pcard->data.code >= 999000001u && pcard->data.code <= 999000004u
	    && pcard->current.controler == game_field->core.reason_player
	    && (pcard->current.location & (LOCATION_HAND | LOCATION_DECK | LOCATION_EXTRA));
}
void duel::chain_forced(effect* peffect) {
	for(uint32_t i = 0; i < forced_activation_count; ++i) {
		if(names(forced_activations[i], peffect)) {
			forced_activations[i] = forced_activations[--forced_activation_count];
			forced_activations[forced_activation_count] = {};
			if(forced_link_count < FORCED_ACTIVATION_CAPACITY)
				forced_links[forced_link_count++] = peffect;
			return;
		}
	}
}
void duel::solve_forced(effect* peffect) {
	drop_from(forced_links, forced_link_count, peffect);
}
void duel::drop_forced(effect* peffect) {
	drop_from(forced_links, forced_link_count, peffect);
}
void duel::delete_effect(effect* peffect) {
	drop_forced(peffect);
	lua->unregister_effect(peffect);
	effects.erase(peffect);
	delete peffect;
}
int32_t duel::read_buffer(byte* buf) {
	auto size = buffer_size();
	if (size)
		std::memcpy(buf, message_buffer.data(), size);
	return (int32_t)size;
}
void duel::release_script_group() {
	for(group* pgroup : collected_groups)
		if(!sgroups.count(pgroup))
			mf_core_refusal("weak_group_invariant", "release_script_group: a collected group outside the call's groups");
	releasing_groups = true;
	std::vector<int32_t> released_slots;
	for(auto& pgroup : sgroups) {
		if(pgroup->is_readonly == GTYPE_DEFAULT) {
			lua->unregister_group(pgroup, &released_slots);
			groups.erase(pgroup);
			delete pgroup;
		}
	}
	sgroups.clear();
	// sgroups is walked in address order; the weak-table slots go back in slot order, so which slot the next weak
	// group takes -- and with it the weak table's shape and the collector's pacing -- follows the calls, not addresses
	std::sort(released_slots.begin(), released_slots.end(), std::greater<int32_t>());
	lua->weak_group_free.insert(lua->weak_group_free.end(), released_slots.begin(), released_slots.end());
	collected_groups.clear();  // all of them were default groups of this call, deleted above
	lua->weak_group_bytes = 0;
	releasing_groups = false;
}
// Lua's collector found a weak script temporary unreachable (the finalizer of its userdata, Group.__gc) and already
// cleared its weak-table slot. Its userdata is freed by a later cycle, so the pointer there is cleared now; the group
// itself waits for the next deterministic point. Incremental cycles end at points that depend on the Lua heap's own
// layout (its string hashes are seeded per process), so deleting here would let group lifetimes differ between
// processes; a full collection at a point fixed by the calls finds the same unreachable set everywhere.
void duel::mark_collected(group* pgroup) {
	if(releasing_groups)
		mf_core_refusal("weak_group_invariant", "mark_collected: a finalizer ran while release_script_group walked sgroups");
	*pgroup->ud = nullptr;
	pgroup->ud = nullptr;
	pgroup->collected = true;
	collected_groups.push_back(pgroup);
#ifdef MF_GROUP_AUDIT
	++groups_marked;
#endif
}
// Delete every collected group, right after a full collection (see interpreter::return_temporary_group), in slot
// order: the arena sees the same frees, and the weak table gets the same slots back, in every process.
void duel::free_collected_groups() {
	std::sort(collected_groups.begin(), collected_groups.end(),
	          [](const group* a, const group* b) { return a->ref_handle > b->ref_handle; });
	for(group* pgroup : collected_groups) {
		lua->weak_group_free.push_back(pgroup->ref_handle);
		groups.erase(pgroup);
		sgroups.erase(pgroup);
		++groups_collected;
		delete pgroup;
	}
	collected_groups.clear();
}
// The weak-group audit (diagnostic builds run it after every process(); duel_group_audit exposes it): no C++
// structure that keeps a group beyond a library call may keep a weak one, and every group's Lua anchor must match
// its state. Returns the number of violations and describes the first in `what`.
int32_t duel::audit_group_holds(char* what, size_t size) {
	int32_t violations = 0;
	if(what && size)
		what[0] = 0;
	auto fail = [&](const char* where, const group* pgroup) {
		if(!violations++ && what && size)
			std::snprintf(what, size, "%s: group %p (weak %d, type %u)", where, (const void*)pgroup,
			              (int)pgroup->weak, pgroup->is_readonly);
	};
	// a processor unit's pointers are cards, effects or groups by unit type: only a live group is checked
	auto kept = [&](const char* where, const void* ptr) {
		group* pgroup = (group*)ptr;
		if(pgroup && groups.count(pgroup) && pgroup->weak)
			fail(where, pgroup);
	};
	auto kept_by_unit = [&](const processor_unit& unit) {
		kept("processor unit ptarget", unit.ptarget);
		kept("processor unit ptr1", unit.ptr1);
		kept("processor unit ptr2", unit.ptr2);
		kept("processor unit ptr3", unit.ptr3);
		kept("processor unit ptr4", unit.ptr4);
	};
	auto kept_by_event = [&](const tevent& event) {
		kept("event cards", event.event_cards);
	};
	auto kept_by_chain = [&](const chain& ch) {
		kept("chain target cards", ch.target_cards);
		kept_by_event(ch.evt);
		for(const auto& info : ch.opinfos)
			kept("chain operation info", info.second.op_cards);
	};
	auto& core = game_field->core;
	for(const auto* list : {&core.units, &core.subunits})
		for(const auto& unit : *list)
			kept_by_unit(unit);
	kept_by_unit(core.damage_step_reserved);
	kept_by_unit(core.summon_reserved);
	for(const auto* list : {&core.point_event, &core.instant_event, &core.queue_event, &core.delayed_activate_event,
	                        &core.full_event, &core.used_event, &core.single_event, &core.solving_event,
	                        &core.sub_solving_event})
		for(const auto& event : *list)
			kept_by_event(event);
	for(const auto* collection : {&core.delayed_quick_tmp, &core.delayed_quick})
		for(const auto& entry : *collection)
			kept_by_event(entry.second);
	for(const auto* array : {&core.select_chains, &core.current_chain, &core.ignition_priority_chains})
		for(const auto& ch : *array)
			kept_by_chain(ch);
	for(const auto* list : {&core.continuous_chain, &core.solving_continuous, &core.sub_solving_continuous,
	                        &core.delayed_continuous_tp, &core.delayed_continuous_ntp, &core.desrep_chain,
	                        &core.new_fchain, &core.new_fchain_s, &core.new_ochain, &core.new_ochain_s,
	                        &core.new_fchain_b, &core.new_ochain_b, &core.new_ochain_h, &core.new_chains})
		for(const auto& ch : *list)
			kept_by_chain(ch);
	for(const auto& entry : core.quick_f_chain)
		kept_by_chain(entry.second);
	kept("core.limit_syn", core.limit_syn);
	kept("core.limit_xyz", core.limit_xyz);
	kept("core.limit_link", core.limit_link);
	lua_State* L = lua->lua_state;
	luaL_checkstack(L, 3, nullptr);
	lua_getglobal(L, "Group");
	const int group_meta = lua_gettop(L);
	auto group_at = [&](int index) -> group* {  // the live group of a group userdata at index, else null
		if(!lua_isuserdata(L, index) || !lua_getmetatable(L, index))
			return nullptr;
		const bool is_group = lua_rawequal(L, -1, group_meta);
		lua_pop(L, 1);
		return is_group ? *(group**)lua_touserdata(L, index) : nullptr;
	};
	// a label keeps a registry reference; one that still names a live group names it as a strong group
	for(effect* peffect : effects) {
		if(peffect->object_type != PARAM_TYPE_GROUP || !peffect->label_object)
			continue;
		lua_rawgeti(L, LUA_REGISTRYINDEX, peffect->label_object);
		group* pgroup = group_at(-1);
		lua_pop(L, 1);
		if(pgroup && (pgroup->weak || pgroup->ref_handle != peffect->label_object))
			fail("effect label object", pgroup);
	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, lua->weak_groups);
	for(group* pgroup : groups) {
		if(pgroup->collected) {
			if(!pgroup->weak || pgroup->ud || !sgroups.count(pgroup))
				fail("collected group not a weak default group of the current call", pgroup);
			continue;
		}
		if(!pgroup->ud || *pgroup->ud != pgroup) {
			fail("group userdata does not point back", pgroup);
			continue;
		}
		if(pgroup->weak) {
			if(pgroup->is_readonly != GTYPE_DEFAULT || !sgroups.count(pgroup))
				fail("weak group outside the current call's default groups", pgroup);
			// an empty slot is a group the collector already found unreachable, its finalizer not yet run
			lua_rawgeti(L, -1, pgroup->ref_handle);
			if(!lua_isnil(L, -1) && lua_touserdata(L, -1) != (void*)pgroup->ud)
				fail("weak table slot names another userdata", pgroup);
			lua_pop(L, 1);
		} else {
			lua_rawgeti(L, LUA_REGISTRYINDEX, pgroup->ref_handle);
			if(lua_touserdata(L, -1) != (void*)pgroup->ud)
				fail("registry reference names another value", pgroup);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 2);
	return violations;
}
void mf_core_refusal(const char* reason, const char* detail) {
	throw std::runtime_error(std::string("ygopro-core refusal [") + reason + "]: " + detail);
}
void duel::restore_assumes() {
	for(auto& pcard : assumes)
		pcard->assume_type = 0;
	assumes.clear();
}
void duel::write_buffer(const void* data, size_t size) {
	vector_write_block(message_buffer, data, size);
}
void duel::write_buffer32(uint32_t value) {
	vector_write<uint32_t>(message_buffer, value);
}
void duel::write_buffer16(uint16_t value) {
	vector_write<uint16_t>(message_buffer, value);
}
void duel::write_buffer8(uint8_t value) {
	vector_write<unsigned char>(message_buffer, value);
}
void duel::clear_buffer() {
	message_buffer.clear();
}
void duel::set_responsei(int32_t resp) {
	game_field->returns.ivalue[0] = resp;
}
void duel::set_responseb(byte* resp) {
	std::memcpy(game_field->returns.bvalue, resp, SIZE_RETURN_VALUE);
}
int32_t duel::get_next_integer(int32_t l, int32_t h) {
	tactical_audit.random();
	if (rng_version == 1)
		return random.get_random_integer_v1(l, h);
	return random.get_random_integer_v2(l, h);
}
int32_t duel::get_next_outcome(int32_t l, int32_t h) {
	const int32_t natural = get_next_integer(l, h);
	if(forced_outcome_head < forced_outcome_count) {
		const int32_t value = forced_outcomes[forced_outcome_head++];
		if(value >= l && value <= h)
			return value;
		++forced_random_misses;
	}
	return natural;
}
const duel::forced_select* duel::next_forced_select() {
	if(forced_select_head >= forced_select_count)
		return nullptr;
	return &forced_selects[forced_select_head++];
}
