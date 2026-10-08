/* Opt-in observation of uncertainty reads in a local hypothetical duel.
 * No rule result is changed. Fixed storage lives in the duel's snapshot arena.
 * The caller must supply local card IDs derived from its public uncertainty.
 * This records covered accesses; it is not a general noninterference proof.
 */
#ifndef MF_TACTICAL_AUDIT_H_
#define MF_TACTICAL_AUDIT_H_

#include <algorithm>
#include <cstdint>
#include <limits>

struct mf_tactical_audit {
	static constexpr uint32_t CAPACITY = 512;
	static constexpr uint32_t COUNTERS = 5;
	enum access : uint32_t { LUA_CARD = 0, LUA_GROUP = 1, LUA_EFFECT = 2, CARD_IDENTITY = 3, RANDOM = 4 };
	bool enabled{ false };
	uint32_t count{ 0 };
	uint64_t uids[CAPACITY]{};
	uint64_t hits[COUNTERS]{};
	// v2 distinguishes an unobserved own-deck shuffle from an actual read of
	// its order. Card identities in that deck are already known as a multiset.
	int32_t order_player{ -1 };
	uint64_t order_reads{ 0 };
	uint64_t own_deck_shuffles{ 0 };
	uint64_t other_random{ 0 };

	static void increment(uint64_t& value) {
		if(value != std::numeric_limits<uint64_t>::max())
			++value;
	}

	void hit(access kind) {
		if(enabled && hits[kind] != std::numeric_limits<uint64_t>::max())
			++hits[kind];
	}
	void card(uint64_t uid, access kind) {
		if(enabled && uid && std::binary_search(uids, uids + count, uid))
			hit(kind);
	}
	void order(uint32_t player, uint32_t location = 1) {
		if(enabled && order_player >= 0 && player == (uint32_t)order_player && location == 1)
			increment(order_reads);
	}
	void random(bool own_deck_shuffle = false) {
		if(!enabled)
			return;
		hit(RANDOM);
		increment(own_deck_shuffle ? own_deck_shuffles : other_random);
	}
};

#endif
