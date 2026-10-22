#include "stdafx.h"
#include <dinput.h>
#include "../xrEngine/xr_ioconsole.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "xr_level_controller.h"
#include "string_table.h"
#ifdef XR_PLATFORM_APPLE
#include <GameInput/Adapter.h>
#include <GameInput/Bindings.h>
#include <GameInput/GameInput.h>
#include <sstream>
#endif

_binding g_key_bindings [bindings_count];
_key_group g_current_keygroup = _sp;

_action actions[] = {
	{"left", kLEFT, _both},
	{"right", kRIGHT, _both},
	{"up", kUP, _both},
	{"down", kDOWN, _both},
	{"jump", kJUMP, _both},
	{"crouch", kCROUCH, _both},
	{"accel", kACCEL, _both},
	{"sprint_toggle", kSPRINT_TOGGLE, _both},

	{"forward", kFWD, _both},
	{"back", kBACK, _both},
	{"lstrafe", kL_STRAFE, _both},
	{"rstrafe", kR_STRAFE, _both},

	{"llookout", kL_LOOKOUT, _both},
	{"rlookout", kR_LOOKOUT, _both},

	{"cam_1", kCAM_1, _both},
	{"cam_2", kCAM_2, _both},
	{"cam_3", kCAM_3, _both},
	{"cam_zoom_in", kCAM_ZOOM_IN, _both},
	{"cam_zoom_out", kCAM_ZOOM_OUT, _both},

	{"torch", kTORCH, _both},
	{"night_vision", kNIGHT_VISION, _both},
	{"show_detector", kDETECTOR, _sp},

	{"wpn_1", kWPN_1, _both},
	{"wpn_2", kWPN_2, _both},
	{"wpn_3", kWPN_3, _both},
	{"wpn_4", kWPN_4, _both},
	{"wpn_5", kWPN_5, _both},
	{"wpn_6", kWPN_6, _both},
	{"artefact", kARTEFACT, _both/*_mp*/},
	{"wpn_next", kWPN_NEXT, _both}, // means next ammo type
	{"wpn_fire", kWPN_FIRE, _both},
	{"wpn_zoom", kWPN_ZOOM, _both},
	{"wpn_zoom_inc", kWPN_ZOOM_INC, _both},
	{"wpn_zoom_dec", kWPN_ZOOM_DEC, _both},
	{"wpn_reload", kWPN_RELOAD, _both},
	{"wpn_func", kWPN_FUNC, _both},
	{"wpn_firemode_prev", kWPN_FIREMODE_PREV, _both},
	{"wpn_firemode_next", kWPN_FIREMODE_NEXT, _both},

	{"pause", kPAUSE, _both},
	{"drop", kDROP, _both},
	{"use", kUSE, _both},
	{"scores", kSCORES, _both},
	{"chat", kCHAT, _both},
	{"chat_team", kCHAT_TEAM, _both},
	{"screenshot", kSCREENSHOT, _both},
	{"quit", kQUIT, _both},
	{"console", kCONSOLE, _both},
	{"inventory", kINVENTORY, _both},
	{"buy_menu", kBUY, _both},
	{"skin_menu", kSKIN, _both},
	{"team_menu", kTEAM, _both},
	{"active_jobs", kACTIVE_JOBS, _sp},

	{"vote_begin", kVOTE_BEGIN, _both},
	{"show_admin_menu", kSHOW_ADMIN_MENU, _both},
	{"vote", kVOTE, _both},
	{"vote_yes", kVOTEYES, _both},
	{"vote_no", kVOTENO, _both},

	{"next_slot", kNEXT_SLOT, _both},
	{"prev_slot", kPREV_SLOT, _both},

	{"speech_menu_0", kSPEECH_MENU_0, _both},
	{"speech_menu_1", kSPEECH_MENU_1, _both},

	{"quick_use_1", kQUICK_USE_1, _both},
	{"quick_use_2", kQUICK_USE_2, _both},
	{"quick_use_3", kQUICK_USE_3, _both},
	{"quick_use_4", kQUICK_USE_4, _both},

	{"quick_save", kQUICK_SAVE, _sp},
	{"quick_load", kQUICK_LOAD, _sp},
	//	{ "alife_command",		kALIFE_CMD				,_sp},		
	{"custom1", kCUSTOM1, _sp},
	{"custom2", kCUSTOM2, _sp},
	{"custom3", kCUSTOM3, _sp},
	{"custom4", kCUSTOM4, _sp},
	{"custom5", kCUSTOM5, _sp},
	{"custom6", kCUSTOM6, _sp},
	{"custom7", kCUSTOM7, _sp},
	{"custom8", kCUSTOM8, _sp},
	{"custom9", kCUSTOM9, _sp},
	{"custom10", kCUSTOM10, _sp},
	{"custom11", kCUSTOM11, _sp},
	{"custom12", kCUSTOM12, _sp},
	{"custom13", kCUSTOM13, _sp},
	{"custom14", kCUSTOM14, _sp},
	{"custom15", kCUSTOM15, _sp},
	{"custom16", kCUSTOM16, _sp },
	{"custom17", kCUSTOM17, _sp },
	{"custom18", kCUSTOM18, _sp },
	{"custom19", kCUSTOM19, _sp },
	{"custom20", kCUSTOM20, _sp },
	{"custom21", kCUSTOM21, _sp },
	{"custom22", kCUSTOM22, _sp },
	{"custom23", kCUSTOM23, _sp },
	{"custom24", kCUSTOM24, _sp },
	{"custom25", kCUSTOM25, _sp },
	{"safemode", kSAFEMODE, _both},
	{"freelook", kFREELOOK, _both},
	{"cam_autoaim", kCAM_AUTOAIM, _sp},
	{"editor", kEDITOR, _both},
	{NULL, kLASTACTION, _both}
};

_keyboard keyboards[] = {
	{"kESCAPE", DIK_ESCAPE}, {"k1", DIK_1},
	{"k2", DIK_2}, {"k3", DIK_3},
	{"k4", DIK_4}, {"k5", DIK_5},
	{"k6", DIK_6}, {"k7", DIK_7},
	{"k8", DIK_8}, {"k9", DIK_9},
	{"k0", DIK_0}, {"kMINUS", DIK_MINUS},
	{"kEQUALS", DIK_EQUALS}, {"kBACK", DIK_BACK},
	{"kTAB", DIK_TAB}, {"kQ", DIK_Q},
	{"kW", DIK_W}, {"kE", DIK_E},
	{"kR", DIK_R}, {"kT", DIK_T},
	{"kY", DIK_Y}, {"kU", DIK_U},
	{"kI", DIK_I}, {"kO", DIK_O},
	{"kP", DIK_P}, {"kLBRACKET", DIK_LBRACKET},
	{"kRBRACKET", DIK_RBRACKET}, {"kRETURN", DIK_RETURN},
	{"kLCONTROL", DIK_LCONTROL}, {"kA", DIK_A},
	{"kS", DIK_S}, {"kD", DIK_D},
	{"kF", DIK_F}, {"kG", DIK_G},
	{"kH", DIK_H}, {"kJ", DIK_J},
	{"kK", DIK_K}, {"kL", DIK_L},
	{"kSEMICOLON", DIK_SEMICOLON}, {"kAPOSTROPHE", DIK_APOSTROPHE},
	{"kGRAVE", DIK_GRAVE}, {"kLSHIFT", DIK_LSHIFT},
	{"kBACKSLASH", DIK_BACKSLASH}, {"kZ", DIK_Z},
	{"kX", DIK_X}, {"kC", DIK_C},
	{"kV", DIK_V}, {"kB", DIK_B},
	{"kN", DIK_N}, {"kM", DIK_M},
	{"kCOMMA", DIK_COMMA}, {"kPERIOD", DIK_PERIOD},
	{"kSLASH", DIK_SLASH}, {"kRSHIFT", DIK_RSHIFT},
	{"kMULTIPLY", DIK_MULTIPLY}, {"kLMENU", DIK_LMENU},
	{"kSPACE", DIK_SPACE}, {"kCAPITAL", DIK_CAPITAL},
	{"kF1", DIK_F1}, {"kF2", DIK_F2},
	{"kF3", DIK_F3}, {"kF4", DIK_F4},
	{"kF5", DIK_F5}, {"kF6", DIK_F6},
	{"kF7", DIK_F7}, {"kF8", DIK_F8},
	{"kF9", DIK_F9}, {"kF10", DIK_F10},
	{"kNUMLOCK", DIK_NUMLOCK}, {"kSCROLL", DIK_SCROLL},
	{"kNUMPAD7", DIK_NUMPAD7}, {"kNUMPAD8", DIK_NUMPAD8},
	{"kNUMPAD9", DIK_NUMPAD9}, {"kSUBTRACT", DIK_SUBTRACT},
	{"kNUMPAD4", DIK_NUMPAD4}, {"kNUMPAD5", DIK_NUMPAD5},
	{"kNUMPAD6", DIK_NUMPAD6}, {"kADD", DIK_ADD},
	{"kNUMPAD1", DIK_NUMPAD1}, {"kNUMPAD2", DIK_NUMPAD2},
	{"kNUMPAD3", DIK_NUMPAD3}, {"kNUMPAD0", DIK_NUMPAD0},
	{"kDECIMAL", DIK_DECIMAL}, {"kF11", DIK_F11},
	{"kF12", DIK_F12}, {"kF13", DIK_F13},
	{"kF14", DIK_F14}, {"kF15", DIK_F15},
	{"kKANA", DIK_KANA}, {"kCONVERT", DIK_CONVERT},
	{"kNOCONVERT", DIK_NOCONVERT}, {"kYEN", DIK_YEN},
	{"kNUMPADEQUALS", DIK_NUMPADEQUALS}, {"kCIRCUMFLEX", DIK_CIRCUMFLEX},
	{"kAT", DIK_AT}, {"kCOLON", DIK_COLON},
	{"kUNDERLINE", DIK_UNDERLINE}, {"kKANJI", DIK_KANJI},
	{"kSTOP", DIK_STOP}, {"kAX", DIK_AX},
	{"kUNLABELED", DIK_UNLABELED}, {"kNUMPADENTER", DIK_NUMPADENTER},
	{"kRCONTROL", DIK_RCONTROL}, {"kNUMPADCOMMA", DIK_NUMPADCOMMA},
	{"kDIVIDE", DIK_DIVIDE}, {"kSYSRQ", DIK_SYSRQ},
	{"kRMENU", DIK_RMENU}, {"kHOME", DIK_HOME},
	{"kUP", DIK_UP}, {"kPRIOR", DIK_PRIOR},
	{"kLEFT", DIK_LEFT}, {"kRIGHT", DIK_RIGHT},
	{"kEND", DIK_END}, {"kDOWN", DIK_DOWN},
	{"kNEXT", DIK_NEXT}, {"kINSERT", DIK_INSERT},
	{"kDELETE", DIK_DELETE}, {"kLWIN", DIK_LWIN},
	{"kRWIN", DIK_RWIN}, {"kAPPS", DIK_APPS},
	{"kPAUSE", DIK_PAUSE}, {"mouse1", MOUSE_1},
	{"mouse2", MOUSE_2}, {"mouse3", MOUSE_3},
	{"mouse4", MOUSE_4}, {"mouse5", MOUSE_5},
	{"mouse6", MOUSE_6}, {"mouse7", MOUSE_7},
	{"mouse8", MOUSE_8}, 

	// RavenAscendant: additional 100 entries, will let the bind console command bind actions to values that won't get triggered by keyboard input
	// but can be triggered from the level.press_action functions
	{"kCUSTOM0", 1000},
	{"kCUSTOM1", 1001},
	{"kCUSTOM2", 1002},
	{"kCUSTOM3", 1003},
	{"kCUSTOM4", 1004},
	{"kCUSTOM5", 1005},
	{"kCUSTOM6", 1006},
	{"kCUSTOM7", 1007},
	{"kCUSTOM8", 1008},
	{"kCUSTOM9", 1009},
	{"kCUSTOM10", 1010},
	{"kCUSTOM11", 1011},
	{"kCUSTOM12", 1012},
	{"kCUSTOM13", 1013},
	{"kCUSTOM14", 1014},
	{"kCUSTOM15", 1015},
	{"kCUSTOM16", 1016},
	{"kCUSTOM17", 1017},
	{"kCUSTOM18", 1018},
	{"kCUSTOM19", 1019},
	{"kCUSTOM20", 1020},
	{"kCUSTOM21", 1021},
	{"kCUSTOM22", 1022},
	{"kCUSTOM23", 1023},
	{"kCUSTOM24", 1024},
	{"kCUSTOM25", 1025},
	{"kCUSTOM26", 1026},
	{"kCUSTOM27", 1027},
	{"kCUSTOM28", 1028},
	{"kCUSTOM29", 1029},
	{"kCUSTOM30", 1030},
	{"kCUSTOM31", 1031},
	{"kCUSTOM32", 1032},
	{"kCUSTOM33", 1033},
	{"kCUSTOM34", 1034},
	{"kCUSTOM35", 1035},
	{"kCUSTOM36", 1036},
	{"kCUSTOM37", 1037},
	{"kCUSTOM38", 1038},
	{"kCUSTOM39", 1039},
	{"kCUSTOM40", 1040},
	{"kCUSTOM41", 1041},
	{"kCUSTOM42", 1042},
	{"kCUSTOM43", 1043},
	{"kCUSTOM44", 1044},
	{"kCUSTOM45", 1045},
	{"kCUSTOM46", 1046},
	{"kCUSTOM47", 1047},
	{"kCUSTOM48", 1048},
	{"kCUSTOM49", 1049},
	{"kCUSTOM50", 1050},
	{"kCUSTOM51", 1051},
	{"kCUSTOM52", 1052},
	{"kCUSTOM53", 1053},
	{"kCUSTOM54", 1054},
	{"kCUSTOM55", 1055},
	{"kCUSTOM56", 1056},
	{"kCUSTOM57", 1057},
	{"kCUSTOM58", 1058},
	{"kCUSTOM59", 1059},
	{"kCUSTOM60", 1060},
	{"kCUSTOM61", 1061},
	{"kCUSTOM62", 1062},
	{"kCUSTOM63", 1063},
	{"kCUSTOM64", 1064},
	{"kCUSTOM65", 1065},
	{"kCUSTOM66", 1066},
	{"kCUSTOM67", 1067},
	{"kCUSTOM68", 1068},
	{"kCUSTOM69", 1069},
	{"kCUSTOM70", 1070},
	{"kCUSTOM71", 1071},
	{"kCUSTOM72", 1072},
	{"kCUSTOM73", 1073},
	{"kCUSTOM74", 1074},
	{"kCUSTOM75", 1075},
	{"kCUSTOM76", 1076},
	{"kCUSTOM77", 1077},
	{"kCUSTOM78", 1078},
	{"kCUSTOM79", 1079},
	{"kCUSTOM80", 1080},
	{"kCUSTOM81", 1081},
	{"kCUSTOM82", 1082},
	{"kCUSTOM83", 1083},
	{"kCUSTOM84", 1084},
	{"kCUSTOM85", 1085},
	{"kCUSTOM86", 1086},
	{"kCUSTOM87", 1087},
	{"kCUSTOM88", 1088},
	{"kCUSTOM89", 1089},
	{"kCUSTOM90", 1090},
	{"kCUSTOM91", 1091},
	{"kCUSTOM92", 1092},
	{"kCUSTOM93", 1093},
	{"kCUSTOM94", 1094},
	{"kCUSTOM95", 1095},
	{"kCUSTOM96", 1096},
	{"kCUSTOM97", 1097},
	{"kCUSTOM98", 1098},
	{"kCUSTOM99", 1099},
	{NULL, 0}
#ifdef XR_PLATFORM_APPLE
	// room for the gamepad keys (gp_cross, ...), which initialize_bindings() puts in front of the terminator
	,{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
	{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
	{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
	{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
	{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
	{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
	{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
	{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}
#endif
};

void initialize_bindings()
{
#ifdef DEBUG
	int i1=0;
	while(true)
	{
		_keyboard& _k1			= keyboards[i1];
		if(_k1.key_name==NULL)	break;
		int i2 = i1;
		while (true)
		{
			_keyboard& _k2			= keyboards[i2];
			if(_k2.key_name==NULL)	break;
			if(_k1.dik==_k2.dik && i1!=i2)
			{
				Msg("%s==%s",_k1.key_name,_k2.key_name);
			}
			++i2;
		}
		++i1;
	}
#endif

	for (int idx = 0; idx < bindings_count; ++idx)
		g_key_bindings[idx].m_action = &actions[idx];

#ifdef XR_PLATFORM_APPLE
	// The gamepad keys (gp_cross ... gp_radial_32), so bind_pad, user.ltx and the key name lookups know them
	static std::vector<gameinput::EngineKeyDef> pad_keys = gameinput::EnginePadKeyTable(gameinput::ControllerFamily::PlayStation);
	int end = 0;
	while (keyboards[end].key_name)
		++end;
	for (const gameinput::EngineKeyDef& def : pad_keys)
	{
		keyboards[end].key_name = def.name.c_str();
		keyboards[end].dik = def.code;
		keyboards[end].key_local_name = def.label.c_str();
		++end;
	}
	keyboards[end].key_name = NULL;
#endif
}

void remap_keys()
{
	int idx = 0;
	string128 buff;
	while (keyboards[idx].key_name)
	{
		buff[0] = 0;
		_keyboard& kb = keyboards[idx];
#ifdef XR_PLATFORM_APPLE
		if (gameinput::IsPadKey(kb.dik))
		{
			kb.key_local_name = gameinput::PadKeyLabel(kb.dik, gameinput::ControllerFamily::PlayStation);
			++idx;
			continue;
		}
#endif
		bool res = pInput->get_dik_name(kb.dik, buff, sizeof(buff));
		if (res)
			kb.key_local_name = buff;
		else
			kb.key_local_name = kb.key_name;

		//.		Msg("[%s]-[%s]",kb.key_name, kb.key_local_name.c_str());
		++idx;
	}
}

LPCSTR id_to_action_name(EGameActions _id)
{
	int idx = 0;
	while (actions[idx].action_name)
	{
		if (_id == actions[idx].id)
			return actions[idx].action_name;
		++idx;
	}
	Msg("! cant find corresponding [action_name] for id");
	return NULL;
}

EGameActions action_name_to_id(LPCSTR _name)
{
	_action* action = action_name_to_ptr(_name);
	if (action)
		return action->id;
	else
		return kNOTBINDED;
}

_action* action_name_to_ptr(LPCSTR _name)
{
	int idx = 0;
	while (actions[idx].action_name)
	{
		if (!stricmp(_name, actions[idx].action_name))
			return &actions[idx];
		++idx;
	}
	Msg("! cant find corresponding [id] for action_name", _name);
	return NULL;
}

LPCSTR dik_to_keyname(int _dik, bool bLocalize)
{
	_keyboard* kb = dik_to_ptr(_dik, true);
	if (kb)
		if (bLocalize)
			return kb->key_local_name.c_str();
		else
			return kb->key_name;
	else
		return NULL;
}

_keyboard* dik_to_ptr(int _dik, bool bSafe)
{
	int idx = 0;
	while (keyboards[idx].key_name)
	{
		_keyboard& kb = keyboards[idx];
		if (kb.dik == _dik)
			return &keyboards[idx];
		++idx;
	}
	if (!bSafe)
		Msg("! cant find corresponding [_keyboard] for dik");
	return NULL;
}

int keyname_to_dik(LPCSTR _name)
{
	_keyboard* _kb = keyname_to_ptr(_name);
	return _kb->dik;
}

_keyboard* keyname_to_ptr(LPCSTR _name)
{
	int idx = 0;
	while (keyboards[idx].key_name)
	{
		_keyboard& kb = keyboards[idx];
		if (!stricmp(_name, kb.key_name))
			return &keyboards[idx];
		++idx;
	}

	Msg("! cant find corresponding [_keyboard*] for keyname %s", _name);
	return NULL;
}

bool is_group_not_conflicted(_key_group g1, _key_group g2)
{
	return ((g1 == _sp && g2 == _mp) || (g1 == _mp && g2 == _sp));
}

bool is_group_matching(_key_group g1, _key_group g2)
{
	return ((g1 == g2) || (g1 == _both) || (g2 == _both));
}

bool is_binded(EGameActions _action_id, int _dik)
{
	_binding* pbinding = &g_key_bindings[_action_id];
	if (pbinding->m_keyboard[0] && pbinding->m_keyboard[0]->dik == _dik)
		return true;

	if (pbinding->m_keyboard[1] && pbinding->m_keyboard[1]->dik == _dik)
		return true;

	if (pbinding->m_keyboard[2] && pbinding->m_keyboard[2]->dik == _dik)
		return true;

	return false;
}

int get_action_dik(EGameActions _action_id, int idx)
{
	_binding* pbinding = &g_key_bindings[_action_id];

	if (idx == -1)
	{
		if (pbinding->m_keyboard[0])
			return pbinding->m_keyboard[0]->dik;

		if (pbinding->m_keyboard[1])
			return pbinding->m_keyboard[1]->dik;

		if (pbinding->m_keyboard[2])
			return pbinding->m_keyboard[2]->dik;
	}
	else
	{
		if (idx >= 0 && idx < binding_slots && pbinding->m_keyboard[idx])
			return pbinding->m_keyboard[idx]->dik;
	}
	return 0;
}

EGameActions get_binded_action(int _dik)
{
	for (int idx = 0; idx < bindings_count; ++idx)
	{
		_binding* binding = &g_key_bindings[idx];

		bool b_is_group_matching = is_group_matching(binding->m_action->key_group, g_current_keygroup);

		if (!b_is_group_matching) continue;

		if (binding->m_keyboard[0] && binding->m_keyboard[0]->dik == _dik && b_is_group_matching)
			return binding->m_action->id;

		if (binding->m_keyboard[1] && binding->m_keyboard[1]->dik == _dik && b_is_group_matching)
			return binding->m_action->id;

		if (binding->m_keyboard[2] && binding->m_keyboard[2]->dik == _dik && b_is_group_matching)
			return binding->m_action->id;
	}
	return kNOTBINDED;
}

void GetActionAllBinding(LPCSTR _action, char* dst_buff, int dst_buff_sz)
{
	int action_id = action_name_to_id(_action);
	_binding* pbinding = &g_key_bindings[action_id];

	string128 prim;
	string128 sec;
	prim[0] = 0;
	sec[0] = 0;

	if (pbinding->m_keyboard[0])
	{
		xr_strcpy(prim, pbinding->m_keyboard[0]->key_local_name.c_str());
	}
	if (pbinding->m_keyboard[1])
	{
		xr_strcpy(sec, pbinding->m_keyboard[1]->key_local_name.c_str());
	}
	if (NULL == pbinding->m_keyboard[0] && NULL == pbinding->m_keyboard[1])
	{
		// the gamepad key only: the keyboard hint strings stay about the keyboard
		xr_sprintf(dst_buff, dst_buff_sz, "%s", CStringTable().translate("st_key_notbinded").c_str());
	}
	else
		xr_sprintf(dst_buff, dst_buff_sz, "%s%s%s", prim[0] ? prim : "", (sec[0] && prim[0]) ? " , " : "",
		           sec[0] ? sec : "");
}

ConsoleBindCmds bindConsoleCmds;
BOOL bRemapped = FALSE;

class CCC_Bind : public IConsole_Command
{
	int m_work_idx;
public:
	CCC_Bind(LPCSTR N, int idx) : IConsole_Command(N), m_work_idx(idx)
	{
	};

	virtual void Execute(LPCSTR args)
	{
		string256 action;
		string256 key;
		*action = 0;
		*key = 0;

		sscanf(args, "%s %s", action, key);
		if (!*action)
			return;

		if (!*key)
			return;

		if (!bRemapped)
		{
			remap_keys();
			bRemapped = TRUE;
		}

		if (!action_name_to_ptr(action))
			return;

		int action_id = action_name_to_id(action);
		if (action_id == kNOTBINDED)
			return;

		_keyboard* pkeyboard = keyname_to_ptr(key);
		if (!pkeyboard)
			return;

		_binding* curr_pbinding = &g_key_bindings[action_id];

		curr_pbinding->m_keyboard[m_work_idx] = pkeyboard;

		{
			for (int idx = 0; idx < bindings_count; ++idx)
			{
				_binding* binding = &g_key_bindings[idx];
				if (binding == curr_pbinding) continue;

				bool b_conflict = !is_group_not_conflicted(binding->m_action->key_group,
				                                           curr_pbinding->m_action->key_group);

				if (binding->m_keyboard[0] == pkeyboard && b_conflict)
					binding->m_keyboard[0] = NULL;

				if (binding->m_keyboard[1] == pkeyboard && b_conflict)
					binding->m_keyboard[1] = NULL;

				if (binding->m_keyboard[2] == pkeyboard && b_conflict)
					binding->m_keyboard[2] = NULL;
			}
		}

#ifdef XR_PLATFORM_APPLE
		// the controller layer emits only the keys of its own map: keep it equal to slot 2
		if (m_work_idx == 2 && pInput && pInput->Pad())
			pInput->Pad()->bindings().Bind(action, pkeyboard->dik);
#endif

		CStringTable::ReparseKeyBindings();
	}

	virtual void Save(IWriter* F)
	{
		if (m_work_idx == 0)
			F->w_printf("default_controls\r\n");

		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			if (pbinding->m_keyboard[m_work_idx])
			{
				F->w_printf("%s %s %s\r\n",
				            cName,
				            pbinding->m_action->action_name,
				            pbinding->m_keyboard[m_work_idx]->key_name);
			}
		}
	}
};

class CCC_UnBind : public IConsole_Command
{
	int m_work_idx;
public:
	CCC_UnBind(LPCSTR N, int idx) : IConsole_Command(N), m_work_idx(idx)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		int action_id = action_name_to_id(args);
		_binding* pbinding = &g_key_bindings[action_id];
		pbinding->m_keyboard[m_work_idx] = NULL;
#ifdef XR_PLATFORM_APPLE
		if (m_work_idx == 2 && pInput && pInput->Pad())
			pInput->Pad()->bindings().Unbind(args);
#endif

		CStringTable::ReparseKeyBindings();
	}
};

class CCC_ListActions : public IConsole_Command
{
public:
	CCC_ListActions(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		Log("- --- Action list start ---");
		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			Log("-", pbinding->m_action->action_name);
		}
		Log("- --- Action list end   ---");
	}
};

class CCC_UnBindAll : public IConsole_Command
{
public:
	CCC_UnBindAll(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			pbinding->m_keyboard[0] = NULL;
			pbinding->m_keyboard[1] = NULL;
			pbinding->m_keyboard[2] = NULL;
		}
#ifdef XR_PLATFORM_APPLE
		if (pInput && pInput->Pad())
			pInput->Pad()->bindings().Clear();
#endif
		bindConsoleCmds.clear();
	}
};

class CCC_DefControls : public CCC_UnBindAll
{
public:
	CCC_DefControls(LPCSTR N) : CCC_UnBindAll(N)
	{
	}

	virtual void Execute(LPCSTR args)
	{
		CCC_UnBindAll::Execute(args);
		string_path _cfg;
		string_path cmd;
		FS.update_path(_cfg, "$game_config$", "default_controls.ltx");
		strconcat(sizeof(cmd), cmd, "cfg_load", " ", _cfg);
		Console->Execute("bind editor kF11"); //Lucy : ImGui default key
		Console->Execute(cmd);
#ifdef XR_PLATFORM_APPLE
		// the default DualSense layout (bind_pad lines; docs/ios-port/CONTROLS.md)
		std::istringstream pad_lines(gameinput::BindingMap::Default().Serialize());
		for (std::string line; std::getline(pad_lines, line);)
			if (!line.empty())
				Console->Execute(line.c_str());
#endif
	}
};

class CCC_BindList : public IConsole_Command
{
public:
	CCC_BindList(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		Log("- --- Bind list start ---");
		string512 buff;

		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			xr_sprintf(buff, "[%s] primary is[%s] secondary is[%s] gamepad is[%s]",
			           pbinding->m_action->action_name,
			           (pbinding->m_keyboard[0]) ? pbinding->m_keyboard[0]->key_local_name.c_str() : "NULL",
			           (pbinding->m_keyboard[1]) ? pbinding->m_keyboard[1]->key_local_name.c_str() : "NULL",
			           (pbinding->m_keyboard[2]) ? pbinding->m_keyboard[2]->key_local_name.c_str() : "NULL");
			Log(buff);
		}
		Log("- --- Bind list end   ---");
	}
};

class CCC_BindConsoleCmd : public IConsole_Command
{
public:
	CCC_BindConsoleCmd(LPCSTR N) : IConsole_Command(N)
	{
	};

	virtual void Execute(LPCSTR args)
	{
		string512 console_command;
		string256 key;
		int cnt = _GetItemCount(args, ' ');
		_GetItems(args, 0, cnt - 1, console_command, ' ');
		_GetItem(args, cnt - 1, key, ' ');

		int dik = keyname_to_dik(key);
		bindConsoleCmds.bind(dik, console_command);
	}

	virtual void Save(IWriter* F)
	{
		bindConsoleCmds.save(F);
	}
};


class CCC_UnBindConsoleCmd : public IConsole_Command
{
public:
	CCC_UnBindConsoleCmd(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = FALSE;
	};

	virtual void Execute(LPCSTR args)
	{
		int _dik = keyname_to_dik(args);
		bindConsoleCmds.unbind(_dik);
	}
};

void ConsoleBindCmds::bind(int dik, LPCSTR N)
{
	_conCmd& c = m_bindConsoleCmds[dik];
	c.cmd = N;
}

void ConsoleBindCmds::unbind(int dik)
{
	xr_map<int, _conCmd>::iterator it = m_bindConsoleCmds.find(dik);
	if (it == m_bindConsoleCmds.end())
		return;

	m_bindConsoleCmds.erase(it);
}

void ConsoleBindCmds::clear()
{
	m_bindConsoleCmds.clear();
}

bool ConsoleBindCmds::execute(int dik)
{
	xr_map<int, _conCmd>::iterator it = m_bindConsoleCmds.find(dik);
	if (it == m_bindConsoleCmds.end())
		return false;

	Console->Execute(it->second.cmd.c_str());
	return true;
}

void ConsoleBindCmds::save(IWriter* F)
{
	xr_map<int, _conCmd>::iterator it = m_bindConsoleCmds.begin();

	for (; it != m_bindConsoleCmds.end(); ++it)
	{
		LPCSTR keyname = dik_to_keyname(it->first, false);
		F->w_printf("bind_console %s %s\n", *it->second.cmd, keyname);
	}
}


void CCC_RegisterInput()
{
	initialize_bindings();
	CMD2(CCC_Bind, "bind", 0);
	CMD2(CCC_Bind, "bind_sec", 1);
	CMD2(CCC_UnBind, "unbind", 0);
	CMD2(CCC_UnBind, "unbind_sec", 1);
#ifdef XR_PLATFORM_APPLE
	CMD2(CCC_Bind, "bind_pad", 2);
	CMD2(CCC_UnBind, "unbind_pad", 2);
#endif
	CMD1(CCC_UnBindAll, "unbindall");
	CMD1(CCC_DefControls, "default_controls");
	CMD1(CCC_ListActions, "list_actions");

	CMD1(CCC_BindList, "bind_list");
	CMD1(CCC_BindConsoleCmd, "bind_console");
	CMD1(CCC_UnBindConsoleCmd, "unbind_console");
};

#ifdef XR_PLATFORM_APPLE
// ---- Controller layer (src/Platform/GameInput) -----------------------------------------------------------------

#include <GameInput/GameInput.h>
#include <GameInput/Feedback.h>
#include "Actor.h"
#include "ActorCondition.h"
#include "Weapon.h"
#include "WeaponMagazined.h"
#include "UICursor.h"
#include "ui_base.h"
#include "ui/UIStatic.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "ui/UIInventoryUtilities.h"

void xr_pad_install_hooks()
{
	if (!pInput)
		return;
	pInput->pad_hooks.cursor_jump = [](float ui_x, float ui_y)
	{
		Fvector2 pos;
		pos.set(ui_x, ui_y);
		GetUICursor().SetUICursorPosition(pos);
	};
	// The last controller was unplugged (or powered off): hold the game until one is back.
	pInput->pad_hooks.connection = [](bool connected)
	{
		static bool paused_by_pad = false;
		if (!connected)
		{
			// Another controller may still be connected (the registry has already promoted it): only the last one counts.
			gameinput::GameInput* pad = pInput ? pInput->Pad() : nullptr;
			if (pad && pad->devices().AnyConnected())
				return;
			Msg("! game controller disconnected, reconnect it to continue");
			if (g_pGameLevel && !Device.Paused())
			{
				Device.Pause(TRUE, TRUE, TRUE, "gamepad_disconnected");
				paused_by_pad = true;
			}
		}
		else
		{
			Msg("* game controller connected");
			if (paused_by_pad)
			{
				paused_by_pad = false;
				Device.Pause(FALSE, TRUE, TRUE, "gamepad_disconnected");
			}
		}
	};
}

void xr_pad_ui_context(bool ui, const Fvector2& cursor)
{
	if (!pInput)
		return;
	pInput->SetUiContext(ui);
	if (gameinput::GameInput* pad = pInput->Pad())
	{
		pad->feedback().SetMenu(ui);
		if (ui)
			pad->SetCursorPosition(gameinput::Vec2(cursor.x, cursor.y));
	}
}

static gameinput::GameInput* controlled_pad(CActor* actor)
{
	return (pInput && actor && actor == Level().CurrentControlEntity()) ? pInput->Pad() : nullptr;
}

static gameinput::WeaponClass pad_weapon_class(CWeapon* weapon)
{
	return weapon ? gameinput::ClassifyWeapon(weapon->cNameSect().c_str()) : gameinput::WeaponClass::None;
}

void xr_pad_update_actor(CActor* actor)
{
	gameinput::GameInput* pad = controlled_pad(actor);
	if (!pad)
		return;
	gameinput::Feedback& fb = pad->feedback();
	CActorCondition& cond = actor->conditions();
	fb.SetHealth(cond.GetHealth());
	fb.SetBleeding(cond.BleedingSpeed() > 0.01f);
	fb.SetRadiation(cond.GetRadiation());
	fb.SetAiming(actor->IsZoomAimingMode());

	PIItem item = actor->inventory().ActiveItem();
	CWeapon* weapon = item ? smart_cast<CWeapon*>(item) : nullptr;
	if (!weapon)
	{
		fb.SetWeapon(gameinput::WeaponClass::None, false);
		fb.SetWeaponStatus(false, false, false);
		return;
	}
	CWeaponMagazined* magazined = smart_cast<CWeaponMagazined*>(weapon);
	const bool automatic = magazined && magazined->GetCurrentFireMode() != 1 && magazined->GetCurrentFireMode() != 0;
	fb.SetWeapon(pad_weapon_class(weapon), automatic);
	fb.SetWeaponStatus(weapon->GetAmmoElapsed() == 0, weapon->IsMisfire(), weapon->GetState() == CWeapon::eReload);
}

void xr_pad_on_hit(int hit_type, float power)
{
	gameinput::GameInput* pad = controlled_pad(Actor());
	if (!pad)
		return;
	gameinput::HitType type;
	switch (hit_type)
	{
	case ALife::eHitTypeFireWound:
	case ALife::eHitTypeWound:
	case ALife::eHitTypeWound_2:
		type = gameinput::HitType::Bullet;
		break;
	case ALife::eHitTypeStrike:
		type = gameinput::HitType::Melee;
		break;
	case ALife::eHitTypeExplosion:
		type = gameinput::HitType::Explosion;
		break;
	case ALife::eHitTypeBurn:
	case ALife::eHitTypeLightBurn:
		type = gameinput::HitType::Fire;
		break;
	case ALife::eHitTypeShock:
		type = gameinput::HitType::Shock;
		break;
	case ALife::eHitTypeChemicalBurn:
		type = gameinput::HitType::Chemical;
		break;
	case ALife::eHitTypeRadiation:
		type = gameinput::HitType::Radiation;
		break;
	case ALife::eHitTypeTelepatic:
		type = gameinput::HitType::Psy;
		break;
	default:
		return;
	}
	pad->feedback().OnHit(type, power < 0.f ? 0.f : (power > 1.f ? 1.f : power)); // max health is 1.0
}

void xr_pad_on_shot(CWeapon* weapon)
{
	if (gameinput::GameInput* pad = controlled_pad(weapon ? smart_cast<CActor*>(weapon->H_Parent()) : nullptr))
		pad->feedback().OnShot();
}

void xr_pad_on_dry_fire(CWeapon* weapon)
{
	if (gameinput::GameInput* pad = controlled_pad(weapon ? smart_cast<CActor*>(weapon->H_Parent()) : nullptr))
		pad->feedback().OnDryFire();
}
#endif // XR_PLATFORM_APPLE

// The radial wheel (hold L1 or R1). The layer keeps the model (which wheel, page and slot the stick points at); the engine had nothing that draws it,
// so the HUD calls this. It is made of the game's own parts and palette: flat dark tiles with the item icons of the inventory (the same atlas and
// rects the inventory grid uses), the menus' amber (216,186,140) for the picked slot, the HUD fonts. Slots sit on a circle, clockwise from the top.
//
// Motion (high-frequency UI, so short and soft): the wheel fades in and grows from 94% over 120 ms with an ease-out (never from nothing), the picked
// tile grows to 110% and warms up with an exponential approach that is interrupted and retargeted every frame, and closing is instant: releasing
// the button already runs the action. Every state also has a static cue (amber border and name in the hub), motion is never the only one.
namespace
{
struct RadialEntry
{
	std::string name;   // the hub's headline when this slot is picked
	std::string detail; // the hub's second line
	std::string badge;  // small corner text: the inventory slot number, the quick-use key
	CInventoryItem* item = nullptr;
	bool item_slot = false; // an inventory slot (drawn with an icon or as empty) rather than a plain action
};

const float INV_GRID_WIDTHF = 50.f, INV_GRID_HEIGHTF = 50.f; // the inventory grid's cell, as in ui/UICellCustomItems.cpp

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
float ease_out(float t) // ~ cubic-bezier(0.23, 1, 0.32, 1)
{
	const float u = 1.f - clamp01(t);
	return 1.f - u * u * u * u;
}
u32 scale_alpha(u32 color, float k) { return color_argb(u32(float(color >> 24) * clamp01(k)), (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff); }
u32 mix(u32 c0, u32 c1, float t)
{
	auto ch = [&](int shift) { return u32(float((c0 >> shift) & 0xff) + (float((c1 >> shift) & 0xff) - float((c0 >> shift) & 0xff)) * clamp01(t)); };
	return color_argb(ch(24), ch(16), ch(8), ch(0));
}

const u32 kAmber = color_argb(255, 216, 186, 140); // the menus' text colour
const u32 kTileBase = color_argb(255, 24, 26, 28);
const u32 kTileHot = color_argb(255, 46, 40, 30);
const u32 kEdgeBase = color_argb(255, 78, 82, 84);
const u32 kGrey = color_argb(255, 150, 152, 150);

void describe(const std::string& action, RadialEntry& e)
{
	CActor* actor = Actor();
	if (action.size() == 5 && !action.compare(0, 4, "wpn_") && action[4] >= '1' && action[4] <= '6')
	{
		const u16 slot = u16(action[4] - '0');
		e.item_slot = true;
		e.badge = std::string(1, action[4]);
		e.item = actor ? actor->inventory().ItemFromSlot(slot) : nullptr;
		static const char* kinds[] = {"", "Melee", "Pistol", "Primary weapon", "Grenade", "Binoculars", "Bolt"};
		e.detail = kinds[slot];
	}
	else if (action.size() == 11 && !action.compare(0, 10, "quick_use_") && action[10] >= '1' && action[10] <= '4')
	{
		const int i = action[10] - '1';
		e.item_slot = true;
		e.badge = std::string("F") + action[10];
		e.detail = "Quick use";
		if (actor && ACTOR_DEFS::g_quick_use_slots[i] && ACTOR_DEFS::g_quick_use_slots[i][0])
			e.item = actor->inventory().GetAny(ACTOR_DEFS::g_quick_use_slots[i]);
	}
	else if (action == "artefact" || action == "show_detector")
	{
		e.item_slot = true;
		e.detail = action == "artefact" ? "Artefact" : "Detector";
		e.item = actor ? actor->inventory().ItemFromSlot(action == "artefact" ? ARTEFACT_SLOT : DETECTOR_SLOT) : nullptr;
	}
	else if (!action.compare(0, 6, "custom"))
		e.detail = "Addon action";
	if (e.item)
		e.name = e.item->NameItem();
	else if (e.item_slot)
		e.name = "Empty";
}

class CPadRadialUi
{
	static const int kMaxSlots = 12;
	CUIStatic m_flat, m_icon, m_text;
	float m_open = 0.f;
	float m_hot[kMaxSlots] = {};
	u32 m_last_frame = u32(-1);

	void rect(float x, float y, float w, float h, u32 color)
	{
		m_flat.SetWndPos(Fvector2().set(x, y));
		m_flat.SetWndSize(Fvector2().set(w, h));
		m_flat.SetTextureColor(color);
		m_flat.Draw();
	}

	void text(CGameFont* font, float x, float y, float w, float h, u32 color, const char* str, bool wrap = false)
	{
		CUILines* lines = m_text.TextItemControl();
		lines->SetFont(font);
		lines->SetTextColor(color);
		lines->SetTextAlignment(CGameFont::alCenter);
		lines->SetVTextAlignment(valCenter);
		lines->SetTextComplexMode(wrap);
		lines->SetText(str);
		m_text.SetWndPos(Fvector2().set(x, y));
		m_text.SetWndSize(Fvector2().set(w, h));
		m_text.Draw();
	}

	// the item's icon, fitted into the square (cx, cy, side)
	void icon(CInventoryItem* item, float cx, float cy, float side, float alpha)
	{
		const char* section = item->m_section_id.c_str();
		if (pSettings->line_exist(section, "icons_texture"))
			m_icon.SetShader(InventoryUtilities::GetCustomIconTextureShader(pSettings->r_string(section, "icons_texture")));
		else
			m_icon.SetShader(InventoryUtilities::GetEquipmentIconsShader());
		const Irect grid = item->GetInvGridRect();
		Frect tex;
		tex.lt.set(INV_GRID_WIDTHF * grid.x1, INV_GRID_HEIGHTF * grid.y1);
		tex.rb.set(tex.lt.x + INV_GRID_WIDTHF * grid.x2, tex.lt.y + INV_GRID_HEIGHTF * grid.y2);
		m_icon.SetTextureRect(tex);
		const float w = INV_GRID_WIDTHF * grid.x2, h = INV_GRID_HEIGHTF * grid.y2;
		const float k = std::min(side / w, side / h);
		m_icon.SetWndSize(Fvector2().set(w * k, h * k));
		m_icon.SetWndPos(Fvector2().set(cx - w * k * 0.5f, cy - h * k * 0.5f));
		m_icon.SetTextureColor(color_argb(u32(255.f * alpha), 255, 255, 255));
		m_icon.Draw();
	}

	void tile(float cx, float cy, float side, float hot, float alpha, const RadialEntry& e, const std::string& label)
	{
		const float k = (0.94f + 0.06f * ease_out(m_open)) * (1.f + 0.10f * ease_out(hot));
		const float s = side * k, x = cx - s * 0.5f, y = cy - s * 0.5f;
		// elevation: two soft shadow layers, then the tile; the 1.2 unit edge is structure (amber when picked)
		if (hot > 0.01f) // the picked tile also glows: a faint amber halo, the static cue is the amber edge and the hub's name
			rect(x - 5.f, y - 5.f, s + 10.f, s + 10.f, scale_alpha(color_argb(34, 216, 186, 140), alpha * ease_out(hot)));
		rect(x - 3.f, y + 1.f, s + 6.f, s + 8.f, scale_alpha(color_argb(46, 0, 0, 0), alpha));
		rect(x - 1.f, y + 3.f, s + 2.f, s + 3.f, scale_alpha(color_argb(70, 0, 0, 0), alpha));
		const bool dim = e.item_slot && !e.item;
		rect(x, y, s, s, scale_alpha(mix(kTileBase, kTileHot, ease_out(hot)), alpha * (dim ? 0.78f : 0.94f)));
		rect(x + 1.2f, y + 1.2f, s - 2.4f, 1.f, scale_alpha(color_argb(20, 255, 255, 255), alpha)); // a faint top light
		const u32 edge = scale_alpha(mix(kEdgeBase, kAmber, ease_out(hot)), alpha * (dim ? 0.6f : 1.f));
		const float t = 1.2f;
		rect(x, y, s, t, edge);
		rect(x, y + s - t, s, t, edge);
		rect(x, y + t, t, s - 2.f * t, edge);
		rect(x + s - t, y + t, t, s - 2.f * t, edge);

		if (e.item)
			icon(e.item, cx, cy + 2.f, s - 16.f, alpha * (0.86f + 0.14f * ease_out(hot)));
		else if (e.item_slot)
		{
			rect(cx - 8.f, cy, 16.f, 1.5f, scale_alpha(color_argb(110, 150, 152, 150), alpha)); // an empty slot: a dash
			if (e.badge.empty()) // the slots without a number say what they are
				text(UI().Font().pFontArial14, x + 2.f, y + s - 22.f, s - 4.f, 18.f, scale_alpha(color_argb(255, 120, 122, 120), alpha), e.detail.c_str());
		}
		else
			text(UI().Font().pFontLetterica16Russian, x + 4.f, y + 4.f, s - 8.f, s - 8.f, scale_alpha(mix(kAmber, color_argb(255, 255, 240, 205), hot), alpha),
			    label.c_str(), true);
		if (!e.badge.empty())
			text(UI().Font().pFontArial14, x + 3.f, y + 2.f, 24.f, 16.f, scale_alpha(mix(kGrey, kAmber, hot), alpha), e.badge.c_str());
	}

public:
	CPadRadialUi()
	{
		m_flat.InitTexture("ui_inGame2_white_rect");
		m_flat.SetStretchTexture(true);
		m_icon.SetStretchTexture(true);
	}

	void Draw(const gameinput::RadialView& view, const gameinput::RadialWheel& wheel, const gameinput::RadialPage& page,
	    const std::vector<std::string>& labels, const std::vector<RadialEntry>& entries)
	{
		const float dt = std::min(Device.fTimeDelta, 0.05f);
		if (Device.dwFrame - m_last_frame > 2) // not drawn for a while: this is a fresh opening
		{
			m_open = 0.f;
			std::fill(std::begin(m_hot), std::end(m_hot), 0.f);
		}
		m_last_frame = Device.dwFrame;
		m_open = clamp01(m_open + dt / 0.12f);
		const float alpha = ease_out(m_open);
		const int n = std::min(int(entries.size()), kMaxSlots);
		for (int i = 0; i < n; ++i)
		{
			const float target = i == view.selected ? 1.f : 0.f;
			m_hot[i] += (target - m_hot[i]) * (1.f - expf(-dt * 20.f));
		}

		const float cx = UI_BASE_WIDTH * 0.5f, cy = UI_BASE_HEIGHT * 0.5f;
		rect(0.f, 0.f, UI_BASE_WIDTH, UI_BASE_HEIGHT, scale_alpha(color_argb(130, 0, 0, 0), alpha)); // the veil

		// hub: the picked slot's name, or the wheel's title while nothing is picked; a page dot per page
		const bool picked = view.selected >= 0 && view.selected < n;
		const std::string headline = picked ? (entries[view.selected].name.empty() ? labels[view.selected] : entries[view.selected].name) : wheel.name;
		const std::string detail = picked ? (entries[view.selected].detail.empty() ? page.title : entries[view.selected].detail) : page.title;
		const float hub_w = 250.f;
		text(UI().Font().pFontGraffiti22Russian, cx - hub_w * 0.5f, cy - 34.f, hub_w, 34.f, scale_alpha(picked ? kAmber : color_argb(255, 200, 202, 200), alpha),
		    headline.c_str());
		text(UI().Font().pFontLetterica16Russian, cx - hub_w * 0.5f, cy, hub_w, 22.f, scale_alpha(kGrey, alpha), detail.c_str());
		if (view.page_count > 1)
		{
			const float step = 12.f, x0 = cx - step * float(view.page_count - 1) * 0.5f - 2.5f;
			for (int p = 0; p < view.page_count; ++p)
				rect(x0 + step * float(p), cy + 30.f, 5.f, 5.f, scale_alpha(p == view.page ? kAmber : color_argb(255, 90, 92, 92), alpha));
		}

		const float side = 100.f, radius = 194.f, kx = UI().get_current_kx();
		for (int i = 0; i < n; ++i)
		{
			const float angle = -0.5f * 3.1415927f + 6.2831853f * float(i) / float(n);
			if (i != view.selected)
				tile(cx + radius * cosf(angle) * kx, cy + radius * sinf(angle), side, m_hot[i], alpha, entries[i], labels[i]);
		}
		if (picked) // the picked tile is drawn last so its shadow and growth sit on top of its neighbours
		{
			const float angle = -0.5f * 3.1415927f + 6.2831853f * float(view.selected) / float(n);
			tile(cx + radius * cosf(angle) * kx, cy + radius * sinf(angle), side, m_hot[view.selected], alpha, entries[view.selected], labels[view.selected]);
		}
	}
};

CPadRadialUi* g_pad_radial_ui = nullptr;
} // namespace

void xr_pad_render_radial()
{
	gameinput::GameInput* pad = pInput ? pInput->Pad() : nullptr;
	if (!pad)
		return;
	const gameinput::RadialView view = pad->radial().View();
	if (!view.open || view.wheel < 0)
		return;
	const std::vector<gameinput::RadialWheel>& wheels = pad->radial().Wheels();
	if (size_t(view.wheel) >= wheels.size() || size_t(view.page) >= wheels[view.wheel].pages.size())
		return;
	const gameinput::RadialWheel& wheel = wheels[view.wheel];
	const gameinput::RadialPage& page = wheel.pages[view.page];
	if (page.slots.empty())
		return;

	std::vector<std::string> labels;
	std::vector<RadialEntry> entries;
	for (const gameinput::RadialSlot& slot : page.slots)
	{
		const std::string* action = pad->bindings().ActionForKey(slot.key);
		labels.push_back(action ? gameinput::ActionLabel(*action) : slot.label);
		RadialEntry entry;
		if (action)
			describe(*action, entry);
		entries.push_back(entry);
	}
	if (!g_pad_radial_ui)
		g_pad_radial_ui = xr_new<CPadRadialUi>();
	g_pad_radial_ui->Draw(view, wheel, page, labels, entries);
}
