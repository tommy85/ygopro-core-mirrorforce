/*
 * libdebug.cpp
 *
 *  Created on: 2012-2-8
 *      Author: Argon
 */

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include "scriptlib.h"
#include "duel.h"
#include "field.h"
#include "card.h"
#include "effect.h"
#include "group.h"
#include "ocgapi.h"

int32_t scriptlib::debug_message(lua_State *L) {
	check_param_count(L, 1);
	duel* pduel = interpreter::get_duel_info(L);
	lua_getglobal(L, "tostring");
	lua_pushvalue(L, -2);
	lua_pcall(L, 1, 1, 0);
	interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(L, -1));
	handle_message(pduel, 2);
	return 0;
}
int32_t scriptlib::debug_add_card(lua_State *L) {
	check_param_count(L, 6);
	duel* pduel = interpreter::get_duel_info(L);
	int32_t code = (int32_t)lua_tointeger(L, 1);
	int32_t owner = (int32_t)lua_tointeger(L, 2);
	int32_t playerid = (int32_t)lua_tointeger(L, 3);
	int32_t location = (int32_t)lua_tointeger(L, 4);
	int32_t sequence = (int32_t)lua_tointeger(L, 5);
	int32_t position = (int32_t)lua_tointeger(L, 6);
	int32_t proc = lua_toboolean(L, 7);
	if (!check_playerid(owner))
		return 0;
	if (!check_playerid(playerid))
		return 0;
	if(pduel->game_field->is_location_useable(playerid, location, sequence)) {
		card* pcard = pduel->new_card(code);
		pcard->owner = owner;
		if(location == LOCATION_EXTRA && position == 0)
			position = POS_FACEDOWN_DEFENSE;
		pcard->sendto_param.position = position;
		if(location == LOCATION_PZONE) {
			int32_t seq = pduel->game_field->get_pzone_sequence(sequence);
			pduel->game_field->add_card(playerid, pcard, LOCATION_SZONE, seq, TRUE);
		} else {
			pduel->game_field->add_card(playerid, pcard, location, sequence);
		}
		pcard->current.position = position;
		if(!(location & (LOCATION_ONFIELD | LOCATION_PZONE)) || (position & POS_FACEUP)) {
			pcard->enable_field_effect(true);
			pduel->game_field->adjust_instant();
		}
		if(proc)
			pcard->set_status(STATUS_PROC_COMPLETE, TRUE);
		interpreter::card2value(L, pcard);
		return 1;
	} else if(location == LOCATION_MZONE) {
		card* fcard = pduel->game_field->get_field_card(playerid, location, sequence);
		if (!fcard || !(fcard->data.type & TYPE_XYZ))
			return 0;
		card* pcard = pduel->new_card(code);
		pcard->owner = owner;
		fcard->xyz_add(pcard);
		if(proc)
			pcard->set_status(STATUS_PROC_COMPLETE, TRUE);
		interpreter::card2value(L, pcard);
		return 1;
	}
	return 0;
}
int32_t scriptlib::debug_set_player_info(lua_State *L) {
	check_param_count(L, 4);
	duel* pduel = interpreter::get_duel_info(L);
	int32_t playerid = (int32_t)lua_tointeger(L, 1);
	int32_t lp = (int32_t)lua_tointeger(L, 2);
	int32_t startcount = (int32_t)lua_tointeger(L, 3);
	int32_t drawcount = (int32_t)lua_tointeger(L, 4);
	if(playerid != 0 && playerid != 1)
		return 0;
	pduel->game_field->player[playerid].lp = lp;
	pduel->game_field->player[playerid].start_count = startcount;
	pduel->game_field->player[playerid].draw_count = drawcount;
	return 0;
}
int32_t scriptlib::debug_pre_summon(lua_State *L) {
	check_param_count(L, 2);
	check_param(L, PARAM_TYPE_CARD, 1);
	card* pcard = *(card**) lua_touserdata(L, 1);
	uint32_t summon_type = (uint32_t)lua_tointeger(L, 2);
	uint8_t summon_location = 0;
	if(lua_gettop(L) > 2)
		summon_location = (uint8_t)lua_tointeger(L, 3);
	pcard->summon_info = summon_type | (summon_location << 16);
	return 0;
}
int32_t scriptlib::debug_pre_equip(lua_State *L) {
	check_param_count(L, 2);
	check_param(L, PARAM_TYPE_CARD, 1);
	check_param(L, PARAM_TYPE_CARD, 2);
	card* equip_card = *(card**) lua_touserdata(L, 1);
	card* target = *(card**) lua_touserdata(L, 2);
	if((equip_card->current.location != LOCATION_SZONE)
	        || (target->current.location != LOCATION_MZONE)
	        || (target->current.position & POS_FACEDOWN))
		lua_pushboolean(L, 0);
	else {
		equip_card->equip(target, FALSE);
		equip_card->effect_target_cards.insert(target);
		target->effect_target_owner.insert(equip_card);
		lua_pushboolean(L, 1);
	}
	return 1;
}
int32_t scriptlib::debug_pre_set_target(lua_State *L) {
	check_param_count(L, 2);
	check_param(L, PARAM_TYPE_CARD, 1);
	check_param(L, PARAM_TYPE_CARD, 2);
	card* t_card = *(card**) lua_touserdata(L, 1);
	card* target = *(card**) lua_touserdata(L, 2);
	t_card->add_card_target(target);
	return 0;
}
int32_t scriptlib::debug_pre_add_counter(lua_State *L) {
	check_param_count(L, 2);
	check_param(L, PARAM_TYPE_CARD, 1);
	card* pcard = *(card**) lua_touserdata(L, 1);
	uint16_t countertype = (uint16_t)lua_tointeger(L, 2);
	uint16_t count = 1;
	if(lua_gettop(L) >= 3)
		count = (uint16_t)lua_tointeger(L, 3);
	auto pr = pcard->counters.emplace(countertype, 0);
	auto cmit = pr.first;
	cmit->second += count;
	return 0;
}
int32_t scriptlib::debug_reload_field_begin(lua_State *L) {
	check_param_count(L, 1);
	duel* pduel = interpreter::get_duel_info(L);
	uint32_t flag = (uint32_t)lua_tointeger(L, 1);
	int32_t rule = (int32_t)lua_tointeger(L, 2);
	pduel->clear();
	pduel->game_field->core.duel_options |= flag;
	if (rule)
		pduel->game_field->core.duel_rule = rule;
	else if (flag & DUEL_OBSOLETE_RULING)
		pduel->game_field->core.duel_rule = 1;
	else
		pduel->game_field->core.duel_rule = CURRENT_RULE;
	if (pduel->game_field->core.duel_rule == MASTER_RULE3) {
		pduel->game_field->player[0].szone_size = 8;
		pduel->game_field->player[1].szone_size = 8;
	}
	return 0;
}
int32_t scriptlib::debug_reload_field_end(lua_State *L) {
	duel* pduel = interpreter::get_duel_info(L);
	pduel->game_field->core.shuffle_hand_check[0] = FALSE;
	pduel->game_field->core.shuffle_hand_check[1] = FALSE;
	pduel->game_field->core.shuffle_deck_check[0] = FALSE;
	pduel->game_field->core.shuffle_deck_check[1] = FALSE;
	pduel->game_field->reload_field_info();
	return 0;
}
int32_t scriptlib::debug_set_ai_name(lua_State *L) {
	check_param_count(L, 1);
	check_param(L, PARAM_TYPE_STRING, 1);
	duel* pduel = interpreter::get_duel_info(L);
	pduel->write_buffer8(MSG_AI_NAME);
	const char* pstr = lua_tostring(L, 1);
	size_t length = std::strlen(pstr);
	if(length > SIZE_AI_NAME - 1)
		length = SIZE_AI_NAME - 1;
	uint16_t len = (uint16_t)length;
	pduel->write_buffer16(len);
	pduel->write_buffer(pstr, len);
	pduel->write_buffer8(0);
	return 0;
}
int32_t scriptlib::debug_show_hint(lua_State *L) {
	check_param_count(L, 1);
	duel* pduel = interpreter::get_duel_info(L);
	lua_getglobal(L, "tostring");
	lua_pushvalue(L, -2);
	lua_pcall(L, 1, 1, 0);
	pduel->write_buffer8(MSG_SHOW_HINT);
	const char* pstr = lua_tostring(L, -1);
	size_t length = std::strlen(pstr);
	if (length > SIZE_HINT_MSG - 1)
		length = SIZE_HINT_MSG - 1;
	uint16_t len = (uint16_t)length;
	pduel->write_buffer16(len);
	pduel->write_buffer(pstr, len);
	pduel->write_buffer8(0);
	return 0;
}

/*
 * State restore: the write side of query_effect_info / query_duel_state.
 *
 * Debug.AddCard and friends rebuild a *board*.  They cannot rebuild the
 * engine's *history*: card::status (only STATUS_PROC_COMPLETE has an entry
 * point, through AddCard's `proc` argument), card::turnid, the three
 * effect_count_code tables that hold once-per-turn bookkeeping,
 * spsummon_once_map, and the processor's activity counters.  A state rebuilt
 * without them offers actions the real duel does not -- spent once-per-turn
 * effects become available again, monsters summoned this turn may change
 * position.  Measured at 84.3-89.3% legal-menu agreement in
 * the MirrorForce documentation; these setters exist to close that gap.
 *
 * Three properties every function below keeps:
 *
 *   - they write members and nothing else: no Lua call, no event raised, no
 *     message written, no adjust pass, no existing code path touched;
 *   - an argument that is absent or nil leaves its member unchanged, so a
 *     caller may restore one field without knowing the rest;
 *   - they are the exact inverse of the corresponding query_effect_info
 *     section, field for field and in the same order.
 *
 * WHEN they may be called matters as much as what they do.  field::process_turn
 * step 0 clears every one of these members at the start of each turn, and
 * ocgapi's start_duel ends by queueing PROCESSOR_TURN -- so anything set from
 * the preload script is wiped before the first menu is built.  The window is
 * *after* the first process() call (which returns once MSG_NEW_TURN is in the
 * buffer, past the wipe) and before the first decision point.  preload_script
 * may be called again at that point: it compiles and runs a chunk in the live
 * duel's Lua state, which is how the caller reaches these functions in time.
 */

// Optional argument that leaves the member alone when absent or nil.
static bool debug_opt_int(lua_State* L, int32_t index, lua_Integer* out) {
	if(lua_gettop(L) < index || lua_isnoneornil(L, index))
		return false;
	*out = lua_tointeger(L, index);
	return true;
}
int32_t scriptlib::debug_set_card_state(lua_State *L) {
	check_param_count(L, 2);
	check_param(L, PARAM_TYPE_CARD, 1);
	card* pcard = *(card**) lua_touserdata(L, 1);
	lua_Integer value = 0;
	if(debug_opt_int(L, 2, &value))
		pcard->status = (uint32_t)value;
	if(debug_opt_int(L, 3, &value))
		pcard->turnid = (uint16_t)value;
	if(debug_opt_int(L, 4, &value))
		pcard->turn_counter = (uint16_t)value;
	if(debug_opt_int(L, 5, &value))
		pcard->summon_player = (uint8_t)value;
	if(debug_opt_int(L, 6, &value))
		pcard->attacked_count = (uint8_t)value;
	if(debug_opt_int(L, 7, &value))
		pcard->announce_count = (uint8_t)value;
	if(debug_opt_int(L, 8, &value))
		pcard->attack_announce_count = (uint8_t)value;
	if(debug_opt_int(L, 9, &value))
		pcard->attack_all_target = (uint8_t)value;
	if(debug_opt_int(L, 10, &value))
		pcard->direct_attackable = (uint8_t)value;
	return 0;
}
int32_t scriptlib::debug_set_count_code(lua_State *L) {
	// table: 0 = per turn, 1 = per duel, 2 = per chain, matching the order
	// query_effect_info writes them in.  playerid 0, 1 or PLAYER_NONE.
	check_param_count(L, 4);
	duel* pduel = interpreter::get_duel_info(L);
	int32_t table = (int32_t)lua_tointeger(L, 1);
	int32_t playerid = (int32_t)lua_tointeger(L, 2);
	uint32_t code = (uint32_t)lua_tointeger(L, 3);
	int32_t value = (int32_t)lua_tointeger(L, 4);
	if(table < 0 || table > 2 || playerid < 0 || playerid > PLAYER_NONE) {
		lua_pushboolean(L, 0);
		return 1;
	}
	auto& core = pduel->game_field->core;
	auto* count_map = (table == 0) ? &core.effect_count_code[playerid]
		: (table == 1) ? &core.effect_count_code_duel[playerid]
		: &core.effect_count_code_chain[playerid];
	if(value <= 0)
		count_map->erase(code);
	else
		(*count_map)[code] = value;
	lua_pushboolean(L, 1);
	return 1;
}
int32_t scriptlib::debug_set_spsummon_once(lua_State *L) {
	check_param_count(L, 3);
	duel* pduel = interpreter::get_duel_info(L);
	int32_t playerid = (int32_t)lua_tointeger(L, 1);
	uint32_t code = (uint32_t)lua_tointeger(L, 2);
	uint32_t count = (uint32_t)lua_tointeger(L, 3);
	if(playerid != 0 && playerid != 1) {
		lua_pushboolean(L, 0);
		return 1;
	}
	auto& once_map = pduel->game_field->core.spsummon_once_map[playerid];
	if(count == 0)
		once_map.erase(code);
	else
		once_map[code] = count;
	lua_pushboolean(L, 1);
	return 1;
}
int32_t scriptlib::debug_set_turn_counters(lua_State *L) {
	// Same field order as query_effect_info's per-player block.
	check_param_count(L, 1);
	duel* pduel = interpreter::get_duel_info(L);
	int32_t playerid = (int32_t)lua_tointeger(L, 1);
	if(playerid != 0 && playerid != 1) {
		lua_pushboolean(L, 0);
		return 1;
	}
	auto& core = pduel->game_field->core;
	lua_Integer value = 0;
	if(debug_opt_int(L, 2, &value))
		core.summon_count[playerid] = (int32_t)value;
	if(debug_opt_int(L, 3, &value))
		core.extra_summon[playerid] = (uint8_t)value;
	if(debug_opt_int(L, 4, &value))
		core.summon_state_count[playerid] = (uint8_t)value;
	if(debug_opt_int(L, 5, &value))
		core.normalsummon_state_count[playerid] = (uint8_t)value;
	if(debug_opt_int(L, 6, &value))
		core.flipsummon_state_count[playerid] = (uint8_t)value;
	if(debug_opt_int(L, 7, &value))
		core.spsummon_state_count[playerid] = (uint8_t)value;
	if(debug_opt_int(L, 8, &value))
		core.attack_state_count[playerid] = (uint8_t)value;
	if(debug_opt_int(L, 9, &value))
		core.battle_phase_count[playerid] = (uint8_t)value;
	if(debug_opt_int(L, 10, &value))
		core.battled_count[playerid] = (uint8_t)value;
	lua_pushboolean(L, 1);
	return 1;
}
int32_t scriptlib::debug_set_activity_count(lua_State *L) {
	// Only updates a counter that already exists.  The map value is
	// (counter_filter, packed count); the filter is a Lua reference installed
	// by Duel.AddCustomActivityCounter when the registering card initialised,
	// so creating an entry here would install a null filter and break
	// field::check_card_counter.  A rebuilt board re-registers its own
	// counters, so every counter worth restoring is already present.
	check_param_count(L, 4);
	duel* pduel = interpreter::get_duel_info(L);
	int32_t kind = (int32_t)lua_tointeger(L, 1);
	int32_t counter_id = (int32_t)lua_tointeger(L, 2);
	uint32_t count0 = (uint32_t)lua_tointeger(L, 3) & 0xffff;
	uint32_t count1 = (uint32_t)lua_tointeger(L, 4) & 0xffff;
	auto& core = pduel->game_field->core;
	activity_map* counter_map =
		(kind == ACTIVITY_SUMMON) ? &core.summon_counter :
		(kind == ACTIVITY_NORMALSUMMON) ? &core.normalsummon_counter :
		(kind == ACTIVITY_SPSUMMON) ? &core.spsummon_counter :
		(kind == ACTIVITY_FLIPSUMMON) ? &core.flipsummon_counter :
		(kind == ACTIVITY_ATTACK) ? &core.attack_counter :
		(kind == ACTIVITY_CHAIN) ? &core.chain_counter : nullptr;
	if(!counter_map) {
		lua_pushboolean(L, 0);
		return 1;
	}
	auto iter = counter_map->find(counter_id);
	if(iter == counter_map->end()) {
		lua_pushboolean(L, 0);
		return 1;
	}
	// field::check_card_counter packs player 1's count in the high half
	iter->second.second = count0 | (count1 << 16);
	lua_pushboolean(L, 1);
	return 1;
}
int32_t scriptlib::debug_set_turn_info(lua_State *L) {
	// process_turn has already incremented turn_id by the time this can be
	// called, so these are absolute values, not deltas.  turn_player is not
	// settable here on purpose: the rebuild picks seats so that the turn
	// player is seat 0, which needs no engine change.
	check_param_count(L, 1);
	duel* pduel = interpreter::get_duel_info(L);
	auto& infos = pduel->game_field->infos;
	lua_Integer value = 0;
	if(debug_opt_int(L, 1, &value))
		infos.turn_id = (uint16_t)value;
	if(debug_opt_int(L, 2, &value))
		infos.turn_id_by_player[0] = (uint16_t)value;
	if(debug_opt_int(L, 3, &value))
		infos.turn_id_by_player[1] = (uint16_t)value;
	return 0;
}

/*
 * Debug.PermuteHidden -- rearrange one player's hidden cards among their hidden
 * slots, for belief-particle search.
 *
 * A particle is not a different set of cards.  In a closed environment the deck
 * list is known, so "what is left" is determined by conservation; only *where*
 * each copy sits is unknown.  A sampled particle is therefore a permutation of
 * the very multiset the duel already holds, which is what makes this safe: no
 * card object is created or destroyed, no script is re-bound, and no event is
 * raised.  Only the mapping from slot to object changes, plus each moved
 * object's current.location / sequence / position -- and, because the field's
 * effect containers hold a card's effects by whether they are in range where
 * the card is, each moved object's field effects leave the containers before
 * the move and come back for the new place after it, exactly as
 * field::remove_card and field::add_card do.  Without that a card moved from
 * the hand into the deck kept its hand registration (the idle menu offered to
 * activate a field spell from the deck, and the activation did nothing, so a
 * greedy player went round it for ever) and a card moved from the deck into
 * the hand had none (it could not be activated from the hand at all).
 *
 * Positions belong to the slot, not to the card: a set spell keeps the slot's
 * face-down position when a different object moves into it.  The slot's other
 * history (turnid, STATUS_SET_TURN, ...) is the caller's job, through
 * Debug.SetCardState -- this function only moves pointers.
 *
 * The whole assignment is resolved before anything is written, so a mismatch
 * leaves the duel exactly as it was and returns false.  Callers must treat
 * false as fatal for that particle rather than continuing: a partially placed
 * particle would still be carrying the opponent's real hand.
 *
 *   Debug.PermuteHidden(playerid, hand_codes, deck_codes [, facedown [, extra_codes]])
 *
 * hand_codes / deck_codes are arrays of passcodes in target order (deck index 1
 * is the bottom, the last entry is the top, matching field::list_main).
 * facedown is a flat array of (location, sequence, code) triples naming further
 * slots to include in the permutation: face-down monster and spell/trap zones,
 * and face-down banishes.  Face-down banishes matter more than they look --
 * they are 93% of the hidden cards that live outside the hand and deck, and
 * they cluster in the decks that banish face-down, so leaving them out does not
 * merely shrink coverage, it shrinks it per-deck.
 *
 * Two pools, never mixed
 * ----------------------
 * A face-down banish can hold an extra-deck monster (Pot of Extravagance banishes
 * random face-down cards from the extra deck), and such a card never belongs to
 * the main-deck multiset: it can only ever have come from, and go back into,
 * the face-down part of the extra deck.  So the participating objects form two
 * pools and every slot draws from exactly one of them:
 *
 *   main pool   hand, main deck, and the face-down slots whose current object
 *               is a main-deck card;
 *   extra pool  the face-down slots whose current object is an extra-deck
 *               monster (card::is_extra_deck_monster), plus the face-down part
 *               of the extra deck when extra_codes is given.
 *
 * A slot's kind is what it holds now: the hand and the main deck are main
 * slots, the extra deck is an extra slot, and a face-down slot is whichever
 * kind its current occupant is.  The same object type decides pool membership,
 * so a main-deck code aimed at an extra slot (or an extra code aimed at a main
 * slot) is simply not in that slot's pool and the whole assignment is refused.
 * That is the refusal the belief side relies on when its public bookkeeping of
 * "this banish came from the extra deck" disagrees with the engine.
 *
 * extra_codes is optional and names the target order of the *face-down* cards
 * in the extra deck (index 1 is list_extra[0]; the face-up pendulum cards at
 * the end of list_extra are public and never move).  Without it the extra deck
 * does not take part, and extra-deck monsters in face-down banishes can only
 * trade places among themselves.
 *
 * Face-up cards are refused wherever they appear: their identity is public, so
 * moving one would change what both players can see.
 */
static void debug_read_u32_array(lua_State* L, int32_t index,
				 std::vector<uint32_t>& out) {
	if(lua_gettop(L) < index || lua_isnoneornil(L, index))
		return;
	if(!lua_istable(L, index))
		return;
	lua_Integer n = (lua_Integer)lua_rawlen(L, index);
	for(lua_Integer i = 1; i <= n; ++i) {
		lua_rawgeti(L, index, i);
		out.push_back((uint32_t)lua_tointeger(L, -1));
		lua_pop(L, 1);
	}
}
namespace {
struct permute_slot {
	uint32_t location;
	uint32_t sequence;
	uint32_t code;
	uint8_t position;
	bool extra_kind;
};
/* The two pools of the comment above.  ``take`` resolves one target code out
 * of one pool; a code that is not there is a refusal, not a search elsewhere. */
struct permute_pools {
	std::multimap<uint32_t, card*> main;
	std::multimap<uint32_t, card*> extra;
	void add(card* pcard, bool extra_kind) {
		(extra_kind ? extra : main).emplace(pcard->data.code, pcard);
	}
	card* take(uint32_t code, bool extra_kind) {
		auto& pool = extra_kind ? extra : main;
		auto it = pool.find(code);
		if(it == pool.end())
			return nullptr;
		card* pcard = it->second;
		pool.erase(it);
		return pcard;
	}
	bool empty() const {
		return main.empty() && extra.empty();
	}
};
/* Collect the face-down slots named by the caller.  Returns false on any slot
 * that may not take part (wrong location, empty, or face-up). */
bool debug_collect_facedown_slots(field* pfield, int32_t playerid,
				  const std::vector<uint32_t>& facedown,
				  std::vector<permute_slot>& slots,
				  permute_pools& pools) {
	for(size_t i = 0; i < facedown.size(); i += 3) {
		uint32_t location = facedown[i];
		uint32_t sequence = facedown[i + 1];
		if(location != LOCATION_MZONE && location != LOCATION_SZONE
		   && location != LOCATION_REMOVED)
			return false;
		card* pcard = pfield->get_field_card((uint8_t)playerid, location,
						     (uint8_t)sequence);
		// only face-down slots may be permuted: a face-up card's identity is
		// public, and moving it would change what both players can see
		if(!pcard || (pcard->current.position & POS_FACEUP))
			return false;
		bool extra_kind = pcard->is_extra_deck_monster();
		pools.add(pcard, extra_kind);
		slots.push_back(permute_slot{ location, sequence, facedown[i + 2],
					      pcard->current.position, extra_kind });
	}
	return true;
}
/* The face-down part of the extra deck: list_extra[0, size - extra_p_count).
 * Returns false if a face-up card sits in that range, which the engine's own
 * bookkeeping never produces. */
bool debug_collect_extra_facedown(player_info& pl, std::vector<uint8_t>& positions,
				  permute_pools& pools) {
	size_t count = pl.list_extra.size();
	if(pl.extra_p_count > count)
		return false;
	count -= pl.extra_p_count;
	for(size_t i = 0; i < count; ++i) {
		card* pcard = pl.list_extra[i];
		if(!pcard || (pcard->current.position & POS_FACEUP))
			return false;
		pools.add(pcard, true);
		positions.push_back(pcard->current.position);
	}
	return true;
}
void debug_place(card* pcard, int32_t playerid, uint8_t location, uint8_t sequence,
		 uint8_t position) {
	pcard->current.controler = (uint8_t)playerid;
	pcard->current.location = location;
	pcard->current.sequence = sequence;
	pcard->current.position = position;
}
/* Why, how and when a card came to a hidden place, which both players saw
 * happen: a hand card drawn this turn stays "drawn this turn" whichever card
 * the particle puts there (a set of it names that reason on the wire). */
struct slot_history {
	uint32_t reason;
	card* reason_card;
	effect* reason_effect;
	uint8_t reason_player;
	uint8_t previous_controler;
	uint8_t previous_location;
	uint8_t previous_sequence;
	uint8_t previous_position;
	uint32_t previous_reason;
	card* previous_reason_card;
	effect* previous_reason_effect;
	uint8_t previous_reason_player;
	uint16_t turnid;
};
slot_history debug_slot_history(card* pcard) {
	return slot_history{ pcard->current.reason, pcard->current.reason_card, pcard->current.reason_effect,
			     pcard->current.reason_player, pcard->previous.controler, pcard->previous.location,
			     pcard->previous.sequence, pcard->previous.position, pcard->previous.reason,
			     pcard->previous.reason_card, pcard->previous.reason_effect, pcard->previous.reason_player,
			     pcard->turnid };
}
void debug_give_history(card* pcard, const slot_history& history) {
	pcard->current.reason = history.reason;
	pcard->current.reason_card = history.reason_card;
	pcard->current.reason_effect = history.reason_effect;
	pcard->current.reason_player = history.reason_player;
	pcard->previous.controler = history.previous_controler;
	pcard->previous.location = history.previous_location;
	pcard->previous.sequence = history.previous_sequence;
	pcard->previous.position = history.previous_position;
	pcard->previous.reason = history.previous_reason;
	pcard->previous.reason_card = history.previous_reason_card;
	pcard->previous.reason_effect = history.previous_reason_effect;
	pcard->previous.reason_player = history.previous_reason_player;
	pcard->turnid = history.turnid;
}
}  // namespace
int32_t scriptlib::debug_permute_hidden(lua_State *L) {
	check_param_count(L, 3);
	duel* pduel = interpreter::get_duel_info(L);
	int32_t playerid = (int32_t)lua_tointeger(L, 1);
	if(playerid != 0 && playerid != 1) {
		lua_pushboolean(L, 0);
		return 1;
	}
	field* pfield = pduel->game_field;
	auto& pl = pfield->player[playerid];

	std::vector<uint32_t> hand_codes, deck_codes, facedown, extra_codes;
	debug_read_u32_array(L, 2, hand_codes);
	debug_read_u32_array(L, 3, deck_codes);
	debug_read_u32_array(L, 4, facedown);
	bool has_extra = lua_gettop(L) >= 5 && !lua_isnoneornil(L, 5);
	if(has_extra)
		debug_read_u32_array(L, 5, extra_codes);
	if(facedown.size() % 3 != 0) {
		lua_pushboolean(L, 0);
		return 1;
	}

	permute_pools pools;
	std::vector<permute_slot> fd_slots;
	std::vector<uint8_t> hand_positions, deck_positions, extra_positions;
	for(auto& pcard : pl.list_hand) {
		pools.add(pcard, false);
		hand_positions.push_back(pcard->current.position);
	}
	for(auto& pcard : pl.list_main) {
		pools.add(pcard, false);
		deck_positions.push_back(pcard->current.position);
	}
	bool ok = debug_collect_facedown_slots(pfield, playerid, facedown, fd_slots, pools);
	if(ok && has_extra)
		ok = debug_collect_extra_facedown(pl, extra_positions, pools);
	if(!ok || hand_codes.size() != pl.list_hand.size()
	   || deck_codes.size() != pl.list_main.size()
	   || (has_extra && extra_codes.size() != extra_positions.size())) {
		lua_pushboolean(L, 0);
		return 1;
	}

	// resolve the whole assignment before writing anything
	std::vector<card*> new_hand, new_main, new_fd, new_extra;
	new_hand.reserve(hand_codes.size());
	new_main.reserve(deck_codes.size());
	new_fd.reserve(fd_slots.size());
	new_extra.reserve(extra_codes.size());
	for(uint32_t code : hand_codes)
		new_hand.push_back(pools.take(code, false));
	for(uint32_t code : deck_codes)
		new_main.push_back(pools.take(code, false));
	for(const auto& slot : fd_slots)
		new_fd.push_back(pools.take(slot.code, slot.extra_kind));
	for(uint32_t code : extra_codes)
		new_extra.push_back(pools.take(code, true));
	ok = pools.empty();
	for(auto* pcards : { &new_hand, &new_main, &new_fd, &new_extra })
		for(card* pcard : *pcards)
			ok = ok && pcard;
	if(!ok) {
		lua_pushboolean(L, 0);
		return 1;
	}

	// every slot keeps its history, read before any object moves
	std::vector<slot_history> hand_history, main_history, fd_history, extra_history;
	for(card* pcard : pl.list_hand)
		hand_history.push_back(debug_slot_history(pcard));
	for(card* pcard : pl.list_main)
		main_history.push_back(debug_slot_history(pcard));
	for(const auto& slot : fd_slots)
		fd_history.push_back(debug_slot_history(pfield->get_field_card((uint8_t)playerid, slot.location,
									       (uint8_t)slot.sequence)));
	for(size_t i = 0; i < new_extra.size(); ++i)
		extra_history.push_back(debug_slot_history(pl.list_extra[i]));
	// every participating object leaves the effect containers from where it is now
	for(auto* pcards : { &new_hand, &new_main, &new_fd, &new_extra })
		for(card* pcard : *pcards)
			pcard->cancel_field_effect();
	for(size_t i = 0; i < new_hand.size(); ++i) {
		debug_place(new_hand[i], playerid, LOCATION_HAND, (uint8_t)i, hand_positions[i]);
		pl.list_hand[i] = new_hand[i];
	}
	for(size_t i = 0; i < new_main.size(); ++i) {
		debug_place(new_main[i], playerid, LOCATION_DECK, (uint8_t)i, deck_positions[i]);
		pl.list_main[i] = new_main[i];
	}
	for(size_t i = 0; i < new_fd.size(); ++i) {
		card* pcard = new_fd[i];
		const auto& slot = fd_slots[i];
		debug_place(pcard, playerid, (uint8_t)slot.location, (uint8_t)slot.sequence,
			    slot.position);
		if(slot.location == LOCATION_MZONE)
			pl.list_mzone[slot.sequence] = pcard;
		else if(slot.location == LOCATION_SZONE)
			pl.list_szone[slot.sequence] = pcard;
		else if(slot.location == LOCATION_REMOVED
			&& slot.sequence < pl.list_remove.size())
			pl.list_remove[slot.sequence] = pcard;
	}
	for(size_t i = 0; i < new_extra.size(); ++i) {
		debug_place(new_extra[i], playerid, LOCATION_EXTRA, (uint8_t)i, extra_positions[i]);
		pl.list_extra[i] = new_extra[i];
	}
	for(size_t i = 0; i < new_hand.size(); ++i)
		debug_give_history(new_hand[i], hand_history[i]);
	for(size_t i = 0; i < new_main.size(); ++i)
		debug_give_history(new_main[i], main_history[i]);
	for(size_t i = 0; i < new_fd.size(); ++i)
		debug_give_history(new_fd[i], fd_history[i]);
	for(size_t i = 0; i < new_extra.size(); ++i)
		debug_give_history(new_extra[i], extra_history[i]);
	// and comes back with the effects that are in range where it now is
	for(auto* pcards : { &new_hand, &new_main, &new_fd, &new_extra })
		for(card* pcard : *pcards)
			pcard->apply_field_effect();
	lua_pushboolean(L, 1);
	return 1;
}

/*
 * Debug.ForceShuffle(playerid, location, codes) -- the order the next shuffle of a player's hand, deck or
 * extra deck leaves, for a local duel that follows a network one: the owner sees its hand after a shuffle
 * and the cards it then draws, the local core must reproduce both. A hand or extra deck order names every
 * card by sequence (index 1 is sequence 0); a deck order names the top cards, index 1 the top. The shuffle
 * still draws its random numbers first, and an order that does not fit its cards leaves the random one.
 * The order is used by one shuffle; an empty table clears it.
 */
int32_t scriptlib::debug_force_shuffle(lua_State *L) {
	check_param_count(L, 3);
	duel* pduel = interpreter::get_duel_info(L);
	if(pduel->shuffle_plan.mode) {
		pduel->shuffle_plan.fail(mf_shuffle_plan::LEGACY_MIX);
		lua_pushboolean(L, 0);
		return 1;
	}
	int32_t playerid = (int32_t)lua_tointeger(L, 1);
	uint32_t location = (uint32_t)lua_tointeger(L, 2);
	if((playerid != 0 && playerid != 1)
	   || (location != LOCATION_HAND && location != LOCATION_DECK && location != LOCATION_EXTRA)) {
		lua_pushboolean(L, 0);
		return 1;
	}
	std::vector<uint32_t> codes;
	debug_read_u32_array(L, 3, codes);
	pduel->game_field->core.forced_shuffle[playerid][location == LOCATION_HAND ? 0 : location == LOCATION_DECK ? 1 : 2] = codes;
	lua_pushboolean(L, 1);
	return 1;
}
#ifdef MF_GROUP_AUDIT
// Diagnostic builds only: a full collection from inside any script callback, finalizers included.
static int32_t debug_collect_garbage(lua_State *L) {
	lua_gc(L, LUA_GCCOLLECT, 0);
	return 0;
}
// Diagnostic builds only: the weak-group counters from inside a script (live groups, groups of the current call,
// weak groups, collected groups deleted so far, collected groups waiting, Lua heap bytes).
static int32_t debug_group_stats(lua_State *L) {
	duel* pduel = interpreter::get_duel_info(L);
	lua_Integer weak = 0;
	for(const group* pgroup : pduel->groups)
		weak += pgroup->weak;
	lua_pushinteger(L, (lua_Integer)pduel->groups.size());
	lua_pushinteger(L, (lua_Integer)pduel->sgroups.size());
	lua_pushinteger(L, weak);
	lua_pushinteger(L, (lua_Integer)pduel->groups_collected);
	lua_pushinteger(L, (lua_Integer)pduel->collected_groups.size());
	lua_pushinteger(L, ((lua_Integer)lua_gc(L, LUA_GCCOUNT, 0) << 10) + lua_gc(L, LUA_GCCOUNTB, 0));
	return 6;
}
// Diagnostic builds only: the holds audit from inside a script (violations, first description), and the one
// mistake it exists to catch -- a C++ structure keeping a group without holding it (core.limit_syn here;
// KeepGroupUnheld(nil) clears it again).
static int32_t debug_group_audit(lua_State *L) {
	duel* pduel = interpreter::get_duel_info(L);
	char what[160];
	const int32_t violations = pduel->audit_group_holds(what, sizeof what);
	lua_pushinteger(L, violations);
	lua_pushstring(L, what);
	return 2;
}
static int32_t debug_keep_group_unheld(lua_State *L) {
	duel* pduel = interpreter::get_duel_info(L);
	if(lua_isnil(L, 1)) {
		pduel->game_field->core.limit_syn = nullptr;
		return 0;
	}
	scriptlib::check_param(L, PARAM_TYPE_GROUP, 1);
	pduel->game_field->core.limit_syn = *(group**)lua_touserdata(L, 1);
	return 0;
}
void mf_group_audit_gc_pressure(lua_State* L) {
	const char* setting = std::getenv("MF_CORE_GC_PRESSURE");
	int pause = 0, stepmul = 0;
	if(!setting)
		return;
	if(std::sscanf(setting, "%d,%d", &pause, &stepmul) != 2 || pause < 10 || stepmul < 100)
		mf_core_refusal("gc_pressure_setting", "MF_CORE_GC_PRESSURE is \"pause,stepmul\" with pause >= 10 and stepmul >= 100");
	lua_gc(L, LUA_GCSETPAUSE, pause);
	lua_gc(L, LUA_GCSETSTEPMUL, stepmul);
}
#endif
static const struct luaL_Reg debuglib[] = {
	{ "Message", scriptlib::debug_message },
	{ "AddCard", scriptlib::debug_add_card },
	{ "SetPlayerInfo", scriptlib::debug_set_player_info },
	{ "PreSummon", scriptlib::debug_pre_summon },
	{ "PreEquip", scriptlib::debug_pre_equip },
	{ "PreSetTarget", scriptlib::debug_pre_set_target },
	{ "PreAddCounter", scriptlib::debug_pre_add_counter },
	{ "ReloadFieldBegin", scriptlib::debug_reload_field_begin },
	{ "ReloadFieldEnd", scriptlib::debug_reload_field_end },
	{ "SetAIName", scriptlib::debug_set_ai_name },
	{ "ShowHint", scriptlib::debug_show_hint },
	{ "SetCardState", scriptlib::debug_set_card_state },
	{ "SetCountCode", scriptlib::debug_set_count_code },
	{ "SetSpSummonOnce", scriptlib::debug_set_spsummon_once },
	{ "SetTurnCounters", scriptlib::debug_set_turn_counters },
	{ "SetActivityCount", scriptlib::debug_set_activity_count },
	{ "SetTurnInfo", scriptlib::debug_set_turn_info },
	{ "PermuteHidden", scriptlib::debug_permute_hidden },
	{ "ForceShuffle", scriptlib::debug_force_shuffle },
#ifdef MF_GROUP_AUDIT
	{ "CollectGarbage", debug_collect_garbage },
	{ "GroupStats", debug_group_stats },
	{ "GroupAudit", debug_group_audit },
	{ "KeepGroupUnheld", debug_keep_group_unheld },
#endif
	{ nullptr, nullptr }
};
void scriptlib::open_debuglib(lua_State *L) {
	luaL_newlib(L, debuglib);
	lua_setglobal(L, "Debug");
}
