/* Viewer-scoped decision-state snapshot for agents and offline supervision. */

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "buffer.h"
#include "card.h"
#include "duel.h"
#include "effect.h"
#include "field.h"
#include "ocgapi.h"

namespace {

struct state_card_entry {
	card* pcard;
	card* host;
	uint32_t host_ordinal;
	uint32_t canonical_sequence;
	bool identity_visible;
	bool own_knowledge;
};

struct state_effect_entry {
	effect* peffect;
	uint8_t container;
};

bool state_view_valid(uint8_t view) {
	return view <= DUEL_STATE_VIEW_OMNISCIENT_LABEL;
}

bool state_is_faceup_or_revealed(card* pcard) {
	uint32_t position = pcard->get_public_info_location() >> 24;
	return (position & POS_FACEUP) || (position & POS_REVEAL);
}

bool state_identity_visible(card* pcard, uint8_t view, card* host = nullptr) {
	if(view == DUEL_STATE_VIEW_OMNISCIENT_LABEL)
		return true;
	// Xyz materials are public objects attached to a public board position.
	if(host)
		return true;
	uint8_t location = pcard->current.location;
	bool public_now = state_is_faceup_or_revealed(pcard)
		|| location == LOCATION_GRAVE;
	if(view == DUEL_STATE_VIEW_SHARED_PUBLIC)
		return public_now;
	// A player knows their own physical cards even after control changes.
	bool own_knowledge = pcard->owner == view;
	bool own_field = pcard->current.controler == view
		&& (location & LOCATION_ONFIELD);
	return own_knowledge || own_field || public_now;
}

bool state_keep_card(card* pcard, uint8_t view, bool visible, card* host = nullptr) {
	if(host || visible)
		return true;
	// Hidden cards whose position is itself public remain as anonymous rows.
	uint8_t location = pcard->current.location;
	return (location & LOCATION_ONFIELD) || location == LOCATION_REMOVED;
}

void state_collect_zone_cards(field* pfield, uint8_t view,
			std::vector<state_card_entry>& out) {
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
		for(const auto* list : lists) {
			for(card* pcard : *list) {
				if(!pcard)
					continue;
				bool visible = state_identity_visible(pcard, view);
				if(state_keep_card(pcard, view, visible))
					out.push_back({ pcard, nullptr, 0, 0, visible,
						view <= DUEL_STATE_VIEW_PLAYER_1 && pcard->owner == view });
			}
		}
	}

	std::sort(out.begin(), out.end(), [view](const state_card_entry& a,
			const state_card_entry& b) {
		card* ca = a.pcard;
		card* cb = b.pcard;
		if(ca->current.controler != cb->current.controler)
			return ca->current.controler < cb->current.controler;
		if(ca->current.location != cb->current.location)
			return ca->current.location < cb->current.location;
		bool canonical_deck_a = view <= DUEL_STATE_VIEW_PLAYER_1
			&& ca->current.location == LOCATION_DECK && ca->owner == view;
		bool canonical_deck_b = view <= DUEL_STATE_VIEW_PLAYER_1
			&& cb->current.location == LOCATION_DECK && cb->owner == view;
		// A card owned by the other player can temporarily be in this Deck.
		// Bucket it before switching from engine sequence to the viewer's
		// passcode-canonical order.  Testing only ``a`` made comp(a,b) use code
		// while comp(b,a) used sequence, violating std::sort's strict weak order.
		if(canonical_deck_a != canonical_deck_b)
			return canonical_deck_a > canonical_deck_b;
		if(canonical_deck_a && ca->data.code != cb->data.code)
			return ca->data.code < cb->data.code;
		return ca->current.sequence < cb->current.sequence;
	});

	// The player knows their Deck as a multiset, not an order.  Replace the
	// engine's sequence by a canonical ordinal after sorting by passcode.
	uint32_t deck_ordinal[2] = { 0, 0 };
	for(auto& entry : out) {
		card* pcard = entry.pcard;
		if(view <= DUEL_STATE_VIEW_PLAYER_1
				&& pcard->current.location == LOCATION_DECK
				&& pcard->owner == view)
			entry.canonical_sequence = deck_ordinal[pcard->current.controler]++;
		else
			entry.canonical_sequence = pcard->current.sequence;
	}

	// Overlay cards are not in a player zone list.  Append them under their host
	// in engine order; breadth-first traversal also handles a nested attachment
	// without relying on an address sort.
	for(size_t i = 0; i < out.size(); ++i) {
		card* host = out[i].pcard;
		for(size_t ordinal = 0; ordinal < host->xyz_materials.size(); ++ordinal) {
			card* material = host->xyz_materials[ordinal];
			if(material)
				out.push_back({ material, host, (uint32_t)ordinal, (uint32_t)ordinal,
					true, false });
		}
	}
}

void state_patch_record_len(std::vector<byte>& out, size_t body_start,
			size_t count) {
	if(!count)
		return;
	uint16_t measured = (uint16_t)((out.size() - body_start) / count);
	std::memcpy(out.data() + body_start - sizeof(uint16_t), &measured,
		sizeof(measured));
}

void state_write_meta(std::vector<byte>& out, field* pfield, uint8_t view,
			uint32_t requested_flags) {
	vector_write<uint16_t>(out, 1);
	vector_write<uint16_t>(out, DUEL_STATE_META_RECORD_LEN);
	vector_write<uint32_t>(out, view);
	vector_write<uint32_t>(out, requested_flags);
	vector_write<uint32_t>(out, pfield->core.duel_rule);
	vector_write<uint32_t>(out, pfield->core.duel_options);
	vector_write<uint32_t>(out, pfield->infos.turn_id);
	vector_write<uint32_t>(out, pfield->infos.phase);
	vector_write<uint32_t>(out, pfield->infos.turn_player);
	vector_write<uint32_t>(out, pfield->infos.priorities[0]);
	vector_write<uint32_t>(out, pfield->infos.priorities[1]);
	vector_write<uint32_t>(out, pfield->infos.can_shuffle);
	vector_write<uint32_t>(out, (uint32_t)pfield->core.current_chain.size());
	vector_write<uint32_t>(out, pfield->core.win_player);
	vector_write<uint32_t>(out, pfield->core.win_reason);
	vector_write<uint32_t>(out, pfield->core.to_bp);
	vector_write<uint32_t>(out, pfield->core.to_m2);
	vector_write<uint32_t>(out, pfield->core.to_ep);
}

void state_write_players(std::vector<byte>& out, field* pfield) {
	vector_write<uint16_t>(out, 2);
	vector_write<uint16_t>(out, DUEL_STATE_PLAYER_RECORD_LEN);
	for(uint8_t playerid = 0; playerid < 2; ++playerid) {
		const auto& player = pfield->player[playerid];
		vector_write<uint32_t>(out, playerid);
		vector_write<int32_t>(out, player.lp);
		vector_write<int32_t>(out, player.start_count);
		vector_write<int32_t>(out, player.draw_count);
		vector_write<uint32_t>(out, player.used_location);
		vector_write<uint32_t>(out, player.disabled_location);
		vector_write<uint32_t>(out, player.extra_p_count);
		vector_write<int32_t>(out, player.szone_size);
		vector_write<uint32_t>(out, (uint32_t)player.list_main.size());
		vector_write<uint32_t>(out, (uint32_t)player.list_hand.size());
		vector_write<uint32_t>(out, (uint32_t)player.list_mzone.size());
		vector_write<uint32_t>(out, (uint32_t)player.list_szone.size());
		vector_write<uint32_t>(out, (uint32_t)player.list_grave.size());
		vector_write<uint32_t>(out, (uint32_t)player.list_remove.size());
		vector_write<uint32_t>(out, (uint32_t)player.list_extra.size());
		vector_write<uint32_t>(out, player.tag_extra_p_count);
	}
}

void state_write_cards(std::vector<byte>& out,
			const std::vector<state_card_entry>& cards,
			const std::map<card*, uint32_t>& refs,
			uint32_t requested_flags) {
	vector_write<uint16_t>(out, (uint16_t)cards.size());
	vector_write<uint16_t>(out, DUEL_STATE_CARD_RECORD_LEN);
	size_t body_start = out.size();
	for(size_t i = 0; i < cards.size(); ++i) {
		const auto& entry = cards[i];
		card* pcard = entry.pcard;
		bool visible = entry.identity_visible;
		uint8_t controller = entry.host ? entry.host->current.controler
			: pcard->current.controler;
		uint8_t location = entry.host
			? (uint8_t)(LOCATION_OVERLAY | entry.host->current.location)
			: pcard->current.location;
		uint8_t sequence = (uint8_t)entry.canonical_sequence;
		uint8_t position = entry.host ? POS_FACEUP : pcard->current.position;
		uint32_t info = controller | ((uint32_t)location << 8)
			| ((uint32_t)sequence << 16) | ((uint32_t)position << 24);
		uint32_t flags = (visible ? DUEL_STATE_CARD_IDENTITY_VISIBLE : 0)
			| (entry.host ? DUEL_STATE_CARD_OVERLAY : 0)
			| (entry.own_knowledge ? DUEL_STATE_CARD_OWN_KNOWLEDGE : 0)
			| (visible && !(location == LOCATION_DECK)
				? DUEL_STATE_CARD_SEQUENCE_VISIBLE : 0)
			| (!visible ? DUEL_STATE_CARD_ANONYMOUS : 0);
		std::pair<int32_t, int32_t> atk_def(0, 0);
		std::pair<int32_t, int32_t> base_atk_def(0, 0);
		if(visible) {
			atk_def = pcard->get_atk_def();
			base_atk_def = pcard->get_base_atk_def();
		}

		vector_write<uint32_t>(out, (uint32_t)i + 1);
		vector_write<uint32_t>(out, info);
		vector_write<uint32_t>(out, visible ? pcard->data.code : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_code() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_another_code() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_type() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_level() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_rank() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_link() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_lscale() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_rscale() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_attribute() : 0);
		vector_write<uint32_t>(out, visible ? pcard->get_race() : 0);
		vector_write<int32_t>(out, atk_def.first);
		vector_write<int32_t>(out, atk_def.second);
		vector_write<int32_t>(out, base_atk_def.first);
		vector_write<int32_t>(out, base_atk_def.second);
		vector_write<uint32_t>(out, pcard->owner);
		vector_write<uint32_t>(out, visible ? pcard->summon_player : PLAYER_NONE);
		vector_write<uint32_t>(out, visible ? pcard->summon_info : 0);
		vector_write<uint32_t>(out, visible ? pcard->status : 0);
		vector_write<uint32_t>(out, visible ? pcard->attack_announce_count : 0);
		vector_write<uint32_t>(out, visible ? pcard->direct_attackable : 0);
		vector_write<uint32_t>(out, visible ? pcard->announce_count : 0);
		vector_write<uint32_t>(out, visible ? pcard->attacked_count : 0);
		vector_write<uint32_t>(out, visible ? pcard->attack_all_target : 0);
		vector_write<uint32_t>(out, visible ? pcard->attack_controler : PLAYER_NONE);
		vector_write<uint32_t>(out, visible ? pcard->turnid : 0);
		vector_write<uint32_t>(out, visible ? pcard->turn_counter : 0);
		vector_write<uint32_t>(out, entry.host ? refs.at(entry.host) : 0);
		vector_write<uint32_t>(out, flags);
		// Stable only inside one duel.  This is teacher metadata for matching a
		// moved card across snapshots; actor-facing calls receive zero.
		vector_write<uint64_t>(out,
			(requested_flags & DUEL_STATE_FLAG_TARGET_ENTITY_IDS)
				? pcard->cardid : 0);
	}
	state_patch_record_len(out, body_start, cards.size());
}

uint32_t state_visible_ref(card* pcard,
			const std::map<card*, uint32_t>& refs,
			const std::map<card*, bool>& visible) {
	if(!pcard)
		return 0;
	auto ref = refs.find(pcard);
	auto seen = visible.find(pcard);
	if(ref == refs.end() || seen == visible.end() || !seen->second)
		return 0;
	return ref->second;
}

void state_write_relations(std::vector<byte>& out,
			const std::vector<state_card_entry>& cards,
			const std::map<card*, uint32_t>& refs,
			const std::map<card*, bool>& visible) {
	struct relation_row {
		uint32_t kind;
		uint32_t source;
		uint32_t target;
		uint32_t reset;
		uint32_t count;
		uint32_t aux0;
		uint32_t aux1;
		uint32_t reserved;
	};
	std::vector<relation_row> rows;
	auto add = [&rows, &refs, &visible](uint32_t kind, card* source,
			card* target, uint32_t reset = 0, uint32_t count = 0,
			uint32_t aux0 = 0, uint32_t aux1 = 0) {
		uint32_t sref = state_visible_ref(source, refs, visible);
		uint32_t tref = state_visible_ref(target, refs, visible);
		if(sref && tref)
			rows.push_back({ kind, sref, tref, reset, count, aux0, aux1, 0 });
	};

	for(const auto& entry : cards) {
		card* source = entry.pcard;
		if(entry.host)
			add(DUEL_STATE_REL_OVERLAY, entry.host, source, 0,
				entry.host_ordinal + 1);
		add(DUEL_STATE_REL_EQUIP, source, source->equiping_target);
		for(card* target : source->material_cards)
			add(DUEL_STATE_REL_SUMMON_MATERIAL, source, target);
		for(card* target : source->effect_target_cards)
			add(DUEL_STATE_REL_EFFECT_TARGET, source, target);
		for(const auto& relation : source->relations)
			add(DUEL_STATE_REL_GENERIC, source, relation.first, relation.second);
		add(DUEL_STATE_REL_REASON_CARD, source, source->current.reason_card,
			source->current.reason);
		for(const auto& attack : source->announced_cards)
			add(DUEL_STATE_REL_ATTACK_ANNOUNCED, source,
				attack.second.first, 0, attack.second.second);
		for(const auto& attack : source->attacked_cards)
			add(DUEL_STATE_REL_ATTACKED, source,
				attack.second.first, 0, attack.second.second);
		for(const auto& attack : source->battled_cards)
			add(DUEL_STATE_REL_BATTLED, source,
				attack.second.first, 0, attack.second.second);
	}
	std::sort(rows.begin(), rows.end(), [](const relation_row& a,
			const relation_row& b) {
		if(a.kind != b.kind)
			return a.kind < b.kind;
		if(a.source != b.source)
			return a.source < b.source;
		if(a.target != b.target)
			return a.target < b.target;
		return a.count < b.count;
	});
	vector_write<uint16_t>(out, (uint16_t)rows.size());
	vector_write<uint16_t>(out, DUEL_STATE_RELATION_RECORD_LEN);
	for(const auto& row : rows) {
		vector_write<uint32_t>(out, row.kind);
		vector_write<uint32_t>(out, row.source);
		vector_write<uint32_t>(out, row.target);
		vector_write<uint32_t>(out, row.reset);
		vector_write<uint32_t>(out, row.count);
		vector_write<uint32_t>(out, row.aux0);
		vector_write<uint32_t>(out, row.aux1);
		vector_write<uint32_t>(out, row.reserved);
	}
}

void state_collect_field_effects(const effect_container& source,
			uint8_t container, std::vector<state_effect_entry>& out,
			std::set<effect*>& seen) {
	for(const auto& item : source) {
		effect* peffect = item.second;
		if(peffect && peffect->owner && peffect->is_flag(EFFECT_FLAG_FIELD_ONLY)
				&& seen.insert(peffect).second)
			out.push_back({ peffect, container });
	}
}

void state_collect_card_effects(const effect_container& source,
			uint8_t container, std::vector<state_effect_entry>& out,
			std::set<effect*>& seen) {
	for(const auto& item : source) {
		effect* peffect = item.second;
		if(!peffect || !peffect->owner)
			continue;
		// INITIAL describes static Lua semantics, but the same object also owns
		// mutable runtime state.  Export only an INITIAL row whose mutable fields
		// are non-default/consumed; exporting every Deck card's static effects
		// would duplicate the embedding and swamp the sparse state set.
		bool dynamic_initial = peffect->is_flag(EFFECT_FLAG_INITIAL)
			&& (peffect->count_limit != peffect->count_limit_max
				|| peffect->status || peffect->active_handler
				|| peffect->last_handler || peffect->cost_checked);
		if((!peffect->is_flag(EFFECT_FLAG_INITIAL) || dynamic_initial)
				&& seen.insert(peffect).second)
			out.push_back({ peffect, container });
	}
}

void state_collect_effects(field* pfield,
			const std::vector<state_card_entry>& cards,
			const std::map<card*, uint32_t>& refs,
			const std::map<card*, bool>& visible,
			std::vector<state_effect_entry>& effects) {
	std::set<effect*> seen;
	state_collect_field_effects(pfield->effects.aura_effect,
		EFFECT_INFO_CONTAINER_AURA, effects, seen);
	state_collect_field_effects(pfield->effects.ignition_effect,
		EFFECT_INFO_CONTAINER_IGNITION, effects, seen);
	state_collect_field_effects(pfield->effects.activate_effect,
		EFFECT_INFO_CONTAINER_ACTIVATE, effects, seen);
	state_collect_field_effects(pfield->effects.trigger_o_effect,
		EFFECT_INFO_CONTAINER_TRIGGER_O, effects, seen);
	state_collect_field_effects(pfield->effects.trigger_f_effect,
		EFFECT_INFO_CONTAINER_TRIGGER_F, effects, seen);
	state_collect_field_effects(pfield->effects.quick_o_effect,
		EFFECT_INFO_CONTAINER_QUICK_O, effects, seen);
	state_collect_field_effects(pfield->effects.quick_f_effect,
		EFFECT_INFO_CONTAINER_QUICK_F, effects, seen);
	state_collect_field_effects(pfield->effects.continuous_effect,
		EFFECT_INFO_CONTAINER_CONTINUOUS, effects, seen);
	for(const auto& entry : cards) {
		card* pcard = entry.pcard;
		state_collect_card_effects(pcard->single_effect,
			EFFECT_INFO_CONTAINER_CARD_SINGLE, effects, seen);
		state_collect_card_effects(pcard->field_effect,
			EFFECT_INFO_CONTAINER_CARD_FIELD, effects, seen);
		state_collect_card_effects(pcard->equip_effect,
			EFFECT_INFO_CONTAINER_CARD_EQUIP, effects, seen);
		state_collect_card_effects(pcard->target_effect,
			EFFECT_INFO_CONTAINER_CARD_TARGET, effects, seen);
		state_collect_card_effects(pcard->xmaterial_effect,
			EFFECT_INFO_CONTAINER_CARD_XMATERIAL, effects, seen);
	}
	// An effect can only enter a player view when its semantic source card is
	// visible.  This is what prevents a hidden Droll in hand from leaking via
	// the global watcher its initial_effect registered at load time.
	effects.erase(std::remove_if(effects.begin(), effects.end(),
		[&refs, &visible](const state_effect_entry& entry) {
			return !state_visible_ref(entry.peffect->owner, refs, visible);
		}), effects.end());
	std::sort(effects.begin(), effects.end(), [](const state_effect_entry& a,
			const state_effect_entry& b) {
		if(a.peffect->id != b.peffect->id)
			return a.peffect->id < b.peffect->id;
		return a.container < b.container;
	});
}

void state_write_effects(std::vector<byte>& out,
			const std::vector<state_effect_entry>& effects,
			const std::map<card*, uint32_t>& refs,
			const std::map<card*, bool>& visible,
			std::map<effect*, uint32_t>& effect_refs,
			uint32_t requested_flags) {
	vector_write<uint16_t>(out, (uint16_t)effects.size());
	vector_write<uint16_t>(out, DUEL_STATE_EFFECT_RECORD_LEN);
	size_t body_start = out.size();
	for(size_t i = 0; i < effects.size(); ++i) {
		effect* peffect = effects[i].peffect;
		uint32_t effect_ref = (uint32_t)i + 1;
		effect_refs[peffect] = effect_ref;
		uint8_t targets = 0;
		if(peffect->is_target_player(0))
			targets |= 1;
		if(peffect->is_target_player(1))
			targets |= 2;
		vector_write<uint32_t>(out, effect_ref);
		vector_write<uint32_t>(out, peffect->code);
		vector_write<uint32_t>(out, peffect->type);
		vector_write<uint32_t>(out,
			state_visible_ref(peffect->owner, refs, visible));
		vector_write<uint32_t>(out,
			state_visible_ref(peffect->get_handler(), refs, visible));
		vector_write<uint32_t>(out, peffect->description);
		vector_write<uint32_t>(out, peffect->reset_flag);
		vector_write<int32_t>(out, peffect->reset_count);
		vector_write<uint32_t>(out, peffect->count_code);
		vector_write<uint32_t>(out, peffect->active_type);
		vector_write<uint32_t>(out, (uint32_t)peffect->category);
		vector_write<uint32_t>(out, (uint32_t)(peffect->category >> 32));
		vector_write<uint32_t>(out, (uint32_t)peffect->flag[0]);
		vector_write<uint32_t>(out, (uint32_t)(peffect->flag[0] >> 32));
		vector_write<uint32_t>(out, (uint32_t)peffect->flag[1]);
		vector_write<uint32_t>(out, (uint32_t)(peffect->flag[1] >> 32));
		vector_write<uint32_t>(out, peffect->s_range);
		vector_write<uint32_t>(out, peffect->o_range);
		vector_write<uint32_t>(out, peffect->range);
		vector_write<uint32_t>(out, peffect->status);
		vector_write<uint32_t>(out, effects[i].container);
		vector_write<uint32_t>(out, peffect->effect_owner);
		vector_write<uint32_t>(out, targets);
		vector_write<uint32_t>(out, peffect->count_limit);
		vector_write<uint32_t>(out, peffect->count_limit_max);
		vector_write<uint32_t>(out,
			(uint32_t)std::min<size_t>(peffect->label.size(), UINT32_MAX));
		vector_write<uint32_t>(out, peffect->object_type);
		vector_write<uint32_t>(out, peffect->copy_id);
		vector_write<uint32_t>(out, peffect->active_location);
		vector_write<uint32_t>(out, peffect->active_sequence);
		vector_write<uint32_t>(out,
			state_visible_ref(peffect->active_handler, refs, visible));
		vector_write<uint32_t>(out,
			state_visible_ref(peffect->last_handler, refs, visible));
		vector_write<uint32_t>(out,
			(requested_flags & DUEL_STATE_FLAG_TARGET_ENTITY_IDS)
				? peffect->id : 0);
	}
	state_patch_record_len(out, body_start, effects.size());
}

void state_write_effect_objects(std::vector<byte>& out,
			const std::vector<state_effect_entry>& effects,
			const std::vector<state_card_entry>& cards,
			const std::map<card*, uint32_t>& refs,
			const std::map<card*, bool>& visible,
			const std::map<effect*, uint32_t>& effect_refs) {
	struct object_row {
		uint32_t effect_ref;
		uint32_t object_type;
		uint32_t card_ref;
		uint32_t aux_ref;
	};
	std::vector<object_row> rows;
	for(const auto& entry : effects) {
		effect* peffect = entry.peffect;
		if(!peffect->label_object)
			continue;
		uint32_t effect_ref = effect_refs.at(peffect);
		if(peffect->object_type == PARAM_TYPE_CARD) {
			for(const auto& candidate : cards) {
				if(candidate.pcard->ref_handle == peffect->label_object) {
					uint32_t card_ref = state_visible_ref(candidate.pcard, refs, visible);
					if(card_ref)
						rows.push_back({ effect_ref, PARAM_TYPE_CARD, card_ref, 0 });
					break;
				}
			}
		}
	}
	vector_write<uint16_t>(out, (uint16_t)rows.size());
	vector_write<uint16_t>(out, DUEL_STATE_EFFECT_OBJECT_RECORD_LEN);
	for(const auto& row : rows) {
		vector_write<uint32_t>(out, row.effect_ref);
		vector_write<uint32_t>(out, row.object_type);
		vector_write<uint32_t>(out, row.card_ref);
		vector_write<uint32_t>(out, row.aux_ref);
	}
}

} // namespace

int32_t query_duel_state(intptr_t pduel, uint8_t view, uint32_t flags,
			byte* buf, int32_t buf_size) {
	if(!pduel || !buf || buf_size <= 0 || !state_view_valid(view))
		return LEN_FAIL;
	field* pfield = ((duel*)pduel)->game_field;
	std::vector<state_card_entry> cards;
	state_collect_zone_cards(pfield, view, cards);
	std::map<card*, uint32_t> refs;
	std::map<card*, bool> visible;
	for(size_t i = 0; i < cards.size(); ++i) {
		refs[cards[i].pcard] = (uint32_t)i + 1;
		visible[cards[i].pcard] = cards[i].identity_visible;
	}
	std::vector<state_effect_entry> effects;
	state_collect_effects(pfield, cards, refs, visible, effects);
	std::map<effect*, uint32_t> effect_refs;
	std::vector<byte> out;
	out.reserve(8192);
	vector_write<uint8_t>(out, DUEL_STATE_VERSION);
	vector_write<uint8_t>(out, DUEL_STATE_HEADER_LEN);
	vector_write<uint16_t>(out, 0);
	vector_write<uint32_t>(out, 0);

	auto begin_section = [&out](uint8_t id) {
		vector_write<uint8_t>(out, id);
		vector_write<uint32_t>(out, 0);
		return out.size();
	};
	auto end_section = [&out](size_t payload_start) {
		uint32_t len = (uint32_t)(out.size() - payload_start);
		std::memcpy(out.data() + payload_start - sizeof(uint32_t), &len,
			sizeof(len));
	};

	size_t mark = begin_section(DUEL_STATE_SECTION_META);
	state_write_meta(out, pfield, view, flags);
	end_section(mark);
	mark = begin_section(DUEL_STATE_SECTION_PLAYER);
	state_write_players(out, pfield);
	end_section(mark);
	mark = begin_section(DUEL_STATE_SECTION_CARD);
	state_write_cards(out, cards, refs, flags);
	end_section(mark);
	mark = begin_section(DUEL_STATE_SECTION_RELATION);
	state_write_relations(out, cards, refs, visible);
	end_section(mark);
	mark = begin_section(DUEL_STATE_SECTION_EFFECT);
	state_write_effects(out, effects, refs, visible, effect_refs, flags);
	end_section(mark);
	mark = begin_section(DUEL_STATE_SECTION_EFFECT_OBJECT);
	state_write_effect_objects(out, effects, cards, refs, visible, effect_refs);
	end_section(mark);
	vector_write<uint8_t>(out, DUEL_STATE_SECTION_END);
	vector_write<uint32_t>(out, 0);

	uint32_t total = (uint32_t)out.size();
	std::memcpy(out.data() + 4, &total, sizeof(total));
	if((int32_t)out.size() > buf_size)
		return LEN_FAIL;
	std::memcpy(buf, out.data(), out.size());
	return (int32_t)out.size();
}
