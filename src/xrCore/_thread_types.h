#pragma once

#ifdef XR_PLATFORM_APPLE
// Apple: Microsoft's PPL is not available. Grand Central Dispatch provides the thread pool.
#include <atomic>
#include <mutex>
#include <memory>
#include <deque>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <dispatch/dispatch.h>

// Atomic types
using xr_atomic_u32 = std::atomic_uint32_t;
using xr_atomic_s32 = std::atomic_int;
using xr_atomic_bool = std::atomic_bool;

// Subset of concurrency::task_group: run() queues a callable on the global concurrent queue, wait() blocks until
// everything queued so far has finished.
class xr_task_group
{
	dispatch_group_t group;

	static void trampoline(void* ctx)
	{
		std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(ctx));
		(*fn)();
	}

public:
	xr_task_group() : group(dispatch_group_create()) {}
	~xr_task_group()
	{
		wait();
		dispatch_release(group);
	}
	xr_task_group(const xr_task_group&) = delete;
	xr_task_group& operator=(const xr_task_group&) = delete;

	template <typename F>
	void run(F&& functor)
	{
		auto* fn = new std::function<void()>(std::forward<F>(functor));
		dispatch_group_async_f(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), fn, &trampoline);
	}
	void wait() { dispatch_group_wait(group, DISPATCH_TIME_FOREVER); }
	template <typename F>
	void run_and_wait(F&& functor)
	{
		run(std::forward<F>(functor));
		wait();
	}
};

// Mutex-protected stand-ins for concurrency::concurrent_unordered_map / concurrent_vector. Nothing in the engine
// uses them yet; they cover insert, lookup by value, push_back and indexed read.
template <typename T, typename U>
class xr_concurrent_unordered_map
{
	mutable std::mutex lock;
	std::unordered_map<T, U> map;

public:
	bool insert(const T& key, const U& value)
	{
		std::lock_guard<std::mutex> g(lock);
		return map.emplace(key, value).second;
	}
	bool try_get(const T& key, U& out) const
	{
		std::lock_guard<std::mutex> g(lock);
		auto it = map.find(key);
		if (it == map.end()) return false;
		out = it->second;
		return true;
	}
	size_t size() const
	{
		std::lock_guard<std::mutex> g(lock);
		return map.size();
	}
	void clear()
	{
		std::lock_guard<std::mutex> g(lock);
		map.clear();
	}
};

template <typename T, typename allocator = xalloc<T>>
class xr_concurrent_vector // 'allocator' is accepted for source compatibility and ignored
{
	mutable std::mutex lock;
	std::deque<T> items; // deque: push_back never moves existing elements, like concurrent_vector

public:
	size_t push_back(const T& value)
	{
		std::lock_guard<std::mutex> g(lock);
		items.push_back(value);
		return items.size() - 1;
	}
	T operator[](size_t index) const
	{
		std::lock_guard<std::mutex> g(lock);
		return items[index];
	}
	size_t size() const
	{
		std::lock_guard<std::mutex> g(lock);
		return items.size();
	}
	void clear()
	{
		std::lock_guard<std::mutex> g(lock);
		items.clear();
	}
};

template <typename BlockRangeType, typename Body>
inline void xr_parallel_for(BlockRangeType Begin, BlockRangeType End, Body Functor)
{
	if (End <= Begin) return;
	struct ctx_t
	{
		BlockRangeType begin;
		Body* functor;
	} ctx = {Begin, &Functor};
	dispatch_apply_f(size_t(End - Begin), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &ctx,
	                 [](void* c, size_t i)
	                 {
		                 ctx_t* x = static_cast<ctx_t*>(c);
		                 (*x->functor)(BlockRangeType(x->begin + BlockRangeType(i)));
	                 });
}

template <typename Index, typename Body>
inline void xr_parallel_foreach(Index Begin, Index End, Body Functor)
{
	// forward iterators: PPL's parallel_for_each over a random access range
	const auto count = size_t(std::distance(Begin, End));
	if (!count) return;
	struct ctx_t
	{
		Index begin;
		Body* functor;
	} ctx = {Begin, &Functor};
	dispatch_apply_f(count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &ctx,
	                 [](void* c, size_t i)
	                 {
		                 ctx_t* x = static_cast<ctx_t*>(c);
		                 (*x->functor)(*(x->begin + i));
	                 });
}

template <typename Data, typename Body>
inline void xr_parallel_sort(Data& data, Body functor)
{
	std::sort(std::begin(data), std::end(data), functor); // PPL falls back to std::sort for small inputs as well
}

#else // XR_PLATFORM_APPLE

#include <ppl.h>
#include <concurrent_unordered_map.h>
#include <concurrent_vector.h>
#include <atomic>

// Atomic types
using xr_atomic_u32 = std::atomic_uint32_t;
using xr_atomic_s32 = std::atomic_int;
using xr_atomic_bool = std::atomic_bool;

// Tasks Redefinition
using xr_task_group = concurrency::task_group;

template <typename T, typename U>
using xr_concurrent_unordered_map = concurrency::concurrent_unordered_map<T, U>;

template <typename T, typename allocator = xalloc<T>>
using xr_concurrent_vector = concurrency::concurrent_vector<T, allocator>;

template<typename BlockRangeType, typename Body>
inline void xr_parallel_for(BlockRangeType Begin, BlockRangeType End, Body Functor)
{
	concurrency::parallel_for(Begin, End, Functor);
}

template<typename Index, typename Body>
inline void xr_parallel_foreach(Index Begin, Index End, Body Functor)
{
	concurrency::parallel_for_each(Begin, End, Functor);
}

// PPL behaviour - fallback to std::sort if chunk size < 2048 and cores < 2
template<typename Data, typename Body>
inline void xr_parallel_sort(Data& data, Body functor)
{
	concurrency::parallel_sort(std::begin(data), std::end(data), functor);
}

#endif // XR_PLATFORM_APPLE

template<typename Data, typename Body>
inline void xr_sort(Data& data, Body functor)
{
	std::sort(std::begin(data), std::end(data), functor);
}

template<typename Data, typename Body>
inline void xr_stable_sort(Data& data, Body functor)
{
	std::stable_sort(std::begin(data), std::end(data), functor);
}
