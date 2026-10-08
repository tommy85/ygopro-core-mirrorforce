#include "shuffle_plan.h"
#include "card.h"
#include "duel.h"
#include "field.h"
#include "interpreter.h"
#include "mfsnap.h"
#include "ocgapi.h"
#include <algorithm>

bool mf_shuffle_plan::consume(duel* owner, uint32_t message, uint32_t player, uint32_t location,
        const std::vector<uint32_t>& sequences, const std::vector<card*>& before,
        std::vector<card*>& after, uint32_t flavor) {
	if(!active() || fault)
		return false;
	last_message = message;
	last_player = player;
	last_location = location;
	last_count = (uint32_t)before.size();
	if(cursor == entries.size()) {
		fail(EXTRA_EVENT);
		return false;
	}
	const auto& plan = entries[cursor];
	if(message != plan.message || player != plan.player || location != plan.location
	   || sequences != plan.sequences || before.size() != sequences.size() || after.size() != before.size()
	   || flavor < 1 || flavor > 4) {
		fail(EVENT_SHAPE);
		return false;
	}
	// Every input/output is one live entity of this hypothetical duel; neither
	// a same-code replacement nor a repeated pointer counts as a permutation.
	for(size_t i = 0; i < before.size(); ++i) {
		card* source = before[i];
		if(!source || source->pduel != owner || source->current.controler != player
		   || source->current.location != location || !source->cardid) {
			fail(NOT_A_BIJECTION);
			return false;
		}
		if(source->data.code != plan.codes[i]) {
			fail(SOURCE_CODE);
			return false;
		}
	}
	for(size_t i = 0; i < before.size(); ++i) {
		card* source = before[i];
		unsigned matches = 0;
		for(size_t j = 0; j < before.size(); ++j) {
			if((i != j && (source == before[j] || source->cardid == before[j]->cardid))
			   || (i != j && after[i] == after[j])) {
				fail(NOT_A_BIJECTION);
				return false;
			}
			matches += source == after[j];
		}
		if(matches != 1) {
			fail(NOT_A_BIJECTION);
			return false;
		}
	}
	if(flavor == 2 || flavor == 4) {
		for(size_t i = 0; i < after.size(); ++i)
			if(after[i] != before[plan.from[i]]) {
				fail(DETERMINISTIC_RESULT);
				return false;
			}
	} else {
		for(size_t i = 0; i < after.size(); ++i)
			after[i] = before[plan.from[i]];
	}
	// This storage was fully sized by install inside the arena. No callback,
	// allocation, RNG draw or mutation of any card/effect/Lua object occurs here.
	auto& receipt = receipts[cursor];
	receipt.flavor = flavor;
	for(size_t i = 0; i < before.size(); ++i) {
		receipt.before[i] = before[i]->cardid;
		receipt.after[i] = after[i]->cardid;
	}
	++cursor;
	return true;
}

static duel* plan_duel(intptr_t pduel) {
	if(mfsnap_current() || !pduel || !mfsnap_arena_of(pduel))
		return nullptr;
	duel* pd = reinterpret_cast<duel*>(pduel);
	if(pd->lua->current_state != pd->lua->lua_state || pd->lua->no_action || !pd->lua->params.empty())
		return nullptr;
	return pd;
}

int32_t duel_shuffle_plan_install(intptr_t pduel, const uint32_t* words, int32_t nwords) {
	duel* pd = plan_duel(pduel);
	if(!pd)
		return -1;
	if(pd->shuffle_plan.mode || pd->phase_pass_audit_process_seen)
		return -3;
	if(!words || nwords < 2 || words[0] != mf_shuffle_plan::VERSION
	   || words[1] > mf_shuffle_plan::MAX_ENTRIES
	   || (uint32_t)nwords > 2 + 4 * mf_shuffle_plan::MAX_ENTRIES + 3 * mf_shuffle_plan::MAX_PARTICIPANTS)
		return -2;
	size_t offset = 2, participants = 0;
	for(uint32_t event = 0; event < words[1]; ++event) {
		if(offset + 4 > (size_t)nwords)
			return -2;
		uint32_t message = words[offset], player = words[offset + 1], location = words[offset + 2], n = words[offset + 3];
		const bool field = message == MSG_SHUFFLE_SET_CARD;
		if(player > 1 || n > 255 || (n == 0 && message != MSG_SHUFFLE_EXTRA)
		   || (field ? ((location != LOCATION_MZONE && location != LOCATION_SZONE) || n < 2 || n > 5)
		             : !((message == MSG_SHUFFLE_HAND && location == LOCATION_HAND)
		                 || (message == MSG_SHUFFLE_DECK && location == LOCATION_DECK)
		                 || (message == MSG_SHUFFLE_EXTRA && location == LOCATION_EXTRA))))
			return -2;
		participants += n;
		if(participants > mf_shuffle_plan::MAX_PARTICIPANTS || offset + 4 + 3 * n > (size_t)nwords)
			return -2;
		bool used[256]{};
		for(uint32_t i = 0; i < n; ++i) {
			uint32_t sequence = words[offset + 4 + i], from = words[offset + 4 + n + i], code = words[offset + 4 + 2 * n + i];
			if((field ? (sequence > 4 || (i && sequence <= words[offset + 3 + i])) : sequence != i)
			   || from >= n || used[from] || !code || code >= 0x80000000u
			   || (code >= 999000001u && code <= 999000004u))
				return -2;
			used[from] = true;
		}
		offset += 4 + 3 * n;
	}
	if(offset != (size_t)nwords)
		return -2;
	for(unsigned player = 0; player < 2; ++player)
		for(unsigned zone = 0; zone < 3; ++zone)
			if(!pd->game_field->core.forced_shuffle[player][zone].empty())
				return -5;
	void* snapshot = mf_duel_snapshot(pduel);
	if(!snapshot)
		return -6;
	bool copied = true;
	try {
		MfArenaGuard guard(pduel);
		auto& plan = pd->shuffle_plan;
		plan.entries.resize(words[1]);
		plan.receipts.resize(words[1]);
		offset = 2;
		for(uint32_t event = 0; event < words[1]; ++event) {
			auto& row = plan.entries[event];
			row.message = words[offset];
			row.player = words[offset + 1];
			row.location = words[offset + 2];
			uint32_t n = words[offset + 3];
			row.sequences.assign(words + offset + 4, words + offset + 4 + n);
			row.from.assign(words + offset + 4 + n, words + offset + 4 + 2 * n);
			row.codes.assign(words + offset + 4 + 2 * n, words + offset + 4 + 3 * n);
			plan.receipts[event].before.resize(n);
			plan.receipts[event].after.resize(n);
			offset += 4 + 3 * n;
		}
		plan.mode = 1;
	} catch(...) {
		copied = false;
	}
	int32_t result = copied ? 0 : (mf_duel_rollback(pduel, snapshot) == 0 ? -6 : -7);
	mf_snapshot_free(snapshot);
	return result;
}

int32_t duel_shuffle_plan_state(intptr_t pduel, uint32_t* out, int32_t cap) {
	const duel* pd = plan_duel(pduel);
	if(!pd)
		return -1;
	if(!out || cap < 9)
		return -2;
	const auto& plan = pd->shuffle_plan;
	const uint32_t words[9] = { mf_shuffle_plan::VERSION, plan.mode, (uint32_t)plan.entries.size(), plan.cursor,
		plan.fault, plan.last_message, plan.last_player, plan.last_location, plan.last_count };
	std::copy(words, words + 9, out);
	return 0;
}

int32_t duel_shuffle_plan_receipts(intptr_t pduel, uint64_t* out, int32_t cap) {
	const duel* pd = plan_duel(pduel);
	if(!pd)
		return -1;
	const auto& plan = pd->shuffle_plan;
	size_t required = 9;
	for(uint32_t i = 0; i < plan.cursor; ++i)
		required += 6 + 3 * plan.entries[i].sequences.size();
	if(!out && cap == 0)
		return (int32_t)required;
	if(!out || cap < 0 || (size_t)cap < required)
		return -2;
	const uint64_t header[9] = { mf_shuffle_plan::VERSION, plan.mode, plan.entries.size(), plan.cursor,
		plan.fault, plan.last_message, plan.last_player, plan.last_location, plan.last_count };
	std::copy(header, header + 9, out);
	size_t cursor = 9;
	for(uint32_t i = 0; i < plan.cursor; ++i) {
		const auto& entry = plan.entries[i];
		const auto& receipt = plan.receipts[i];
		const uint64_t row[6] = { i, receipt.flavor, entry.message, entry.player, entry.location, entry.sequences.size() };
		std::copy(row, row + 6, out + cursor);
		cursor += 6;
		for(uint32_t sequence : entry.sequences)
			out[cursor++] = sequence;
		for(uint64_t uid : receipt.before)
			out[cursor++] = uid;
		for(uint64_t uid : receipt.after)
			out[cursor++] = uid;
	}
	return (int32_t)required;
}

int32_t duel_shuffle_plan_finish(intptr_t pduel) {
	duel* pd = plan_duel(pduel);
	if(!pd)
		return -1;
	auto& plan = pd->shuffle_plan;
	if(plan.mode != 1)
		return -3;
	if(plan.cursor != plan.entries.size())
		plan.fail(mf_shuffle_plan::UNCONSUMED);
	if(plan.fault)
		return -4;
	plan.mode = 2;
	return 0;
}
