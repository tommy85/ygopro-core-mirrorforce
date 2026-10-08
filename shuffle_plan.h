#ifndef MF_SHUFFLE_PLAN_H_
#define MF_SHUFFLE_PLAN_H_

#include <cstdint>
#include <vector>

class card;
class duel;

// An opt-in closed plan for a detached hypothetical duel. All backing stores
// are allocated while the owning duel's arena is entered, including receipts.
// It is not a rule bypass: only genuinely random permutations can be replaced;
// deterministic anonymous rearrangements are checked without modification.
struct mf_shuffle_entry {
	uint32_t message, player, location;
	std::vector<uint32_t> sequences, from, codes;
};
struct mf_shuffle_receipt {
	uint32_t flavor{ 0 };
	std::vector<uint64_t> before, after;
};
struct mf_shuffle_plan {
	static constexpr uint32_t VERSION = 1;
	static constexpr uint32_t MAX_ENTRIES = 4096;
	static constexpr uint32_t MAX_PARTICIPANTS = 262144;
	// 0 never installed, 1 active, 2 successfully finished. No reset/reinstall.
	uint32_t mode{ 0 }, cursor{ 0 }, fault{ 0 };
	uint32_t last_message{ 0 }, last_player{ 0 }, last_location{ 0 }, last_count{ 0 };
	std::vector<mf_shuffle_entry> entries;
	std::vector<mf_shuffle_receipt> receipts;
	enum fault_code : uint32_t {
		NONE = 0, EXTRA_EVENT = 1, EVENT_SHAPE = 2, SOURCE_CODE = 3,
		NOT_A_BIJECTION = 4, DETERMINISTIC_RESULT = 5, LEGACY_MIX = 6,
		UNCONSUMED = 7, DUEL_CLEAR = 8
	};
	bool active() const { return mode == 1; }
	void fail(uint32_t code) { if(!fault) fault = code; }
	// before/after are indexed by the same sorted physical sequence vector.
	// flavor: 1 ordinary random, 2 ordinary deterministic, 3 Lua field random,
	// 4 deterministic set-group placement. Only flavors 1 and 3 may be replaced.
	bool consume(duel* owner, uint32_t message, uint32_t player, uint32_t location,
	             const std::vector<uint32_t>& sequences, const std::vector<card*>& before,
	             std::vector<card*>& after, uint32_t flavor);
};

#endif
