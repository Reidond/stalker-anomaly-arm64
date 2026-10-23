#include "stdafx.h"
#include "PHCommander.h"

#include "phsimplecalls.h"
#ifdef DEBUG
//#include "phworld.h"
//extern CPHWorld	*ph_world;
#include "../xrphysics/IPHWorld.h"
#endif

CPHCall::CPHCall(CPHCondition* condition, CPHAction* action)
{
	m_action = action;
	m_condition = condition;
}

CPHCall::~CPHCall()
{
	xr_delete(m_action);
	xr_delete(m_condition);
}

bool CPHCall::obsolete()
{
	return !m_action || m_action->obsolete() || !m_condition || m_condition->obsolete();
}

void CPHCall::check()
{
	if (m_condition && m_condition->is_true() && m_action) m_action->run();
}

bool CPHCall::equal(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
	return m_action->compare(cmp_action) && m_condition->compare(cmp_condition);
}

bool CPHCall::is_any(CPHReqComparerV* v)
{
	return m_action->compare(v) || m_condition->compare(v);
}

void delete_call(CPHCall* & call)
{
	try
	{
		xr_delete(call);
	}
	catch (...)
	{
		call = NULL;
	}
}

/////////////////////////////////////////////////////////////////////////////////
CPHCommander::~CPHCommander()
{
	clear();
}

void CPHCommander::clear()
{
	if (m_update_depth)
	{
		for (u32 i = 0; i < m_calls.size(); i++)
			defer_call(m_calls[i]);
		return;
	}
	while (m_calls.size())
	{
		remove_call(m_calls.end() - 1);
	}
	while (m_calls_as_add_buffer.size())
	{
		remove_call(m_calls_as_add_buffer.end() - 1);
	}
	while (m_calls_as_remove_buffer.size())
	{
		remove_call(m_calls_as_remove_buffer.end() - 1);
	}
}

void CPHCommander::defer_call(CPHCall*& slot)
{
	if (!slot)
		return;
	m_deferred.push_back(slot);
	slot = NULL;
}

void CPHCommander::end_update()
{
	if (--m_update_depth)
		return;
	m_calls.erase(std::remove(m_calls.begin(), m_calls.end(), (CPHCall*)NULL), m_calls.end());
	while (m_deferred.size())
	{
		CPHCall* call = m_deferred.back();
		m_deferred.pop_back();
		delete_call(call); // (its destructor may remove calls too: m_deferred is read again each turn)
	}
}

void CPHCommander::update()
{
	++m_update_depth;
	// The slot i is stable while the loop runs: nothing is erased from m_calls then (remove_call*() clear the slot and defer the
	// deletion), a call added by an action is appended, and m_calls is indexed afresh each time because the append may move it.
	for (u32 i = 0; i < m_calls.size(); i++)
	{
		if (!m_calls[i])
			continue;
		try
		{
			m_calls[i]->check();
		}
		catch (...)
		{
			defer_call(m_calls[i]);
			continue;
		}

		if (m_calls[i] && m_calls[i]->obsolete())
			defer_call(m_calls[i]);
	}
	end_update();
}

void CPHCommander::update_threadsafety()
{
	lock.Enter();
	update();
	lock.Leave();
}

void CPHCommander::add_call_threadsafety(CPHCondition* condition, CPHAction* action)
{
	lock.Enter();
	add_call(condition, action);
	lock.Leave();
}

void CPHCommander::add_call(CPHCondition* condition, CPHAction* action, PHCALL_STORAGE& cs)
{
	cs.push_back(xr_new<CPHCall>(condition, action));
}

void CPHCommander::add_call(CPHCondition* condition, CPHAction* action)
{
	add_call(condition, action, m_calls);
}

void CPHCommander::remove_call(PHCALL_I i, PHCALL_STORAGE& cs)
{
#ifdef DEBUG
	const CPHCallOnStepCondition	* esc  = smart_cast<const CPHCallOnStepCondition*>((*i)->condition());
	const CPHConstForceAction		* cfa = smart_cast<const CPHConstForceAction*>((*i)->action());
	if(esc&&cfa)
	{
		Fvector f = cfa->force();
		float m = f.magnitude();
		if(m>EPS_S)
				f.mul(1.f/m);
		//Msg(" const force removed: force: %f,  remove step: %d  world step: %d ,dir(%f,%f,%f) ", m, esc->step(), (u32)physics_world()->StepsNum(), f.x, f.y , f.z ); 
	}
#endif
	if (deferring(cs))
	{
		defer_call(*i);
		return;
	}
	delete_call(*i);
	cs.erase(i);
}

void CPHCommander::remove_call(PHCALL_I i)
{
	remove_call(i, m_calls);
}


struct SFEqualPred
{
	CPHReqComparerV *cmp_condition, *cmp_action;

	SFEqualPred(CPHReqComparerV* cmp_c, CPHReqComparerV* cmp_a)
	{
		cmp_condition = cmp_c;
		cmp_action = cmp_a;
	}

	bool operator()(CPHCall* call)
	{
		return call && call->equal(cmp_condition, cmp_action);
	}
};

struct SFRemovePred2
{
	CPHReqComparerV *cmp_condition, *cmp_action;

	SFRemovePred2(CPHReqComparerV* cmp_c, CPHReqComparerV* cmp_a)
	{
		cmp_condition = cmp_c;
		cmp_action = cmp_a;
	}

	bool operator()(CPHCall* call)
	{
		if (call && call->equal(cmp_condition, cmp_action))
		{
			delete_call(call);
			return true;
		}
		return false;
	}
};

PHCALL_I CPHCommander::find_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action, PHCALL_STORAGE& cs)
{
	return std::find_if(cs.begin(), cs.end(), SFEqualPred(cmp_condition, cmp_action));
}

PHCALL_I CPHCommander::find_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
	return find_call(cmp_condition, cmp_action, m_calls);
}

bool CPHCommander::has_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
	return find_call(cmp_condition, cmp_action) != m_calls.end();
}

void CPHCommander::remove_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action, PHCALL_STORAGE& cs)
{
	if (deferring(cs))
	{
		SFEqualPred match(cmp_condition, cmp_action);
		for (u32 i = 0; i < cs.size(); i++)
			if (match(cs[i]))
				defer_call(cs[i]);
		return;
	}
	cs.erase(
		std::remove_if(
			cs.begin(),
			cs.end(),
			SFRemovePred2(
				cmp_condition,
				cmp_action
			)
		),
		cs.end()
	);
}

void CPHCommander::remove_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
	remove_call(cmp_condition, cmp_action, m_calls);
}

bool CPHCommander::add_call_unique(CPHCondition* condition, CPHReqComparerV* cmp_condition, CPHAction* action,
                                   CPHReqComparerV* cmp_action, PHCALL_STORAGE& cs)
{
	if (cs.end() == find_call(cmp_condition, cmp_action, cs))
	{
		add_call(condition, action, cs);
		return true;
	}
	return false;
}

bool CPHCommander::add_call_unique(CPHCondition* condition, CPHReqComparerV* cmp_condition, CPHAction* action,
                                   CPHReqComparerV* cmp_action)
{
	return add_call_unique(condition, cmp_condition, action, cmp_action, m_calls);
}

struct SRemoveRped
{
	CPHReqComparerV* cmp_object;

	SRemoveRped(CPHReqComparerV* cmp_o)
	{
		cmp_object = cmp_o;
	}

	bool operator()(CPHCall* call)
	{
		if (call && call->is_any(cmp_object))
		{
			delete_call(call);
			return true;
		}
		else
			return false;
	}
};

void CPHCommander::remove_calls(CPHReqComparerV* cmp_object, PHCALL_STORAGE& cs)
{
	if (deferring(cs))
	{
		for (u32 i = 0; i < cs.size(); i++)
			if (cs[i] && cs[i]->is_any(cmp_object))
				defer_call(cs[i]);
		return;
	}
	cs.erase(
		std::remove_if(
			cs.begin(),
			cs.end(),
			SRemoveRped(cmp_object)
		),
		cs.end()
	);
}

void CPHCommander::remove_calls_threadsafety(CPHReqComparerV* cmp_object)
{
	lock.Enter();
	remove_calls(cmp_object);
	lock.Leave();
}

void CPHCommander::remove_calls(CPHReqComparerV* cmp_object)
{
	remove_calls(cmp_object, m_calls);
}

void CPHCommander::add_call_unique_as(CPHCondition* condition, CPHReqComparerV* cmp_condition, CPHAction* action,
                                      CPHReqComparerV* cmp_action)
{
	add_call_unique(condition, cmp_condition, action, cmp_action, m_calls_as_add_buffer);
}

void CPHCommander::add_call_as(CPHCondition* condition, CPHAction* action)
{
	add_call(condition, action, m_calls_as_add_buffer);
}


PHCALL_I CPHCommander::find_call_as(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
	return find_call(cmp_condition, cmp_action, m_calls);
}

void CPHCommander::remove_call_as(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
	remove_call(cmp_condition, cmp_action, m_calls_as_add_buffer);
}

void CPHCommander::remove_calls_as(CPHReqComparerV* cmp_object)
{
}

void CPHCommander::update_as()
{
}


void CPHCommander::phys_shell_relcase(CPhysicsShell* sh)
{
	CPHReqComparerHasShell c(sh);
	remove_calls_threadsafety(&c);
}
