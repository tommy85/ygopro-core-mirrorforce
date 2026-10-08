/*
 * scriptlib.cpp
 *
 *  Created on: 2010-7-29
 *      Author: Argon
 */
#include <string>
#include "scriptlib.h"
#include "duel.h"
#include "card.h"
#include "group.h"
#include "effect.h"

static int32_t check_data_type(lua_State* L, int32_t index, const char* tname) {
	luaL_checkstack(L, 2, nullptr);
	int32_t result = FALSE;
	if(lua_getmetatable(L, index)) {
		lua_getglobal(L, tname);
		if(lua_rawequal(L, -1, -2))
			result = TRUE;
		lua_pop(L, 2);
	}
	return result;
}
int32_t scriptlib::check_param(lua_State* L, int32_t param_type, int32_t index, int32_t retfalse) {
	switch (param_type) {
	case PARAM_TYPE_CARD: {
		luaL_checkstack(L, 1, nullptr);
		int32_t result = FALSE;
		if(lua_isuserdata(L, index) && lua_getmetatable(L, index)) {
			result = check_data_type(L, -1, "Card");
			lua_pop(L, 1);
		}
		if(result) {
			duel* pd = interpreter::get_duel_info(L);
			if(pd->tactical_audit.enabled) {
				card* value = *(card**)lua_touserdata(L, index);
				if(value && pd->cards.count(value))
					pd->tactical_audit.card(value->cardid, mf_tactical_audit::LUA_CARD);
				else
					pd->tactical_audit.hit(mf_tactical_audit::LUA_CARD);
			}
			return TRUE;
		}
		if(retfalse)
			return FALSE;
		return luaL_error(L, "Parameter %d should be \"Card\".", index);
		break;
	}
	case PARAM_TYPE_GROUP: {
		if(lua_isuserdata(L, index) && check_data_type(L, index, "Group")) {
			// deleting a group clears its userdata's pointer (the outermost return releases the call's default
			// groups): a script still holding one ends the duel here, instead of reading a freed group
			if(!*(group**)lua_touserdata(L, index)) {
				luaL_where(L, 1);
				const std::string detail = std::string(lua_tostring(L, -1)) + "parameter " + std::to_string(index)
				    + " is a group the core already released";
				lua_pop(L, 1);
				mf_core_refusal("released_group", detail.c_str());
			}
			group* value = *(group**)lua_touserdata(L, index);
			duel* pd = interpreter::get_duel_info(L);
			if(pd->tactical_audit.enabled)
				for(card* member : value->container) {
					if(member && pd->cards.count(member))
						pd->tactical_audit.card(member->cardid, mf_tactical_audit::LUA_GROUP);
					else
						pd->tactical_audit.hit(mf_tactical_audit::LUA_GROUP);
				}
			return TRUE;
		}
		if(retfalse)
			return FALSE;
		return luaL_error(L, "Parameter %d should be \"Group\".", index);
		break;
	}
	case PARAM_TYPE_EFFECT: {
		if(lua_isuserdata(L, index) && check_data_type(L, index, "Effect")) {
			duel* pd = interpreter::get_duel_info(L);
			if(pd->tactical_audit.enabled) {
				effect* value = *(effect**)lua_touserdata(L, index);
				if(value) {
					for(card* source : {value->handler, value->owner}) {
						if(!source)
							continue;
						if(pd->cards.count(source))
							pd->tactical_audit.card(source->cardid, mf_tactical_audit::LUA_EFFECT);
						else
							pd->tactical_audit.hit(mf_tactical_audit::LUA_EFFECT);
					}
				}
			}
			return TRUE;
		}
		if(retfalse)
			return FALSE;
		return luaL_error(L, "Parameter %d should be \"Effect\".", index);
		break;
	}
	case PARAM_TYPE_FUNCTION: {
		if(lua_isfunction(L, index))
			return TRUE;
		if(retfalse)
			return FALSE;
		return luaL_error(L, "Parameter %d should be \"Function\".", index);
		break;
	}
	case PARAM_TYPE_STRING: {
		if(lua_isstring(L, index))
			return TRUE;
		if(retfalse)
			return FALSE;
		return luaL_error(L, "Parameter %d should be \"String\".", index);
		break;
	}
	}
	return FALSE;
}

int32_t scriptlib::check_param_count(lua_State* L, int32_t count) {
	if (lua_gettop(L) < count)
		return luaL_error(L, "%d Parameters are needed.", count);
	return TRUE;
}
int32_t scriptlib::check_action_permission(lua_State* L) {
	duel* pduel = interpreter::get_duel_info(L);
	if(pduel->lua->no_action)
		return luaL_error(L, "Action is not allowed here.");
	return TRUE;
}
