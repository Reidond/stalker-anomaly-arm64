#include "stdafx.h"
#include "profiler.h"

#ifdef PROFILE_CRITICAL_SECTIONS
static add_profile_portion_callback add_profile_portion = 0;
void set_add_profile_portion(add_profile_portion_callback callback)
{
    add_profile_portion = callback;
}

struct profiler
{
    u64 m_time;
    LPCSTR m_timer_id;

    IC profiler::profiler(LPCSTR timer_id)
    {
        if (!add_profile_portion)
            return;

        m_timer_id = timer_id;
        m_time = CPU::QPC();
    }

    IC profiler::~profiler()
    {
        if (!add_profile_portion)
            return;

        u64 time = CPU::QPC();
        (*add_profile_portion)(m_timer_id, time - m_time);
    }
};
#endif // PROFILE_CRITICAL_SECTIONS

#ifdef PROFILE_CRITICAL_SECTIONS
xrCriticalSection::xrCriticalSection(LPCSTR id) : m_id(id)
#else // PROFILE_CRITICAL_SECTIONS
xrCriticalSection::xrCriticalSection()
#endif // PROFILE_CRITICAL_SECTIONS
{
#ifdef XR_PLATFORM_APPLE
	pmutex = new std::recursive_mutex(); // CRITICAL_SECTION is recursive
#else
	pmutex = xr_alloc<CRITICAL_SECTION>(1);
	InitializeCriticalSection((CRITICAL_SECTION*)pmutex);
#endif
}

xrCriticalSection::~xrCriticalSection()
{
#ifdef XR_PLATFORM_APPLE
	delete static_cast<std::recursive_mutex*>(pmutex);
#else
	DeleteCriticalSection((CRITICAL_SECTION*)pmutex);
	xr_free(pmutex);
#endif
}

#ifdef DEBUG
extern void OutputDebugStackTrace(const char* header);
#endif // DEBUG

void xrCriticalSection::Enter()
{
#ifdef PROFILE_CRITICAL_SECTIONS
# if 0//def DEBUG
    static bool show_call_stack = false;
    if (show_call_stack)
        OutputDebugStackTrace("----------------------------------------------------");
# endif // DEBUG
    profiler temp(m_id);
#endif // PROFILE_CRITICAL_SECTIONS
#ifdef XR_PLATFORM_APPLE
	static_cast<std::recursive_mutex*>(pmutex)->lock();
#else
	EnterCriticalSection((CRITICAL_SECTION*)pmutex);
#endif
}

void xrCriticalSection::Leave()
{
#ifdef XR_PLATFORM_APPLE
	static_cast<std::recursive_mutex*>(pmutex)->unlock();
#else
	LeaveCriticalSection((CRITICAL_SECTION*)pmutex);
#endif
}

BOOL xrCriticalSection::TryEnter()
{
#ifdef XR_PLATFORM_APPLE
	return static_cast<std::recursive_mutex*>(pmutex)->try_lock() ? TRUE : FALSE;
#else
	return TryEnterCriticalSection((CRITICAL_SECTION*)pmutex);
#endif
}

xrCriticalSection::raii::raii(xrCriticalSection* critical_section)
	: critical_section(critical_section)
{
	VERIFY(critical_section);
	critical_section->Enter();
}

xrCriticalSection::raii::~raii()
{
	critical_section->Leave();
}

#ifdef XR_PLATFORM_APPLE
xrSRWLock::xrSRWLock() {}
void xrSRWLock::AcquireExclusive() { smutex.lock(); }
void xrSRWLock::ReleaseExclusive() { smutex.unlock(); }
void xrSRWLock::AcquireShared() { smutex.lock_shared(); }
void xrSRWLock::ReleaseShared() { smutex.unlock_shared(); }
BOOL xrSRWLock::TryAcquireExclusive() { return smutex.try_lock() ? TRUE : FALSE; }
BOOL xrSRWLock::TryAcquireShared() { return smutex.try_lock_shared() ? TRUE : FALSE; }
#else
xrSRWLock::xrSRWLock()
{
    InitializeSRWLock(&smutex);
}

void xrSRWLock::AcquireExclusive()
{
	PROF_EVENT("xrSRWLock::AcquireExclusive");
    AcquireSRWLockExclusive(&smutex);
}

void xrSRWLock::ReleaseExclusive()
{
	PROF_EVENT("xrSRWLock::ReleaseExclusive");
    ReleaseSRWLockExclusive(&smutex);
}

void xrSRWLock::AcquireShared()
{
	PROF_EVENT("xrSRWLock::AcquireShared");
    AcquireSRWLockShared(&smutex);
}

void xrSRWLock::ReleaseShared()
{
	PROF_EVENT("xrSRWLock::ReleaseShared");
    ReleaseSRWLockShared(&smutex);
}

BOOL xrSRWLock::TryAcquireExclusive()
{
    return TryAcquireSRWLockExclusive(&smutex);
}

BOOL xrSRWLock::TryAcquireShared()
{
    return TryAcquireSRWLockShared(&smutex);
}
#endif // XR_PLATFORM_APPLE


xrSRWLockGuard::xrSRWLockGuard(xrSRWLock* lock, bool shared)
    : lock(lock), shared(shared)
{
    if (shared)
        lock->AcquireShared();
    else
        lock->AcquireExclusive();
}

xrSRWLockGuard::xrSRWLockGuard(xrSRWLock& lock, bool shared)
    : lock(&lock), shared(shared)
{
    if (shared)
        lock.AcquireShared();
    else
        lock.AcquireExclusive();
}

xrSRWLockGuard::~xrSRWLockGuard()
{
    if (shared)
        lock->ReleaseShared();
    else
        lock->ReleaseExclusive();
}
