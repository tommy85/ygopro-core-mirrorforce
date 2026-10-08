/*
 * interpreter.cpp
 *
 *  Created on: 2010-4-28
 *      Author: Argon
 */

#include <cstring>
#include "duel.h"
#include "group.h"
#include "card.h"
#include "effect.h"
#include "scriptlib.h"
#include "ocgapi.h"
#include "interpreter.h"
#include "mfsnap.h"
#include <cstdlib>

interpreter::interpreter(duel* pd, bool enable_unsafe_libraries)
	: coroutines(256), pduel(pd), enable_unsafe_feature(enable_unsafe_libraries) {
	/* the whole Lua heap lives in the duel's arena: snapshots then cover
	 * every coroutine and GC structure by construction, with nothing to
	 * serialize.  Panic mirrors luaL_newstate's abort-on-error contract. */
	lua_state = lua_newstate(mfsnap_lua_alloc, mfsnap_current());
	lua_atpanic(lua_state, [](lua_State* L) -> int {
		(void)L;
		std::abort();
		return 0;
	});
	current_state = lua_state;
	std::memcpy(lua_getextraspace(lua_state), &pd, LUA_EXTRASPACE); //set_duel_info
	//Initial
	luaL_requiref(lua_state, "base", luaopen_base, 0);
	lua_pop(lua_state, 1);
	luaL_requiref(lua_state, "string", luaopen_string, 1);
	lua_pop(lua_state, 1);
	luaL_requiref(lua_state, "utf8", luaopen_utf8, 1);
	lua_pop(lua_state, 1);
	luaL_requiref(lua_state, "table", luaopen_table, 1);
	lua_pop(lua_state, 1);
	luaL_requiref(lua_state, "math", luaopen_math, 1);
	lua_pop(lua_state, 1);
	if (enable_unsafe_libraries) {
		luaL_requiref(lua_state, "io", luaopen_io, 1);
		lua_pop(lua_state, 1);
	}

	auto nil_out = [&](const char* name) {
		lua_pushnil(lua_state);
		lua_setglobal(lua_state, name);
	};
	nil_out("collectgarbage");
	if (!enable_unsafe_libraries) {
		nil_out("dofile");
		nil_out("loadfile");
	}
	// the weak-valued table that anchors script temporaries (see return_temporary_group)
	lua_createtable(lua_state, 0, 0);
	lua_createtable(lua_state, 0, 1);
	lua_pushliteral(lua_state, "v");
	lua_setfield(lua_state, -2, "__mode");
	lua_setmetatable(lua_state, -2);
	weak_groups = luaL_ref(lua_state, LUA_REGISTRYINDEX);
#ifdef MF_GROUP_AUDIT
	mf_group_audit_gc_pressure(lua_state);
#endif
	//open all libs
	scriptlib::open_cardlib(lua_state);
	scriptlib::open_effectlib(lua_state);
	scriptlib::open_grouplib(lua_state);
	scriptlib::open_duellib(lua_state);
	scriptlib::open_debuglib(lua_state);
	//extra scripts
	load_script("./script/constant.lua");
	load_script("./script/utility.lua");
	load_script("./script/procedure.lua");
}
interpreter::~interpreter() {
	lua_close(lua_state);
}
void interpreter::register_card(card *pcard) {
	if (!pcard)
		return;
	//create a card in by userdata
	luaL_checkstack(lua_state, 1, nullptr);
	card ** ppcard = (card**) lua_newuserdata(lua_state, sizeof(card*));	//+1 userdata
	*ppcard = pcard;
	pcard->ref_handle = luaL_ref(lua_state, LUA_REGISTRYINDEX);				//-1
	//some userdata may be created in script like token so use current_state
	luaL_checkstack(current_state, 1, nullptr);
	lua_rawgeti(current_state, LUA_REGISTRYINDEX, pcard->ref_handle);	//+1 userdata
	load_card_script(pcard->data.get_original_code());
	//stack: table cxxx, userdata
	//set metatable of pointer to base script
	lua_setmetatable(current_state, -2);	//-1
	lua_pop(current_state, 1);				//-1
	//Initial
	if(is_load_script(pcard->data)) {
		pcard->set_status(STATUS_INITIALIZING, TRUE);
		card* outer = pduel->initializing;
		pduel->initializing = pcard;
		add_param(pcard, PARAM_TYPE_CARD);
		call_card_function(pcard, "initial_effect", 1, 0);
		pduel->initializing = outer;
		pcard->set_status(STATUS_INITIALIZING, FALSE);
	}
	pcard->cardid = pduel->game_field->infos.card_id++;
	pcard->sortid = pcard->cardid;
}
bool interpreter::rebind_card(card *pcard) {
	if(!pcard || !pcard->ref_handle)
		return false;
	const int top = lua_gettop(current_state);
	luaL_checkstack(current_state, 2, nullptr);
	lua_rawgeti(current_state, LUA_REGISTRYINDEX, pcard->ref_handle);
	if(!load_card_script(pcard->data.get_original_code())) {
		lua_settop(current_state, top);
		return false;
	}
	lua_setmetatable(current_state, -2);
	lua_settop(current_state, top);
	return true;
}
void interpreter::unregister_card(card *pcard) {
	if (!pcard)
		return;
	luaL_unref(lua_state, LUA_REGISTRYINDEX, pcard->ref_handle);
	pcard->ref_handle = 0;
}
void interpreter::register_effect(effect *peffect) {
	if (!peffect)
		return;
	//create a effect in userdata
	luaL_checkstack(lua_state, 3, nullptr);
	effect ** ppeffect = (effect**) lua_newuserdata(lua_state, sizeof(effect*));
	*ppeffect = peffect;
	peffect->ref_handle = luaL_ref(lua_state, LUA_REGISTRYINDEX);
	//set metatable of pointer to base script
	lua_rawgeti(lua_state, LUA_REGISTRYINDEX, peffect->ref_handle);
	lua_getglobal(lua_state, "Effect");
	lua_setmetatable(lua_state, -2);
	lua_pop(lua_state, 1);
}
void interpreter::unregister_effect(effect *peffect) {
	if (!peffect)
		return;
	if(peffect->condition)
		luaL_unref(lua_state, LUA_REGISTRYINDEX, peffect->condition);
	if(peffect->cost)
		luaL_unref(lua_state, LUA_REGISTRYINDEX, peffect->cost);
	if(peffect->target)
		luaL_unref(lua_state, LUA_REGISTRYINDEX, peffect->target);
	if(peffect->operation)
		luaL_unref(lua_state, LUA_REGISTRYINDEX, peffect->operation);
	if(peffect->value && peffect->is_flag(EFFECT_FLAG_FUNC_VALUE))
		luaL_unref(lua_state, LUA_REGISTRYINDEX, peffect->value);
	luaL_unref(lua_state, LUA_REGISTRYINDEX, peffect->ref_handle);
	peffect->ref_handle = 0;
}
void interpreter::register_group(group *pgroup) {
	if (!pgroup)
		return;
	//create a group in by userdata
	luaL_checkstack(lua_state, 3, nullptr);
	group ** ppgroup = (group**) lua_newuserdata(lua_state, sizeof(group*));
	*ppgroup = pgroup;
	pgroup->ud = ppgroup;
	pgroup->weak = false;
	pgroup->ref_handle = luaL_ref(lua_state, LUA_REGISTRYINDEX);
	//set metatable of pointer to base script
	lua_rawgeti(lua_state, LUA_REGISTRYINDEX, pgroup->ref_handle);
	lua_getglobal(lua_state, "Group");
	lua_setmetatable(lua_state, -2);
	lua_pop(lua_state, 1);
}
// `released_slots` collects a weak group's slot instead of freeing it (release_script_group frees them in slot
// order, not in the address order it walks sgroups in).
void interpreter::unregister_group(group *pgroup, std::vector<int32_t>* released_slots) {
	if (!pgroup)
		return;
	if(pgroup->ud)
		*pgroup->ud = nullptr;
	if(pgroup->weak) {
		clear_weak_slot(pgroup->ref_handle);
		if(released_slots)
			released_slots->push_back(pgroup->ref_handle);
		else
			weak_group_free.push_back(pgroup->ref_handle);
	} else
		luaL_unref(lua_state, LUA_REGISTRYINDEX, pgroup->ref_handle);
	pgroup->ref_handle = 0;
	pgroup->weak = false;
	pgroup->ud = nullptr;
}
// Hand a group a library function just created to Lua as its result, and let Lua's collector free it once no Lua
// value refers to it. The caller keeps no pointer to it, and no C++ structure holds it: a group a structure keeps
// is held first (hold_group). The group stays strong through every callback its function made while filling it,
// so it only turns weak here, on the Lua stack. Only a script temporary turns weak: a default-type group of the
// current script call (sgroups), which the outermost return would otherwise delete. The release at that return
// still deletes whatever is left, exactly as before.
void interpreter::return_temporary_group(lua_State* L, group* pgroup) {
	group2value(L, pgroup);
	interpreter* lua = pgroup->pduel->lua;
	if(pgroup->weak || pgroup->is_readonly != GTYPE_DEFAULT || !pgroup->pduel->sgroups.count(pgroup))
		return;
	int32_t slot;
	if(lua->weak_group_free.empty()) {
		slot = lua->weak_group_next++;
	} else {
		slot = lua->weak_group_free.back();
		lua->weak_group_free.pop_back();
	}
	luaL_checkstack(L, 2, nullptr);
	lua_rawgeti(L, LUA_REGISTRYINDEX, lua->weak_groups);
	lua_pushvalue(L, -2);
	lua_rawseti(L, -2, slot);
	lua_pop(L, 1);
	luaL_unref(L, LUA_REGISTRYINDEX, pgroup->ref_handle);
	pgroup->ref_handle = slot;
	pgroup->weak = true;
	// Lua's collector paces itself by the Lua heap, where a group is a small userdata; its C++ object, card set and
	// bookkeeping live outside, and it cannot see them. Count that side (an estimate from counts and fixed sizes:
	// the object with its duel-set entries, one set node per card) and, every WEAK_GROUP_COLLECT_BYTES of it, run a
	// full collection and delete what it found unreachable. The point depends only on the calls, and a full
	// collection finds the same set in every process, so group lifetimes do too. This group is on the Lua stack.
	lua->weak_group_bytes += (uint32_t)(sizeof(group) + 96 + 64 * pgroup->container.size());
	if(lua->weak_group_bytes >= WEAK_GROUP_COLLECT_BYTES) {
		lua->weak_group_bytes = 0;
		lua_gc(L, LUA_GCCOLLECT, 0);
		pgroup->pduel->free_collected_groups();
#ifdef MF_GROUP_AUDIT
		++pgroup->pduel->group_full_collections;
#endif
	}
}
// A C++ structure is about to keep this group beyond the current library call: make it strong again. A held
// default-type group is still deleted by the outermost return, as before; a kept-alive one lives on.
void interpreter::hold_group(group* pgroup) {
	if(!pgroup->weak)
		return;
	luaL_checkstack(lua_state, 2, nullptr);
	lua_rawgeti(lua_state, LUA_REGISTRYINDEX, weak_groups);
	lua_rawgeti(lua_state, -1, pgroup->ref_handle);
	if(lua_isnil(lua_state, -1))
		mf_core_refusal("weak_group_invariant", "hold_group: a weak group C++ still names was already collected");
	const int32_t ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
	lua_pop(lua_state, 1);
	clear_weak_slot(pgroup->ref_handle);
	weak_group_free.push_back(pgroup->ref_handle);
	pgroup->ref_handle = ref;
	pgroup->weak = false;
}
void interpreter::clear_weak_slot(int32_t slot) {
	luaL_checkstack(lua_state, 2, nullptr);
	lua_rawgeti(lua_state, LUA_REGISTRYINDEX, weak_groups);
	lua_pushnil(lua_state);
	lua_rawseti(lua_state, -2, slot);
	lua_pop(lua_state, 1);
}
int32_t interpreter::load_script(const char* script_name) {
	int len = 0;
	byte* buffer = ::read_script(script_name, &len);
	if (!buffer)
		return OPERATION_FAIL;
	++no_action;
	luaL_checkstack(current_state, 2, nullptr);
	int32_t error = 0;
	if (enable_unsafe_feature)
		error = luaL_loadbuffer(current_state, (const char*)buffer, len, script_name) || lua_pcall(current_state, 0, 0, 0);
	else
		error = luaL_loadbufferx(current_state, (const char*)buffer, len, script_name, "t") || lua_pcall(current_state, 0, 0, 0);
	if (error) {
		interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(current_state, -1));
		handle_message(pduel, 1);
		lua_pop(current_state, 1);
		--no_action;
		return OPERATION_FAIL;
	}
	--no_action;
	return OPERATION_SUCCESS;
}
//push table cxxx onto the stack of current_state 
int32_t interpreter::load_card_script(uint32_t code) {
	char class_name[20];
	interpreter::sprintf(class_name, "c%d", code);
	luaL_checkstack(current_state, 1, nullptr);
	lua_getglobal(current_state, class_name);	//+1 table cxxx
	if (lua_isnil(current_state, -1)) {
		luaL_checkstack(current_state, 5, nullptr);
		lua_pop(current_state, 1);	//-1
		//create a table & set metatable
		lua_createtable(current_state, 0, 0);		//+1, {}
		lua_setglobal(current_state, class_name);	//-1
		lua_getglobal(current_state, class_name);	//+1 table cxxx
		lua_getglobal(current_state, "Card");		//+1 Card, table cxxx
		lua_setmetatable(current_state, -2);		//-1 table cxxx
		lua_pushstring(current_state, "__index");	//+1 "__index", table cxxx
		lua_pushvalue(current_state, -2);			//+1 table cxxx, "__index", table cxxx
		lua_rawset(current_state, -3);				//-2 table cxxx
		card_data cdata;
		int32_t res = OPERATION_SUCCESS;
		::read_card(code, &cdata);
		if (is_load_script(cdata)) {
			lua_getglobal(current_state, class_name);	//+1
			lua_setglobal(current_state, "self_table");	//-1
			lua_pushinteger(current_state, code);		//+1
			lua_setglobal(current_state, "self_code");	//-1
			char script_name[64];
			interpreter::sprintf(script_name, "./script/c%d.lua", code);
			res = load_script(script_name);
			lua_pushnil(current_state);					//+1
			lua_setglobal(current_state, "self_table"); //-1
			lua_pushnil(current_state);					//+1
			lua_setglobal(current_state, "self_code");	//-1 table cxxx {__index: cxxx }
		}
		if(!res) {
			return OPERATION_FAIL;
		}
	}
	return OPERATION_SUCCESS;
}
void interpreter::add_param(void* param, LuaParamType type, bool front) {
	lua_param p;
	p.ptr = param;
	if(front)
		params.emplace_front(p, type);
	else
		params.emplace_back(p, type);
}
void interpreter::add_param(int32_t param, LuaParamType type, bool front) {
	lua_param p;
	p.integer = param;
	if(front)
		params.emplace_front(p, type);
	else
		params.emplace_back(p, type);
}
void interpreter::push_param(lua_State* L, bool is_coroutine) {
	int32_t pushed = 0;
	for (const auto& it : params) {
		luaL_checkstack(L, 1, nullptr);
		auto type = it.second;
		switch(type) {
		case PARAM_TYPE_INT:
			lua_pushinteger(L, it.first.integer);
			break;
		case PARAM_TYPE_STRING:
			lua_pushstring(L, (const char*)it.first.ptr);
			break;
		case PARAM_TYPE_BOOLEAN:
			lua_pushboolean(L, it.first.integer);
			break;
		case PARAM_TYPE_CARD: {
			if (it.first.ptr)
				lua_rawgeti(L, LUA_REGISTRYINDEX, ((card*)it.first.ptr)->ref_handle);
			else
				lua_pushnil(L);
			break;
		}
		case PARAM_TYPE_EFFECT: {
			if (it.first.ptr)
				lua_rawgeti(L, LUA_REGISTRYINDEX, ((effect*)it.first.ptr)->ref_handle);
			else
				lua_pushnil(L);
			break;
		}
		case PARAM_TYPE_GROUP: {
			group2value(L, (group*)it.first.ptr);
			break;
		}
		case PARAM_TYPE_FUNCTION: {
			function2value(L, it.first.integer);
			break;
		}
		case PARAM_TYPE_INDEX: {
			int32_t index = it.first.integer;
			if(index > 0)
				lua_pushvalue(L, index);
			else if(is_coroutine) {
				//copy value from current_state to new stack
				lua_pushvalue(current_state, index);
				lua_xmove(current_state, L, 1);
			} else {
				//the calling function is pushed before the params, so the actual index is: index - pushed -1
				lua_pushvalue(L, index - pushed - 1);
			}
			break;
		}
		}
		++pushed;
	}
	params.clear();
}
int32_t interpreter::call_function(int32_t f, uint32_t param_count, int32_t ret_count) {
	if (!f) {
		interpreter::sprintf(pduel->strbuffer, "%s", "\"CallFunction\": attempt to call a null function.");
		handle_message(pduel, 1);
		params.clear();
		return OPERATION_FAIL;
	}
	if (param_count != params.size()) {
		interpreter::sprintf(pduel->strbuffer, "\"CallFunction\": incorrect parameter count (%d expected, %zu pushed)", param_count, params.size());
		handle_message(pduel, 1);
		params.clear();
		return OPERATION_FAIL;
	}
	function2value(current_state, f);
	if (!lua_isfunction(current_state, -1)) {
		interpreter::sprintf(pduel->strbuffer, "%s", "\"CallFunction\": attempt to call an error function");
		handle_message(pduel, 1);
		lua_pop(current_state, 1);
		params.clear();
		return OPERATION_FAIL;
	}
	++no_action;
	++call_depth;
	push_param(current_state);
	if (lua_pcall(current_state, param_count, ret_count, 0)) {
		interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(current_state, -1));
		handle_message(pduel, 1);
		lua_pop(current_state, 1);
		--no_action;
		--call_depth;
		if(call_depth == 0) {
			pduel->release_script_group();
			pduel->restore_assumes();
		}
		return OPERATION_FAIL;
	}
	--no_action;
	--call_depth;
	if(call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return OPERATION_SUCCESS;
}
int32_t interpreter::call_card_function(card* pcard, const char* f, uint32_t param_count, int32_t ret_count) {
	if (param_count != params.size()) {
		interpreter::sprintf(pduel->strbuffer, "\"CallCardFunction\"(c%d.%s): incorrect parameter count", pcard->data.get_original_code(), f);
		handle_message(pduel, 1);
		params.clear();
		return OPERATION_FAIL;
	}
	card2value(current_state, pcard);
	luaL_checkstack(current_state, 1, nullptr);
	lua_getfield(current_state, -1, f);
	if (!lua_isfunction(current_state, -1)) {
		interpreter::sprintf(pduel->strbuffer, "\"CallCardFunction\"(c%d.%s): attempt to call an error function", pcard->data.get_original_code(), f);
		handle_message(pduel, 1);
		lua_pop(current_state, 2);
		params.clear();
		return OPERATION_FAIL;
	}
	++no_action;
	++call_depth;
	lua_remove(current_state, -2);
	push_param(current_state);
	if (lua_pcall(current_state, param_count, ret_count, 0)) {
		interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(current_state, -1));
		handle_message(pduel, 1);
		lua_pop(current_state, 1);
		--no_action;
		--call_depth;
		if(call_depth == 0) {
			pduel->release_script_group();
			pduel->restore_assumes();
		}
		return OPERATION_FAIL;
	}
	--no_action;
	--call_depth;
	if(call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return OPERATION_SUCCESS;
}
int32_t interpreter::call_code_function(uint32_t code, const char* f, uint32_t param_count, int32_t ret_count) {
	if (param_count != params.size()) {
		interpreter::sprintf(pduel->strbuffer, "%s", "\"CallCodeFunction\": incorrect parameter count");
		handle_message(pduel, 1);
		params.clear();
		return OPERATION_FAIL;
	}
	load_card_script(code);
	luaL_checkstack(current_state, 1, nullptr);
	lua_getfield(current_state, -1, f);
	if (!lua_isfunction(current_state, -1)) {
		interpreter::sprintf(pduel->strbuffer, "%s", "\"CallCodeFunction\": attempt to call an error function");
		handle_message(pduel, 1);
		lua_pop(current_state, 2);
		params.clear();
		return OPERATION_FAIL;
	}
	lua_remove(current_state, -2);
	++no_action;
	++call_depth;
	push_param(current_state);
	if (lua_pcall(current_state, param_count, ret_count, 0)) {
		interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(current_state, -1));
		handle_message(pduel, 1);
		lua_pop(current_state, 1);
		--no_action;
		--call_depth;
		if(call_depth == 0) {
			pduel->release_script_group();
			pduel->restore_assumes();
		}
		return OPERATION_FAIL;
	}
	--no_action;
	--call_depth;
	if(call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return OPERATION_SUCCESS;
}
int32_t interpreter::check_condition(int32_t f, uint32_t param_count) {
	if(!f) {
		params.clear();
		return TRUE;
	}
	++no_action;
	++call_depth;
	if (call_function(f, param_count, 1)) {
		int32_t result = lua_toboolean(current_state, -1);
		lua_pop(current_state, 1);
		--no_action;
		--call_depth;
		if(call_depth == 0) {
			pduel->release_script_group();
			pduel->restore_assumes();
		}
		return result;
	}
	--no_action;
	--call_depth;
	if(call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return OPERATION_FAIL;
}
int32_t interpreter::check_filter(lua_State* L, card* pcard, int32_t findex, int32_t extraargs) {
	if (!findex || lua_isnil(L, findex))
		return TRUE;
	if (pduel->blank_passes_filter(pcard))
		return TRUE;
	++no_action;
	++call_depth;
	luaL_checkstack(L, 1 + extraargs, nullptr);
	lua_pushvalue(L, findex);
	card2value(L, pcard);
	for (int32_t i = 0; i < extraargs; ++i)
		lua_pushvalue(L, (int32_t)(-extraargs - 2));
	if (lua_pcall(L, 1 + extraargs, 1, 0)) {
		interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(L, -1));
		handle_message(pduel, 1);
		lua_pop(L, 1);
		--no_action;
		--call_depth;
		if (call_depth == 0) {
			pduel->release_script_group();
			pduel->restore_assumes();
		}
		return OPERATION_FAIL;
	}
	int32_t result = lua_toboolean(L, -1);
	lua_pop(L, 1);
	--no_action;
	--call_depth;
	if (call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return result;
}
int32_t interpreter::get_operation_value(card* pcard, int32_t findex, int32_t extraargs) {
	if(!findex || lua_isnil(current_state, findex))
		return 0;
	++no_action;
	++call_depth;
	luaL_checkstack(current_state, 1 + extraargs, nullptr);
	lua_pushvalue(current_state, findex);
	interpreter::card2value(current_state, pcard);
	for(int32_t i = 0; i < extraargs; ++i)
		lua_pushvalue(current_state, (int32_t)(-extraargs - 2));
	if (lua_pcall(current_state, 1 + extraargs, 1, 0)) {
		interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(current_state, -1));
		handle_message(pduel, 1);
		lua_pop(current_state, 1);
		--no_action;
		--call_depth;
		if(call_depth == 0) {
			pduel->release_script_group();
			pduel->restore_assumes();
		}
		return OPERATION_FAIL;
	}
	int32_t result = lua_isinteger(current_state, -1) ? (int32_t)lua_tointeger(current_state, -1) : (int32_t)lua_tonumber(current_state, -1);
	lua_pop(current_state, 1);
	--no_action;
	--call_depth;
	if(call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return result;
}
int32_t interpreter::get_function_value(int32_t f, uint32_t param_count) {
	if(!f) {
		params.clear();
		return 0;
	}
	++no_action;
	++call_depth;
	if (call_function(f, param_count, 1)) {
		int32_t result = 0;
		if(lua_isboolean(current_state, -1))
			result = lua_toboolean(current_state, -1);
		else if(lua_isinteger(current_state, -1))
			result = (int32_t)lua_tointeger(current_state, -1);
		else
			result = (int32_t)lua_tonumber(current_state, -1);
		lua_pop(current_state, 1);
		--no_action;
		--call_depth;
		if(call_depth == 0) {
			pduel->release_script_group();
			pduel->restore_assumes();
		}
		return result;
	}
	--no_action;
	--call_depth;
	if(call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return OPERATION_FAIL;
}
int32_t interpreter::get_function_value(int32_t f, uint32_t param_count, std::vector<lua_Integer>& result) {
	int32_t is_success = OPERATION_FAIL;
	if(!f) {
		params.clear();
		return is_success;
	}
	int32_t stack_top = lua_gettop(current_state);
	++no_action;
	++call_depth;
	if (call_function(f, param_count, LUA_MULTRET)) {
		int32_t stack_newtop = lua_gettop(current_state);
		for (int32_t index = stack_top + 1; index <= stack_newtop; ++index) {
			lua_Integer return_value = 0;
			if(lua_isboolean(current_state, index))
				return_value = lua_toboolean(current_state, index);
			else if(lua_isinteger(current_state, index))
				return_value = lua_tointeger(current_state, index);
			else
				return_value = static_cast<lua_Integer>(lua_tonumber(current_state, index));
			result.push_back(return_value);
		}
		lua_settop(current_state, stack_top);
		is_success = OPERATION_SUCCESS;
	}
	--no_action;
	--call_depth;
	if(call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return is_success;
}
int32_t interpreter::call_coroutine(int32_t f, uint32_t param_count, int32_t* yield_value, uint16_t step) {
	*yield_value = 0;
	if (!f) {
		interpreter::sprintf(pduel->strbuffer, "%s", "\"CallCoroutine\": attempt to call a null function");
		handle_message(pduel, 1);
		params.clear();
		return OPERATION_FAIL;
	}
	if (param_count != params.size()) {
		interpreter::sprintf(pduel->strbuffer, "%s", "\"CallCoroutine\": incorrect parameter count");
		handle_message(pduel, 1);
		params.clear();
		return OPERATION_FAIL;
	}
	auto it = coroutines.find(f);
	lua_State* rthread;
	if (it == coroutines.end()) {
		rthread = lua_newthread(lua_state);
		const auto threadref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
		function2value(rthread, f);
		if(!lua_isfunction(rthread, -1)) {
			luaL_unref(lua_state, LUA_REGISTRYINDEX, threadref);
			interpreter::sprintf(pduel->strbuffer, "%s", "\"CallCoroutine\": attempt to call an error function");
			handle_message(pduel, 1);
			params.clear();
			return OPERATION_FAIL;
		}
		++call_depth;
		auto ret = coroutines.emplace(f, coroutine_map::mapped_type(rthread, threadref));
		it = ret.first;
	} else {
		if(step == 0) {
			auto threadref = it->second.second;
			coroutines.erase(it);
			luaL_unref(lua_state, LUA_REGISTRYINDEX, threadref);
			interpreter::sprintf(pduel->strbuffer, "%s", "recursive event trigger detected");
			handle_message(pduel, 1);
			params.clear();
			--call_depth;
			if(call_depth == 0) {
				pduel->release_script_group();
				pduel->restore_assumes();
			}
			return OPERATION_FAIL;
		}
		rthread = it->second.first;
	}
	push_param(rthread, true);
	int32_t result = 0, nresults = 0;
	{
		auto prev_state = current_state;
		current_state = rthread;
#if (LUA_VERSION_NUM >= 504)
		result = lua_resume(rthread, prev_state, param_count, &nresults);
#else
		result = lua_resume(rthread, prev_state, param_count);
		nresults = lua_gettop(rthread);
#endif
		current_state = prev_state;
	}
	if (result == LUA_YIELD)
		return COROUTINE_YIELD;
	if (result != LUA_OK) {
		interpreter::sprintf(pduel->strbuffer, "%s", lua_tostring(rthread, -1));
		handle_message(pduel, 1);
		lua_pop(rthread, 1);
	}
	else if (yield_value) {
		if (nresults == 0)
			*yield_value = 0;
		else if (lua_isboolean(rthread, -1))
			*yield_value = lua_toboolean(rthread, -1);
		else
			*yield_value = (int32_t)lua_tointeger(rthread, -1);
	}
	auto threadref = it->second.second;
	coroutines.erase(it);
	luaL_unref(lua_state, LUA_REGISTRYINDEX, threadref);
	--call_depth;
	if (call_depth == 0) {
		pduel->release_script_group();
		pduel->restore_assumes();
	}
	return (result == LUA_OK) ? COROUTINE_FINISH : COROUTINE_ERROR;
}
int32_t interpreter::clone_function_ref(int32_t func_ref) {
	luaL_checkstack(current_state, 1, nullptr);
	lua_rawgeti(current_state, LUA_REGISTRYINDEX, func_ref);
	int32_t ref = luaL_ref(current_state, LUA_REGISTRYINDEX);
	return ref;
}
void* interpreter::get_ref_object(int32_t ref_handler) {
	if(ref_handler == 0)
		return nullptr;
	luaL_checkstack(current_state, 1, nullptr);
	lua_rawgeti(current_state, LUA_REGISTRYINDEX, ref_handler);
	void* p = *(void**)lua_touserdata(current_state, -1);
	lua_pop(current_state, 1);
	return p;
}
//push the object onto the stack of L, +1
void interpreter::card2value(lua_State* L, card* pcard) {
	luaL_checkstack(L, 1, nullptr);
	if (!pcard || pcard->ref_handle == 0)
		lua_pushnil(L);
	else
		lua_rawgeti(L, LUA_REGISTRYINDEX, pcard->ref_handle);
}
void interpreter::group2value(lua_State* L, group* pgroup) {
	luaL_checkstack(L, 2, nullptr);
	if (!pgroup || pgroup->ref_handle == 0)
		lua_pushnil(L);
	else if (!pgroup->weak)
		lua_rawgeti(L, LUA_REGISTRYINDEX, pgroup->ref_handle);
	else {
		lua_rawgeti(L, LUA_REGISTRYINDEX, pgroup->pduel->lua->weak_groups);
		lua_rawgeti(L, -1, pgroup->ref_handle);
		lua_remove(L, -2);
		// only the collector clears a weak group's slot, and only once no Lua value refers to it: C++ naming such a
		// group held it nowhere, which hold_group exists to prevent
		if(lua_isnil(L, -1))
			mf_core_refusal("weak_group_invariant", "group2value: a weak group C++ still names was already collected");
	}
}
void interpreter::effect2value(lua_State* L, effect* peffect) {
	luaL_checkstack(L, 1, nullptr);
	if (!peffect || peffect->ref_handle == 0)
		lua_pushnil(L);
	else
		lua_rawgeti(L, LUA_REGISTRYINDEX, peffect->ref_handle);
}
void interpreter::function2value(lua_State* L, int32_t func_ref) {
	luaL_checkstack(L, 1, nullptr);
	if (!func_ref)
		lua_pushnil(L);
	else
		lua_rawgeti(L, LUA_REGISTRYINDEX, func_ref);
}
int32_t interpreter::get_function_handle(lua_State* L, int32_t index) {
	luaL_checkstack(L, 1, nullptr);
	lua_pushvalue(L, index);
	int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return ref;
}
duel* interpreter::get_duel_info(lua_State* L) {
	duel* pduel;
	std::memcpy(&pduel, lua_getextraspace(L), LUA_EXTRASPACE);
	return pduel;
}
bool interpreter::is_load_script(const card_data& data) {
	// TEMP_CARD_ID and the reserved client placeholders (999000001-999000004) have no script.
	if(data.code == TEMP_CARD_ID || (data.code >= 999000001u && data.code <= 999000004u))
		return false;
	return !(data.type & TYPE_NORMAL) || (data.type & TYPE_PENDULUM);
}
