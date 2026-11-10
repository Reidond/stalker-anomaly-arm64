// The fixed thread pool behind xr_task_group / xr_parallel_for on the web build. See xrThreadPool_web.h.
#include "../stdafx.h"
#pragma hdrstop

#ifdef XR_PLATFORM_WEB

#include "xrThreadPool_web.h"

#include <deque>
#include <thread>

namespace
{
struct pool_t
{
	std::mutex lock;
	std::condition_variable wake;
	std::deque<std::function<void()>> queue;
	unsigned threads = 0;
	bool started = false;
};

// Never destroyed: the workers are detached and may still be inside wait() at exit; the OS (Node, the page) ends them.
pool_t& pool()
{
	static pool_t* p = new pool_t;
	return *p;
}

void worker_main()
{
	pool_t& p = pool();
	for (;;)
	{
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> guard(p.lock);
			p.wake.wait(guard, [&p] { return !p.queue.empty(); });
			task = std::move(p.queue.front());
			p.queue.pop_front();
		}
		task();
	}
}

// Called with pool().lock held.
void start_threads_locked(pool_t& p)
{
	if (p.started) return;
	p.started = true;
	// pthread attributes: the engine puts string4096 buffers on the stack, so no pool thread gets less than 2 MB
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 2u << 20);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for (unsigned i = 0; i < XR_WEB_TASK_THREADS; ++i)
	{
		pthread_t thread;
		if (pthread_create(&thread, &attr,
		                   [](void*) -> void*
		                   {
			                   worker_main();
			                   return nullptr;
		                   },
		                   nullptr) == 0)
			++p.threads;
	}
	pthread_attr_destroy(&attr);
}

// One xr_parallel_for call. Shared with the helper tasks, so it outlives the caller's stack frame when a helper starts late;
// a late helper claims an index >= count and never touches ctx or fn.
struct apply_t
{
	size_t count;
	void* ctx;
	void (*fn)(void*, size_t);
	std::atomic<size_t> next{0};
	std::atomic<size_t> done{0};
	std::mutex lock;
	std::condition_variable finished;
};

void apply_run(const std::shared_ptr<apply_t>& a)
{
	for (;;)
	{
		const size_t i = a->next.fetch_add(1, std::memory_order_acq_rel);
		if (i >= a->count) return;
		a->fn(a->ctx, i);
		if (a->done.fetch_add(1, std::memory_order_acq_rel) + 1 == a->count)
		{
			std::lock_guard<std::mutex> guard(a->lock);
			a->finished.notify_all();
		}
	}
}
} // namespace

namespace xr_web
{
void pool_submit(std::function<void()> task)
{
	pool_t& p = pool();
	{
		std::lock_guard<std::mutex> guard(p.lock);
		start_threads_locked(p);
		p.queue.push_back(std::move(task));
	}
	p.wake.notify_one();
}

bool pool_try_run_one()
{
	pool_t& p = pool();
	std::function<void()> task;
	{
		std::lock_guard<std::mutex> guard(p.lock);
		if (p.queue.empty()) return false;
		task = std::move(p.queue.front());
		p.queue.pop_front();
	}
	task();
	return true;
}

unsigned pool_thread_count()
{
	pool_t& p = pool();
	std::lock_guard<std::mutex> guard(p.lock);
	return p.threads;
}

void parallel_apply(size_t count, void* ctx, void (*fn)(void*, size_t))
{
	if (!count) return;
	if (count == 1)
	{
		fn(ctx, 0);
		return;
	}
	auto a = std::make_shared<apply_t>();
	a->count = count;
	a->ctx = ctx;
	a->fn = fn;
	const size_t helpers = count - 1 < XR_WEB_TASK_THREADS ? count - 1 : XR_WEB_TASK_THREADS;
	for (size_t i = 0; i < helpers; ++i)
		pool_submit([a] { apply_run(a); });
	apply_run(a); // the caller works too
	std::unique_lock<std::mutex> guard(a->lock);
	a->finished.wait(guard, [&a] { return a->done.load(std::memory_order_acquire) == a->count; });
}
} // namespace xr_web

#endif // XR_PLATFORM_WEB
