////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator_base.h
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator base class
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "random32.h"
#include "alife_space.h"
#include "game_graph_space.h"
#include "object_interfaces.h"

class xrServer;
class CALifeSimulatorHeader;
class CALifeTimeManager;
class CALifeSpawnRegistry;
class CALifeObjectRegistry;
class CALifeGraphRegistry;
class CALifeScheduleRegistry;
class CALifeStoryRegistry;
class CALifeSmartTerrainRegistry;
class CALifeGroupRegistry;
class CALifeRegistryContainer;

class CSE_Abstract;
class CSE_ALifeObject;
class CSE_ALifeDynamicObject;
class CSE_ALifeGroupAbstract;
class CSE_ALifeCreatureAbstract;

namespace inventory
{
	namespace upgrade
	{
		class Manager;
	}
} // namespace upgrade, inventory

class CALifeSimulatorBase : public IPureDestroyableObject
{
protected:
	xrServer* m_server;
	CALifeSimulatorHeader* m_header;
	CALifeTimeManager* m_time_manager;
	CALifeSpawnRegistry* m_spawns;
	CALifeObjectRegistry* m_objects;
	CALifeGraphRegistry* m_graph_objects;
	CALifeScheduleRegistry* m_scheduled;
	CALifeStoryRegistry* m_story_objects;
	CALifeSmartTerrainRegistry* m_smart_terrains;
	CALifeGroupRegistry* m_groups;
	CALifeRegistryContainer* m_registry_container;
	inventory::upgrade::Manager* m_upgrade_manager;
	CRandom32 m_random;
	bool m_initialized;
	shared_str* m_server_command_line;
	bool m_can_register_objects;
	// temp
	ALife::SCHEDULE_P_VECTOR m_tpaCombatGroups[2];

private:
	xr_vector<CSE_Abstract*> m_deferred_destroy;
	xrCriticalSection m_deferred_destroy_lock;
	void flush_deferred_destroy();

protected:
	IC CALifeSimulatorHeader& header();
	IC CALifeTimeManager& time();
	IC CALifeSpawnRegistry& spawns();
	IC CALifeObjectRegistry& objects();
	IC CALifeStoryRegistry& story_objects();
	IC CALifeSmartTerrainRegistry& smart_terrains();
	IC CALifeGroupRegistry& groups();
	IC void can_register_objects(const bool& value);
	IC const bool& can_register_objects() const;

public:
	IC CALifeGraphRegistry& graph();
	IC CALifeScheduleRegistry& scheduled();
	IC CALifeTimeManager& time_manager();
	IC CALifeRegistryContainer& registry() const;
	IC inventory::upgrade::Manager& inventory_upgrade_manager() const;

public:
	CALifeSimulatorBase(xrServer* server, LPCSTR section);
	virtual ~CALifeSimulatorBase();
	virtual void destroy();
	IC bool initialized() const;
	IC const CALifeSimulatorHeader& header() const;
	IC const CALifeTimeManager& time() const;
	IC const CALifeSpawnRegistry& spawns() const;
	IC const CALifeObjectRegistry& objects() const;
	IC const CALifeGraphRegistry& graph() const;
	IC const CALifeScheduleRegistry& scheduled() const;
	IC const CALifeStoryRegistry& story_objects() const;
	IC const CALifeSmartTerrainRegistry& smart_terrains() const;
	IC const CALifeGroupRegistry& groups() const;
	IC CRandom32& random();
	IC xrServer& server() const;
	IC const CALifeTimeManager& time_manager() const;
	IC shared_str* server_command_line() const;
	template <typename T>
	IC T& registry(T* t) const;

protected:
	void unload();
	virtual void reload(LPCSTR section);
	IC void setup_command_line(shared_str* command_line);
	void assign_death_position(CSE_ALifeCreatureAbstract* tpALifeCreatureAbstract, GameGraph::_GRAPH_ID tGraphID,
	                           CSE_ALifeSchedulable* tpALifeSchedulable = 0);
	virtual void setup_simulator(CSE_ALifeObject* object) = 0;

public:
	void register_object(CSE_ALifeDynamicObject* object, bool add_object = false);
	void unregister_object(CSE_ALifeDynamicObject* object, bool alife_query = true);
	void release(CSE_Abstract* object, bool alife_query = true);
	void create(CSE_ALifeDynamicObject*& object, CSE_ALifeDynamicObject* spawn_object,
	            const ALife::_SPAWN_ID& spawn_id);
	void create(CSE_ALifeObject* object);
	CSE_Abstract* create(CSE_ALifeGroupAbstract* object, CSE_ALifeDynamicObject* j);
	CSE_Abstract* spawn_item(LPCSTR section, const Fvector& position, u32 level_vertex_id,
	                         GameGraph::_GRAPH_ID game_vertex_id, u16 parent_id, bool registration = true);
	void append_item_vector(ALife::OBJECT_VECTOR& tObjectVector, ALife::ITEM_P_VECTOR& tItemList);
	shared_str level_name() const;
	void on_death(CSE_Abstract* killed, CSE_Abstract* killer);

	// Parks `object` instead of destroying it, for a server entity a script may still hold: `alife():register(se_obj)` replaces
	// the object it is given and the script goes on reading the old one. Nothing can tell when the last Lua reference is gone, so
	// a parked entity lives as long as the simulator, and with it the Lua state that could hold it (unload()), at most
	// deferred_destroy_cap of them (the oldest goes first).
	void defer_destroy(CSE_Abstract* object);

public:
	ALife::ITEM_P_VECTOR m_temp_item_vector;
};

#include "alife_simulator_base_inline.h"
