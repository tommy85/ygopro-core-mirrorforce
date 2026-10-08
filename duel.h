/*
 * duel.h
 *
 *  Created on: 2010-4-8
 *      Author: Argon
 */

#ifndef DUEL_H_
#define DUEL_H_

#include "common.h"
#include "sort.h"
#include "mtrandom.h"
#include "shuffle_plan.h"
#include "tactical_audit.h"
#include <set>
#include <unordered_set>
#include <vector>

class card;
class group;
class effect;
class field;
class interpreter;

using card_set = std::set<card*, card_sort>;

class duel {
public:
	struct phase_pass_record {
		uint32_t message_offset;
		uint32_t turn;
		uint16_t phase;
		uint8_t player;
		uint8_t priority_passed;
	};
	char strbuffer[256]{};
	int32_t rng_version{ 2 };
	interpreter* lua;
	field* game_field;
	mtrandom random;
	mf_shuffle_plan shuffle_plan;
	mf_tactical_audit tactical_audit;

	std::vector<byte> message_buffer;
	std::unordered_set<card*> cards;
	std::unordered_set<card*> assumes;
	std::unordered_set<group*> groups;
	std::unordered_set<group*> sgroups;
	// Weak script temporaries the collector found unreachable, waiting for the next deterministic point to be
	// deleted; how many were deleted there so far (diagnostics); and a tripwire: no finalizer may run while
	// release_script_group walks sgroups.
	std::vector<group*> collected_groups;
	uint64_t groups_collected{ 0 };
	bool releasing_groups{ false };
#ifdef MF_GROUP_AUDIT
	// diagnostic builds: how often the holds audit ran, how many full collections the weak-group debt forced, and
	// how many groups finalizers marked collected (reported at end_duel, see MF_CORE_AUDIT_REPORT)
	uint64_t group_audits{ 0 };
	uint64_t group_full_collections{ 0 };
	uint64_t groups_marked{ 0 };
#endif
	std::unordered_set<effect*> effects;
	std::unordered_set<effect*> uncopy;
	// Dormant identities (belief-world search): one card object per code of the run's registered load-side-effect
	// table, created at location 0 before any deck card (duel_create_dormant), so every duel-level registration a
	// card's initial_effect makes -- guarded watchers, activity counters, global flags -- is made once, at duel
	// creation, independently of both decks. Card ids below dormant_end are dormant. While the setting is on, a card
	// whose code is outside the table and that registers at duel level while it initializes is a script error.
	bool dormant_identities{ false };
	std::vector<uint32_t> dormant_codes;  // sorted
	uint64_t dormant_end{ 0 };
	card* initializing{ nullptr };       // the card whose initial_effect runs (register_card, hydrate, replace)
	bool dormant_code(uint32_t code) const;
	// a duel-level registration by the initializing card: refused (false) when it is outside the table
	bool dormant_allows_registration() const;
	// Opt-in local shadow audit; no protocol or rule branch changes.
	bool phase_pass_audit_enabled{ false };
	bool phase_pass_audit_overflow{ false };
	bool phase_pass_audit_process_seen{ false };
	uint64_t phase_pass_audit_batch{ 0 };
	uint32_t phase_pass_audit_count{ 0 };
	static constexpr uint32_t PHASE_PASS_AUDIT_CAPACITY = 64;
	phase_pass_record phase_pass_audit[PHASE_PASS_AUDIT_CAPACITY]{};
	// Random outcomes a following client read from the public stream (opt-in, local only). Coin and dice tosses
	// take forced_outcomes in order, Group.RandomSelect takes forced_selects in order; the natural random words are
	// drawn all the same, so nothing else in the batch changes. An entry that does not fit is dropped and counted.
	struct forced_select {
		uint32_t count;
		uint32_t locations[8];
		uint32_t codes[8];  // a card named by its code (0: by its location)
	};
	static constexpr uint32_t FORCED_OUTCOME_CAPACITY = 64;
	static constexpr uint32_t FORCED_SELECT_CAPACITY = 16;
	uint8_t forced_outcomes[FORCED_OUTCOME_CAPACITY]{};
	uint32_t forced_outcome_head{ 0 };
	uint32_t forced_outcome_count{ 0 };
	forced_select forced_selects[FORCED_SELECT_CAPACITY]{};
	uint32_t forced_select_head{ 0 };
	uint32_t forced_select_count{ 0 };
	uint32_t forced_random_misses{ 0 };
	// Activations a following client saw the opponent make but cannot prove legal locally: their legality reads
	// hidden cards the local duel holds only as blank placeholders (opt-in, local only). An activation is named as
	// the stream shows it -- the controller, the card's code and the effect's description -- so it holds wherever
	// the card stands and whenever its identity is written. A forced effect's script checks (condition, cost,
	// target) still run, for what they set, but their results do not decide; the core's own checks (count limit,
	// location, position, timing) still do. Pending until an effect it names is chained; then a forced link until
	// that link is solved, during which blank placeholders in its player's hidden zones pass the script's card
	// filters (the card the opponent took there is not public). Duel state: rolls back with snapshots.
	struct forced_activation {
		uint32_t code;
		uint32_t description;
		uint8_t player;
	};
	static constexpr uint32_t FORCED_ACTIVATION_CAPACITY = 8;
	forced_activation forced_activations[FORCED_ACTIVATION_CAPACITY]{};
	uint32_t forced_activation_count{ 0 };
	effect* forced_links[FORCED_ACTIVATION_CAPACITY]{};
	uint32_t forced_link_count{ 0 };
	bool is_forced_activation(const effect* peffect) const;
	bool is_forced_link(const effect* peffect) const;
	bool blank_passes_filter(const card* pcard) const;
	void chain_forced(effect* peffect);
	void solve_forced(effect* peffect);
	void drop_forced(effect* peffect);
	
	duel();
	~duel();
	void clear();
	
	uint32_t buffer_size() const {
		return (uint32_t)message_buffer.size() & PROCESSOR_BUFFER_LEN;
	}
	card* new_card(uint32_t code);
	group* new_group();
	group* new_group(card* pcard);
	group* new_group(const card_set& cset);
	effect* new_effect();
	void delete_card(card* pcard);
	void delete_group(group* pgroup);
	void mark_collected(group* pgroup);
	void free_collected_groups();
	int32_t audit_group_holds(char* what, size_t size);
	void delete_effect(effect* peffect);
	void release_script_group();
	void restore_assumes();
	int32_t read_buffer(byte* buf);
	void write_buffer(const void* data, size_t size);
	void write_buffer32(uint32_t value);
	void write_buffer16(uint16_t value);
	void write_buffer8(uint8_t value);
	void clear_buffer();
	void set_responsei(int32_t resp);
	void set_responseb(byte* resp);
	int32_t get_next_integer(int32_t l, int32_t h);
	int32_t get_next_outcome(int32_t l, int32_t h);
	const forced_select* next_forced_select();
private:
	group* register_group(group* pgroup);
};

#endif /* DUEL_H_ */
