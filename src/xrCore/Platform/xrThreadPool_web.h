// A small fixed thread pool for the WebAssembly build: the replacement for Grand Central Dispatch (dispatch_group_async_f,
// dispatch_apply_f) behind xr_task_group, xr_parallel_for and xr_parallel_foreach in _thread_types.h. Spike S1b,
// docs/web-port/WASM-BUILD.md.
//
// Why a pool of our own: Emscripten pthreads are Web Workers. Creating one is asynchronous (the browser's main thread cannot
// wait for it) and costs milliseconds, so the engine's threads are created once, up front, from the worker pool the module is
// linked with (-sPTHREAD_POOL_SIZE); this pool takes XR_WEB_TASK_THREADS of those and never creates another.
//
// Semantics kept from GCD: a task may run on any pool thread, in any order; the thread that waits (xr_task_group::wait,
// the end of xr_parallel_for) takes part in the work, so a wait inside a pool task cannot starve the pool.
#pragma once

#ifdef XR_PLATFORM_WEB

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#ifndef XR_WEB_TASK_THREADS
#	define XR_WEB_TASK_THREADS 4
#endif

namespace xr_web
{
// Queues a task; the pool starts its threads on the first call.
void pool_submit(std::function<void()> task);
// Runs one queued task on the calling thread. False when the queue is empty.
bool pool_try_run_one();
// fn(ctx, i) for every i in [0, count), spread over the pool and the calling thread; returns when all have finished.
void parallel_apply(size_t count, void* ctx, void (*fn)(void*, size_t));
// Number of pool threads (0 until the first use).
unsigned pool_thread_count();
} // namespace xr_web

// Subset of concurrency::task_group, as in the Apple build: run() queues a callable, wait() blocks until everything queued
// so far has finished (running queued work meanwhile).
class xr_task_group
{
	struct state_t
	{
		std::atomic<size_t> pending{0};
		std::mutex lock;
		std::condition_variable idle;
	};
	std::shared_ptr<state_t> state;

public:
	xr_task_group() : state(std::make_shared<state_t>()) {}
	~xr_task_group() { wait(); }
	xr_task_group(const xr_task_group&) = delete;
	xr_task_group& operator=(const xr_task_group&) = delete;

	template <typename F>
	void run(F&& functor)
	{
		state->pending.fetch_add(1, std::memory_order_acq_rel);
		std::shared_ptr<state_t> s = state;
		xr_web::pool_submit(
			[s, fn = std::function<void()>(std::forward<F>(functor))]()
			{
				fn();
				if (s->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
				{
					std::lock_guard<std::mutex> guard(s->lock);
					s->idle.notify_all();
				}
			});
	}
	void wait()
	{
		while (state->pending.load(std::memory_order_acquire) != 0)
		{
			if (xr_web::pool_try_run_one()) continue;
			std::unique_lock<std::mutex> guard(state->lock);
			state->idle.wait_for(guard, std::chrono::milliseconds(1),
			                     [this] { return state->pending.load(std::memory_order_acquire) == 0; });
		}
	}
	template <typename F>
	void run_and_wait(F&& functor)
	{
		run(std::forward<F>(functor));
		wait();
	}
};

#endif // XR_PLATFORM_WEB
