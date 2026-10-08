/*
 * group.h
 *
 *  Created on: 2010-5-6
 *      Author: Argon
 */

#ifndef GROUP_H_
#define GROUP_H_

#include "common.h"
#include "sort.h"
#include <set>
#include <list>

class card;
class duel;

using card_set = std::set<card*, card_sort>;

constexpr uint32_t GTYPE_DEFAULT = 0;
constexpr uint32_t GTYPE_READ_ONLY = 1;
constexpr uint32_t GTYPE_KEEP_ALIVE = 2;

class alignas(8) group {
public:
	// The Lua anchor of this group's userdata: a registry reference while the group is strong, an index into the
	// interpreter's weak-valued group table while it is weak (see interpreter::return_temporary_group). A weak group
	// is a script temporary nothing in C++ holds; Lua's garbage collector may free it once no Lua value refers to it.
	int32_t ref_handle{ 0 };
	uint32_t is_readonly{ GTYPE_DEFAULT };
	bool weak{ false };
	// The userdata block that points at this group, while that block is sure to exist: a strong group's userdata is
	// in the registry, and a weak group's is freed only after its finalizer ran, which clears both pointers (see
	// duel::mark_collected). Deleting the group clears the pointer there, so neither a stale script reference nor a
	// finalizer can reach a freed group.
	group** ud{ nullptr };
	// A weak group Lua's collector found unreachable (its finalizer ran). It is deleted at the next deterministic
	// point (duel::free_collected_groups, or the outermost return), never by the finalizer itself.
	bool collected{ false };
	duel* pduel;
	card_set container;
	card_set::iterator it;
	bool is_iterator_dirty{ true };
	
	bool has_card(card* c) {
		return container.find(c) != container.end();
	}
	
	explicit group(duel* pd);
	group(duel* pd, card* pcard);
	group(duel* pd, const card_set& cset);
	~group() = default;
};

#endif /* GROUP_H_ */
