/*
The Iristorm Concurrency Framework

This software is a C++ 20 Header-Only reimplementation of core part from project PaintsNow.

The MIT License (MIT)

Copyright (c) 2014-2026 PaintDream

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once

#include "iris_dispatcher.h"

// C++20 coroutine support
#include <coroutine>
#include <optional>
#include <vector>

// Coroutine fiber profiling is profiler-agnostic: iris only invokes the
// IRIS_PROFILE_COROUTINE_* hooks (defined in iris_common.h), which the host
// engine overrides with its profiler's fiber machinery. There is no profiler
// include here, so upstream iris builds standalone without a profiler
// dependency.

namespace iris {
	// standard coroutine interface settings
	namespace impl {
		template <typename return_t, template <typename...> class function_t>
		struct promise_type_base {
			constexpr std::suspend_always initial_suspend() noexcept { return std::suspend_always(); }
			constexpr std::suspend_never final_suspend() noexcept { return std::suspend_never(); }

			// call completion handle when returning a value
			// notice that value must be a rvalue and this completion is happened before destruction of living local variables in coroutine body
			void return_value(return_t&& value) noexcept {
				if (completion) {
					completion(std::coroutine_handle<decltype(*this)>::from_promise(*this).address(), std::move(value));
				}
			}

			// currently we do not handle unexcepted exceptions
			void unhandled_exception() noexcept { return std::terminate(); }
			function_t<void(void*, return_t&&)> completion;
		};

		template <template <typename...> class function_t>
		struct promise_type_base<void, function_t> {
			constexpr std::suspend_always initial_suspend() noexcept { return std::suspend_always(); }
			constexpr std::suspend_never final_suspend() noexcept { return std::suspend_never(); }

			void return_void() noexcept {
				if (completion) {
					completion(std::coroutine_handle<decltype(*this)>::from_promise(*this).address());
				}
			}

			void unhandled_exception() noexcept { return std::terminate(); }
			function_t<void(void*)> completion;
		};
	}

	// Coroutine fiber profiling.
	//
	// This engine's coroutines resume on arbitrary thread-pool/warp threads, so
	// a plain per-thread profiling zone inside a coroutine body would begin on
	// one OS thread and end on another - a per-thread profiler would then pop
	// the wrong stack and fail the capture. Fiber-based profiling (e.g. Tracy
	// fibers) gives every NAMED coroutine its own timeline lane + zone stack,
	// keyed by the fiber name regardless of which OS thread executes each
	// resume.
	//
	// iris is profiler-agnostic: it only invokes the IRIS_PROFILE_COROUTINE_*
	// hooks (see iris_common.h) around every resume/suspension point and keeps
	// the fiber name on the promise/awaitables (named()). The concrete profiler
	// machinery (per-thread fiber stack, current(), an enter/leave RAII guard)
	// lives in the host engine, which overrides those hooks - e.g. PaintsNow
	// maps them onto Tracy fibers. Call sites look like:
	//
	//     IRIS_PROFILE_COROUTINE_RESUME(handle, coro_identifier);
	//     handle.resume();
	//
	// where the hook brackets the resume within the enclosing scope.

	// uniform coroutine class with a return type specified
	//
	// Lifetime contract for completion / co_await chaining:
	//   final_suspend() returns std::suspend_never, so the coroutine frame is
	//   destroyed automatically right after return_value()/return_void()
	//   finishes.  return_value() invokes `completion` *before* the frame is
	//   destroyed and *with* its return value bound to a parameter that
	//   outlives the call.  Consequences:
	//     * Inside `completion`, you may safely read the return value and
	//       invoke other code synchronously. User complete()/then() callbacks
	//       still run before the child's local variables are destroyed.
	//     * co_await moves the result into the parent awaiter. If the child
	//       completes during await_suspend(), the parent continues without
	//       suspension after run() returns, rather than recursively resuming
	//       from completion. Otherwise completion resumes the suspended
	//       parent inline in the existing scheduler context. No extra queue
	//       or warp acquisition is introduced by coroutine chaining.
	//     * You MUST NOT capture a reference to anything on the coroutine
	//       frame (locals, the return value, etc.) and stash it for use after
	//       `completion` returns.  By the time control unwinds out of
	//       return_value() the frame is gone.  If you need the value after
	//       that point, make a copy / move into your own storage.
	//     * Throwing out of a coroutine body calls std::terminate() (see
	//       unhandled_exception below); the framework intentionally does not
	//       propagate exceptions across coroutine boundaries.
	template <typename return_t = void, template <typename...> class function_t = std::function>
	struct iris_coroutine_t {
		using return_type_t = return_t;

		struct promise_type : impl::promise_type_base<return_t, function_t> {
			// Coroutine fiber name (see named()); nullptr = no fiber tracking.
			const char* coro_identifier = nullptr;

			iris_coroutine_t get_return_object() noexcept {
				return iris_coroutine_t(std::coroutine_handle<promise_type>::from_promise(*this));
			}
		};

		explicit iris_coroutine_t(std::coroutine_handle<promise_type>&& h) : handle(std::move(h)) {}
		iris_coroutine_t(const iris_coroutine_t& rhs) = delete;
		iris_coroutine_t(iris_coroutine_t&& rhs) noexcept : handle(std::move(rhs.handle)) {
			rhs.handle = std::coroutine_handle<promise_type>();
		}

		iris_coroutine_t& operator = (const iris_coroutine_t& rhs) = delete;
		iris_coroutine_t& operator = (iris_coroutine_t&& rhs) noexcept {
			if (this != &rhs) {
				handle = std::exchange(rhs.handle, std::coroutine_handle<promise_type>());
			}

			return *this;
		}

		~iris_coroutine_t() noexcept {
			IRIS_ASSERT(!handle); // must call run() before destruction
		}

		// set a unique, optional completion handler
		template <typename func_t>
		iris_coroutine_t& complete(func_t&& func) noexcept {
			IRIS_ASSERT(handle);
			IRIS_ASSERT(!handle.promise().completion);
			handle.promise().completion = std::forward<func_t>(func);

			return *this;
		}

		// Safer completion adapter.
		//
		// Unlike complete(), this wrapper first moves the coroutine result into
		// an owned call frame, then invokes the user callback without exposing
		// the coroutine frame address.  That narrows the lifetime hazard around
		// final_suspend() == suspend_never: user code no longer receives a
		// reference directly tied to the coroutine frame.
		template <typename func_t, typename type_t = return_t>
		typename std::enable_if<std::is_void_v<type_t>, iris_coroutine_t&>::type then(func_t&& func) noexcept(noexcept(std::declval<iris_coroutine_t&>().complete(std::declval<function_t<void()>>()))) {
			function_t<void()> callback(std::forward<func_t>(func));
			return complete([callback = std::move(callback)](void*) mutable noexcept(noexcept(callback())) {
				callback();
			});
		}

		template <typename func_t, typename type_t = return_t>
		typename std::enable_if<!std::is_void_v<type_t>, iris_coroutine_t&>::type then(func_t&& func) noexcept(noexcept(std::declval<iris_coroutine_t&>().complete(std::declval<function_t<void(return_t)>>()))) {
			function_t<void(return_t)> callback(std::forward<func_t>(func));
			return complete([callback = std::move(callback)](void*, return_t&& value) mutable noexcept(noexcept(callback(std::declval<return_t>()))) {
				callback(return_t(std::move(value)));
			});
		}

		// Fiber name for this coroutine (must be a stable string literal,
		// e.g. "Class::Method"). A named coroutine is entered/left as a profiler
		// fiber around every resume (see IRIS_PROFILE_COROUTINE), so zones inside
		// its body are attributed to its own fiber lane and may safely span
		// co_await. Unnamed coroutines emit no fiber events.
		iris_coroutine_t& named(const char* name) noexcept {
			IRIS_ASSERT(handle);
			handle.promise().coro_identifier = name;
			return *this;
		}

		// run coroutine intermediately
		void run() noexcept(noexcept(std::declval<std::coroutine_handle<promise_type>>().resume())) {
			IRIS_ASSERT(handle);
			std::coroutine_handle<promise_type> execute_handle(std::move(handle));
			handle = std::coroutine_handle<promise_type>();
			IRIS_PROFILE_COROUTINE_RESUME(execute_handle, execute_handle.promise().coro_identifier);
			execute_handle.resume();
		}

		// iris_coroutine_t is also awaitable, so here we implement await_* series methods
		constexpr bool await_ready() const noexcept {
			return false;
		}

		// chain execution
		bool await_suspend(std::coroutine_handle<> parent_handle) {
			// Fiber of the PARENT coroutine: the completion callback below
			// resumes the parent from inside THIS coroutine's completion (a
			// nested resume), so it must switch to the parent's fiber lane for
			// the duration of that resume and switch back afterwards.
			const char* parentFiber = IRIS_PROFILE_COROUTINE_CURRENT();
			if constexpr (!std::is_void_v<return_t>) {
				complete([this, parent_handle = std::move(parent_handle), parentFiber](void*, return_t&& value) mutable noexcept(noexcept(std::declval<std::coroutine_handle<>>().resume())) {
					await_result.emplace(std::move(value));
					if (await_handshake.exchange(true, std::memory_order_acq_rel)) {
						// Resuming may destroy this awaiter; do not access it afterwards.
						std::coroutine_handle<> handle = parent_handle;
						const char* fiber = parentFiber;
						IRIS_PROFILE_COROUTINE_RESUME(handle, fiber);
						handle.resume();
					}
				});
			} else {
				complete([this, parent_handle = std::move(parent_handle), parentFiber](void*) mutable noexcept(noexcept(std::declval<std::coroutine_handle<>>().resume())) {
					if (await_handshake.exchange(true, std::memory_order_acq_rel)) {
						std::coroutine_handle<> handle = parent_handle;
						const char* fiber = parentFiber;
						IRIS_PROFILE_COROUTINE_RESUME(handle, fiber);
						handle.resume();
					}
				});
			}

			run();
			// The first side publishes completion/suspension; the second proceeds.
			// After publication completion may destroy this awaiter on another thread.
			return !await_handshake.exchange(true, std::memory_order_acq_rel);
		}

		// carry out return value
		return_t await_resume() noexcept {
			if constexpr (!std::is_void_v<return_t>) {
				IRIS_ASSERT(await_result.has_value());
				return std::move(*await_result);
			}
		}

		operator std::coroutine_handle<promise_type>() noexcept {
			return handle;
		}

		operator std::coroutine_handle<>() noexcept {
			return std::coroutine_handle<>(handle);
		}

		std::coroutine_handle<promise_type>& get_handle() noexcept {
			return handle;
		}

		std::coroutine_handle<promise_type> move_handle() noexcept {
			return std::exchange(handle, std::coroutine_handle<promise_type>());
		}

		const std::coroutine_handle<promise_type>& get_handle() const noexcept {
			return handle;
		}

	protected:
		struct void_t {};
		std::coroutine_handle<promise_type> handle;
		std::conditional_t<std::is_void_v<return_t>, void_t, std::optional<return_t>> await_result;
		std::atomic<bool> await_handshake{ false };
	};
	// awaitable object, can be used by:
	// co_await iris_awaitable_t(...);
	template <typename warp_type_t, typename func_type_t>
	struct iris_awaitable_t {
		using warp_t = warp_type_t;
		using func_t = func_type_t;
		using return_t = std::invoke_result_t<func_t>;

		static constexpr size_t status_mask_dispatched = 1u;
		static constexpr size_t status_mask_waited = 1u << 1u;
		static constexpr size_t status_mask_completed = 1u << 2u;

		// constructed from a given target warp and routine function
		// notice that we do not initialize `caller` here, let `await_suspend` do
		// parallel_priority: ~size_t(0) means no parallization, other value indicates the dispatch priority of parallel routines
		template <typename callable_t>
		iris_awaitable_t(warp_t* target_warp, callable_t&& f, size_t p) noexcept : caller(nullptr), target(target_warp), parallel_priority(p), func(std::forward<callable_t>(f)), resume_handle(std::coroutine_handle()) {
			IRIS_ASSERT(target_warp != nullptr || parallel_priority == ~size_t(0));
			if constexpr (!std::is_void_v<return_t>) {
				ret = return_t();
			}

			status.store(0u, std::memory_order_relaxed);
		}

		~iris_awaitable_t() noexcept {
			IRIS_ASSERT(await_ready() || status.load(std::memory_order_acquire) == 0u);
			IRIS_ASSERT(resume_handle == std::coroutine_handle());
		}

		// always suspended
		bool await_ready() const noexcept {
			return !!(status.load(std::memory_order_acquire) & status_mask_completed);
		}

		// Public so that callers can pre-dispatch an awaitable and overlap its
		// work with other code before issuing the final `co_await`.
		//
		// Lifetime/threading contract:
		//   * The `caller` warp is captured *here*, not at `await_suspend()`.
		//     If you explicitly `dispatch()` an awaitable on one warp and then
		//     `co_await` it on another, resumption is routed back to the warp
		//     that ran dispatch().  Avoid splitting dispatch and co_await
		//     across different warps unless that behavior is intended.
		//   * Returns true if this call actually performed the dispatch, false
		//     if it had already been dispatched (e.g. by await_suspend).
		bool dispatch() noexcept {
			// already dispatched?
			if (status.fetch_or(status_mask_dispatched, std::memory_order_release) & status_mask_dispatched) {
				return false;
			}

			// Fiber of the coroutine that will resume on completion
			// (captured from the awaiting coroutine's context).
			coro_identifier = IRIS_PROFILE_COROUTINE_CURRENT();

			caller = warp_t::get_current();

			// the same warp, execute at once!
			// even they are both null
			if (target == caller) {
				if constexpr (std::is_void_v<return_t>) {
					func();
				} else {
					ret = func(); // auto moved here
				}

				if (status.fetch_or(status_mask_completed, std::memory_order_release) & status_mask_waited) {
					auto handle = std::exchange(resume_handle, std::coroutine_handle());
					IRIS_PROFILE_COROUTINE_RESUME(handle, coro_identifier);
					handle.resume();
				}
			} else {
				if (target == nullptr) {
					// targeting to thread pool with no warp context
					caller->get_async_worker().queue([this]() mutable {
						if constexpr (std::is_void_v<return_t>) {
							func();
						} else {
							ret = func();
						}

						if (status.fetch_or(status_mask_completed, std::memory_order_release) & status_mask_waited) {
							resume_one();
						}
					});
				} else {
					if (parallel_priority == ~size_t(0)) {
						// targeting to a valid warp
						// prepare callback first
						auto callback = [this]() mutable noexcept(noexcept(func()) && noexcept(std::declval<iris_awaitable_t>().resume_one())) {
							if constexpr (std::is_void_v<return_t>) {
								func();
							} else {
								ret = func();
							}

							if (status.fetch_or(status_mask_completed, std::memory_order_release) & status_mask_waited) {
								resume_one();
							}
						};

						target->queue_routine_post(std::move(callback));
					} else {
						// suspend the target warp
						target->suspend();

						// let async_worker running them, so multiple routines around the same target could be executed in parallel
						typename warp_t::suspend_guard_t guard(target);
						target->get_async_worker().queue([this]() mutable noexcept(noexcept(func()) && noexcept(std::declval<iris_awaitable_t>().resume_one()) && noexcept(target->resume())) {
							typename warp_t::suspend_guard_t guard(target);
							if constexpr (std::is_void_v<return_t>) {
								func();
							} else {
								ret = func();
							}

							guard.cleanup();
							target->resume();

							if (status.fetch_or(status_mask_completed, std::memory_order_release) & status_mask_waited) {
								resume_one();
							}
						}, parallel_priority);

						guard.cleanup();
					}
				}
			}

			return true;
		}

		bool await_suspend(std::coroutine_handle<> handle) {
			// Fiber of the awaiting coroutine: the eventual resume (in
			// resume_one, from a worker/warp task) re-enters this fiber.
			coro_identifier = IRIS_PROFILE_COROUTINE_CURRENT();
			resume_handle = std::move(handle);

			dispatch();
			// Dispatch can complete inline. Publish the waiter only afterwards so
			// that synchronous completion cannot recursively resume this coroutine.
			if (status.fetch_or(status_mask_waited, std::memory_order_acq_rel) & status_mask_completed) {
				resume_handle = std::coroutine_handle<>();
				return false;
			}

			return true;
		}

		return_t await_resume() noexcept {
			if constexpr (!std::is_void_v<return_t>) {
				return std::move(ret);
			}
		}

	protected:
		void resume_one() {
			IRIS_ASSERT(resume_handle != std::coroutine_handle<>());

			const char* fiber = coro_identifier;

			// return to caller's warp
			if (caller != nullptr) {
				// notice that the condition `caller != target` holds
				// so we can use `post` to skip self-queueing check
				caller->queue_routine_post([handle = std::exchange(resume_handle, std::coroutine_handle()), fiber]() mutable noexcept(noexcept(resume_handle.resume())) {
					// Locals (not the enclosing captures): MSVC cannot reference
					// an enclosing lambda's captures from a nested lambda.
					auto h = handle;
					const char* f = fiber;
					IRIS_PROFILE_COROUTINE_RESUME(h, f);
					h.resume();
				});
			} else {
				// otherwise dispatch to thread pool
				// notice that we mustn't call handle.resume() directly
				// since it may blocks execution of current warp
				target->get_async_worker().queue([handle = std::exchange(resume_handle, std::coroutine_handle()), fiber]() mutable noexcept(noexcept(resume_handle.resume())) {
					auto h = handle;
					const char* f = fiber;
					IRIS_PROFILE_COROUTINE_RESUME(h, f);
					h.resume();
				});
			}
		}

		struct void_t {};
		std::atomic<size_t> status;
		warp_t* caller;
		warp_t* target;
		size_t parallel_priority;
		func_t func;
		std::coroutine_handle<> resume_handle;
		const char* coro_identifier = nullptr;
		std::conditional_t<std::is_void_v<return_t>, void_t, return_t> ret;
	};

	// simple wrapper for constructing an awaitable object
	template <typename warp_t, typename iris_func_t>
	auto iris_awaitable(warp_t* target_warp, iris_func_t&& func) noexcept {
		return iris_awaitable_t<warp_t, std::decay_t<iris_func_t>>(target_warp, std::forward<iris_func_t>(func), ~size_t(0));
	}

	// simple wrapper for constructing an awaitable object in parallel
	template <typename warp_t, typename iris_func_t>
	auto iris_awaitable_parallel(warp_t* target_warp, iris_func_t&& func, size_t priority = 0) noexcept {
		IRIS_ASSERT(priority != ~size_t(0));
		return iris_awaitable_t<warp_t, std::decay_t<iris_func_t>>(target_warp, std::forward<iris_func_t>(func), priority);
	}

	// switch to specified warp or warp pair, and return the original current warp
	template <typename warp_t>
	struct iris_switch_t {
		using async_worker_t = typename warp_t::async_worker_t;
		iris_switch_t(warp_t* target_warp, warp_t* other_warp, bool parallel_target_warp, bool parallel_other_warp) noexcept : source(warp_t::get_current()), target(target_warp), other(other_warp), parallel_target(parallel_target_warp), parallel_other(parallel_other_warp) {}

		bool await_ready() const noexcept {
			if (parallel_target || parallel_other)
				return false;

			if (source == target) {
				return other == nullptr || source == other;
			} else {
				return target == nullptr && source == other;
			}
		}

		void handler(std::coroutine_handle<>&& handle) {
			const char* fiber = coro_identifier;

			// 1-1 mapping, just resume directly
			if (other == nullptr) {
				IRIS_PROFILE_COROUTINE_RESUME(handle, fiber);
				handle.resume();
				return;
			} else if (parallel_other) {
				other->suspend();
				typename warp_t::suspend_guard_t guard(other);
				if (!other->running()) {
					IRIS_PROFILE_COROUTINE_RESUME(handle, fiber);
					handle.resume();
					return;
				}
			} else {
				// try to preempt the other one
				typename warp_t::preempt_guard_t guard(*other, 0);
				if (guard) {
					// success, go resume directly
					IRIS_PROFILE_COROUTINE_RESUME(handle, fiber);
					handle.resume();
					return;
				}
			}

			// failed, swap and continue dispatching until success!
			std::swap(other, target);
			std::swap(parallel_other, parallel_target);

			if (parallel_target) {
				target->queue_routine_parallel_post([this, handle = std::move(handle)]() mutable noexcept(noexcept(handle.resume())) {
					handler(std::move(handle));
				});
			} else {
				target->queue_routine_post([this, handle = std::move(handle)]() mutable noexcept(noexcept(handle.resume())) {
					handler(std::move(handle));
				});
			}
		}

		void await_suspend(std::coroutine_handle<> handle) {
			// Fiber of the switching coroutine: handler() may resume it
			// from a worker/warp task later.
			coro_identifier = IRIS_PROFILE_COROUTINE_CURRENT();
			if (target == nullptr) {
				std::swap(other, target);
			}

			if (target == nullptr) {
				// full parallel dispatching
				IRIS_ASSERT(source != nullptr);
				source->get_async_worker().queue([this, handle = std::move(handle)]() mutable {
					handler(std::move(handle));
				});
			} else {
				// dispatching under warp context
				if (parallel_target && target->get_async_worker().get_current_thread_index() != ~size_t(0)) {
					target->queue_routine_parallel([this, handle = std::move(handle)]() mutable {
						handler(std::move(handle));
					});
				} else {
					target->queue_routine_post([this, handle = std::move(handle)]() mutable {
						handler(std::move(handle));
					});
				}
			}
		}

		// return the caller warp
		warp_t* await_resume() const noexcept {
			return source;
		}

	protected:
		warp_t* source;
		warp_t* target;
		warp_t* other;
		bool parallel_target;
		bool parallel_other;
		const char* coro_identifier = nullptr;
	};

	template <typename warp_t>
	auto iris_switch(warp_t* target, warp_t* other = nullptr, bool parallel_target = false, bool parallel_other = false) noexcept {
		return iris_switch_t<warp_t>(target, other, parallel_target, parallel_other);
	}

	// switch to any warp from specified range [from, to]
	//
	// Concurrency note:
	//   We post a copy of `shared_handle` to every candidate warp.  The first
	//   callback to win the `exchange()` race captures the handle and writes
	//   `selected`; every later callback observes a null handle and becomes a
	//   no-op.  The early `break` is purely an optimization -- correctness
	//   does not depend on it -- because losing callbacks never touch state.
	//
	//   The winning callback is the *only* writer to `selected`, so no
	//   atomic is needed: the `shared_handle->first.exchange(..., release)`
	//   that publishes the win synchronizes-with the resumed coroutine's
	//   later `await_resume()` read via the std::coroutine_handle resume
	//   barrier.
	template <typename iterator_t>
	struct iris_select_t {
		using warp_t = std::decay_t<decltype(*std::declval<iterator_t>())>;
		using async_worker_t = typename warp_t::async_worker_t;

		iris_select_t(iterator_t from, iterator_t to) noexcept : begin(from), end(to), selected(nullptr) {
			IRIS_ASSERT(from != to);
			IRIS_ASSERT(warp_t::get_current() == nullptr);
		}

		constexpr bool await_ready() const noexcept {
			return false;
		}

		void await_suspend(std::coroutine_handle<> handle) {
			// Fiber of the selecting coroutine: the winning warp task
			// resumes it later.
			coro_identifier = IRIS_PROFILE_COROUTINE_CURRENT();
			// all warps are busy, so we need to post tasks to them
			auto shared_handle = std::make_shared<std::pair<std::atomic<std::coroutine_handle<>>, iris_select_t*>>(std::move(handle), this);
			for (iterator_t p = begin; p != end; ++p) {
				warp_t* target = &(*p);

				target->queue_routine_post([shared_handle, target]() mutable noexcept(noexcept(handle.resume())) {
					// taken and go execute
					auto handle = shared_handle->first.exchange(std::coroutine_handle<>(), std::memory_order_release);
					if (handle) {
						shared_handle->second->selected = target;
						IRIS_PROFILE_COROUTINE_RESUME(handle, shared_handle->second->coro_identifier);
						handle.resume();
					}
				});

				// already dispatched to one warp sucessfully, just give up the remaining
				if (shared_handle->first.load(std::memory_order_acquire) == std::coroutine_handle<>())
					break;
			}
		}

		warp_t* await_resume() const noexcept {
			return selected;
		}

	protected:
		warp_t* selected;
		iterator_t begin;
		iterator_t end;
		const char* coro_identifier = nullptr;
	};

	template <typename iterator_t>
	auto iris_select(iterator_t begin, iterator_t end) noexcept {
		return iris_select_t<iterator_t>(begin, end);
	}

	// basic asynchornized base class
	template <typename warp_t, typename async_worker_t = typename warp_t::async_worker_t>
	struct iris_sync_t {
		async_worker_t& get_async_worker() noexcept {
			return async_worker;
		}

	protected:
		explicit iris_sync_t(async_worker_t& worker) : async_worker(worker) {}
		iris_sync_t(const iris_sync_t& rhs) = delete;
		iris_sync_t& operator = (const iris_sync_t& rhs) = delete;

		struct info_base_warp_t {
			std::coroutine_handle<> handle;
			warp_t* warp = nullptr;
			const char* coro_identifier = nullptr;
		};

		struct info_base_t {
			std::coroutine_handle<> handle;
			const char* coro_identifier = nullptr;
		};

		using info_t = std::conditional_t<std::is_same_v<warp_t, void>, info_base_t, info_base_warp_t>;

		// dispatch coroutine based on warp status
		void dispatch(info_t&& info) {
			const char* fiber = info.coro_identifier;
			if constexpr (std::is_same_v<warp_t, void>) {
				async_worker.queue([handle = std::move(info.handle), fiber]() mutable noexcept(noexcept(info.handle.resume())) {
					// Locals (not the enclosing captures): MSVC cannot reference
					// an enclosing lambda's captures from a nested lambda.
					auto h = handle;
					const char* f = fiber;
					IRIS_PROFILE_COROUTINE_RESUME(h, f);
					h.resume();
				});
			} else {
				warp_t* target = info.warp;
				if (target == nullptr) {
					async_worker.queue([handle = std::move(info.handle), fiber]() mutable noexcept(noexcept(info.handle.resume())) {
						auto h = handle;
						const char* f = fiber;
						IRIS_PROFILE_COROUTINE_RESUME(h, f);
						h.resume();
					});
				} else {
					target->queue_routine_post([handle = std::move(info.handle), fiber]() mutable noexcept(noexcept(info.handle.resume())) {
						auto h = handle;
						const char* f = fiber;
						IRIS_PROFILE_COROUTINE_RESUME(h, f);
						h.resume();
					});
				}
			}
		}

		async_worker_t& async_worker;
	};

	// event-like multiple coroutine sychronization
	template <typename warp_t, typename async_worker_t = typename warp_t::async_worker_t>
	struct iris_event_t : iris_sync_t<warp_t, async_worker_t> {
		iris_event_t(async_worker_t& worker) : iris_sync_t<warp_t, async_worker_t>(worker) {
			signaled.store(0, std::memory_order_release);
		}

		bool await_ready() const noexcept {
			return signaled.load(std::memory_order_acquire) != 0;
		}

		void await_suspend(std::coroutine_handle<> handle) {
			info_t info;
			info.handle = std::move(handle);

			if constexpr (!std::is_same_v<warp_t, void>) {
				info.warp = warp_t::get_current();
			}
			info.coro_identifier = IRIS_PROFILE_COROUTINE_CURRENT();

			// already signaled?
			if (signaled.load(std::memory_order_acquire) == 0) {
				std::lock_guard<std::mutex> guard(lock);
				// double check
				if (signaled.load(std::memory_order_acquire) == 0) {
					handles.emplace_back(std::move(info));
					return;
				}
			}

			iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
		}

		constexpr void await_resume() const noexcept {}

		void reset() noexcept {
			signaled.store(0, std::memory_order_release);
		}

		// notify all waiting coroutines
		void notify() {
			std::vector<info_t> set_handles;
			do {
				std::lock_guard<std::mutex> guard(lock);
				signaled.store(1, std::memory_order_relaxed);

				std::swap(set_handles, handles);
			} while (false);

			for (auto&& info : set_handles) {
				iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
			}
		}

	protected:
		using info_t = typename iris_sync_t<warp_t, async_worker_t>::info_t;
		std::atomic<size_t> signaled;
		std::mutex lock;
		std::vector<info_t> handles;
	};

	// pipe-like multiple coroutine synchronization (mpmc/spsc).
	//
	// The default `mutex_t = std::mutex` is correct for MPMC use.  You may
	// substitute `iris_no_mutex_t` *only* when external synchronization
	// guarantees that emplace() and the await side never observe a torn
	// view of the handshake (e.g., a strictly serialized SPSC pair where
	// emplace and co_await each happen on a dedicated warp).  In the
	// no-mutex mode the rendezvous re-check is elided, so a generic SPSC
	// use without the above guarantee may lose wakeups.
	template <typename element_t, typename warp_t, typename async_worker_t = typename warp_t::async_worker_t, typename mutex_t = std::mutex>
	struct iris_pipe_t : iris_sync_t<warp_t, async_worker_t> {
		iris_pipe_t(async_worker_t& worker) : iris_sync_t<warp_t, async_worker_t>(worker) {
			prepared_count.store(0, std::memory_order_relaxed);
			waiting_count.store(0, std::memory_order_release);
		}

		constexpr bool await_ready() const noexcept {
			return false;
		}

		void await_suspend(std::coroutine_handle<> handle) {
			info_t info;
			info.handle = std::move(handle);

			if constexpr (!std::is_same_v<warp_t, void>) {
				info.warp = warp_t::get_current();
			}
			info.coro_identifier = IRIS_PROFILE_COROUTINE_CURRENT();

			// fast path
			if (flush_prepared()) {
				iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
				return;
			}

			std::unique_lock<mutex_t> guard(handle_lock);
			if constexpr (!std::is_same_v<mutex_t, iris_no_mutex_t>) {
				// retry under lock to close the lost-wakeup window with emplace()
				if (flush_prepared()) {
					guard.unlock();
					iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
					return;
				}
			}

			// failed, push to deferred list
			handles.push(std::move(info));
			waiting_count.fetch_add(1, std::memory_order_release);
		}

		element_t await_resume() noexcept {
			std::lock_guard<mutex_t> guard(data_pop_lock);
			element_t element = std::move(elements.top());
			elements.pop();

			return element;
		}

		template <typename element_data_t>
		void emplace(element_data_t&& element) {
			do {
				std::lock_guard<mutex_t> guard(data_push_lock);
				elements.push(std::forward<element_data_t>(element));
			} while (false);

			// fast path
			if (flush_waiting()) {
				info_t info;
				do {
					std::lock_guard<mutex_t> guard(handle_lock);
					info = std::move(handles.top());
					handles.pop();
				} while (false);

				iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
				return;
			}

			std::unique_lock<mutex_t> guard(handle_lock);
			if constexpr (!std::is_same_v<mutex_t, iris_no_mutex_t>) {
				if (flush_waiting()) {
					info_t info = std::move(handles.top());
					handles.pop();
					guard.unlock();

					iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
					return;
				}
			}

			// still not success, go starving
			prepared_count.fetch_add(1, std::memory_order_release);
		}

	protected:
		// test and fetch producer
		bool flush_prepared() noexcept {
			size_t prepared = prepared_count.load(std::memory_order_acquire);
			while (prepared != 0) {
				if (prepared_count.compare_exchange_strong(prepared, prepared - 1, std::memory_order_release)) {
					return true;
				}
			}

			return false;
		}

		// test and fetch consumer
		bool flush_waiting() noexcept {
			size_t waiting = waiting_count.load(std::memory_order_acquire);
			while (waiting != 0) {
				if (waiting_count.compare_exchange_strong(waiting, waiting - 1, std::memory_order_release)) {
					return true;
				}
			}

			return false;
		}

	protected:
		using info_t = typename iris_sync_t<warp_t, async_worker_t>::info_t;
		std::atomic<size_t> prepared_count;
		std::atomic<size_t> waiting_count;
		mutex_t handle_lock;
		mutex_t data_push_lock;
		mutex_t data_pop_lock;

		iris_queue_list_t<info_t> handles;
		iris_queue_list_t<element_t> elements;
	};

	// barrier-like multiple coroutine sychronization
	template <typename warp_t, typename value_t = bool, typename async_worker_t = typename warp_t::async_worker_t>
	struct iris_barrier_t : iris_sync_t<warp_t, async_worker_t> {
		iris_barrier_t(async_worker_t& worker, size_t max_await_count_count, value_t init_value = value_t()) : iris_sync_t<warp_t, async_worker_t>(worker), max_await_count(max_await_count_count), value(init_value) {
			handles.resize(max_await_count.load(std::memory_order_relaxed));
			// pre-reserve the dispatch snapshot so complete() performs no allocation
			// on the steady-state path (see complete()'s lifetime contract)
			snapshot.reserve(handles.size());
			slot_count.store(0, std::memory_order_relaxed);
			commit_count.store(0, std::memory_order_relaxed);
			release_await_count.store(0, std::memory_order_release);
		}

		void reset(size_t max_await_count_count, value_t init_value = value_t()) {
			IRIS_ASSERT(slot_count.load(std::memory_order_acquire) == 0);
			IRIS_ASSERT(commit_count.load(std::memory_order_acquire) == 0);
			IRIS_ASSERT(release_await_count.load(std::memory_order_acquire) == 0);

			max_await_count.store(max_await_count_count, std::memory_order_release);
			value = init_value;
			handles.resize(max_await_count_count);
			snapshot.reserve(handles.size());
			slot_count.store(0, std::memory_order_relaxed);
			commit_count.store(0, std::memory_order_relaxed);
			release_await_count.store(0, std::memory_order_release);
		}

		~iris_barrier_t() noexcept {
#if IRIS_DEBUG
			// no more suspended coroutines
			for (size_t k = 0; k < handles.size(); k++) {
				IRIS_ASSERT(handles[k].handle == std::coroutine_handle<>());
			}
#endif
		}

		constexpr bool await_ready() const noexcept {
			return false;
		}

		template <typename func_t>
		void dispatch(func_t&& cb) {
			callback = std::forward<func_t>(cb);
			await_suspend(std::coroutine_handle<>());
		}

		// release() does not provide a handle; it only consumes one "expected"
		// participant slot.  We bump commit_count directly (no handles[] write
		// to publish), and trigger complete() if we are the last one.
		void release(size_t count = 1) noexcept {
			release_await_count.fetch_add(count, std::memory_order_relaxed);
			IRIS_ASSERT(max_await_count.load(std::memory_order_acquire) >= release_await_count.load(std::memory_order_relaxed));
			// commit_count uses acq_rel so that the thread eventually firing
			// complete() acquires all preceding handles[] writes from peers.
			size_t prev = commit_count.fetch_add(count, std::memory_order_acq_rel);
			size_t current_max = max_await_count.load(std::memory_order_acquire);
			if (prev + count == current_max) {
				complete();
			}
		}

		void await_suspend(std::coroutine_handle<> handle) {
			// Step 1: allocate an index slot.  slot_count is *only* used to
			// assign indices, never to decide when to fire complete().  This
			// way the index-allocating fetch_add can be relaxed.
			size_t index = slot_count.fetch_add(1, std::memory_order_relaxed);
			IRIS_ASSERT(index < max_await_count.load(std::memory_order_acquire));

			if (handle) {
				auto& info = handles[index];
				info.handle = std::move(handle);

				if constexpr (!std::is_same_v<warp_t, void>) {
					info.warp = warp_t::get_current();
				}
			}

			// Step 2: publish the write to handles[index] before triggering
			// complete().  The acq_rel fetch_add releases our handles[]
			// store and acquires every other waiter's store, so the thread
			// that observes commit_count == max_await_count is guaranteed
			// to see *all* handles[] entries populated.  max_await_count is
			// loaded atomically (see the member comment -- the trigger
			// comparison must not race complete()'s shrink).
			size_t prev = commit_count.fetch_add(1, std::memory_order_acq_rel);
			if (prev + 1 == max_await_count.load(std::memory_order_acquire)) {
				complete();
			}
		}

		template <typename type_t>
		void set_value(type_t&& new_value) {
			value = std::forward<type_t>(new_value);
		}

		const value_t& get_value() const noexcept {
			return value;
		}

		const value_t& await_resume() const noexcept {
			return get_value();
		}

		size_t get_max_await_count() const noexcept {
			return max_await_count.load(std::memory_order_acquire);
		}

		size_t get_await_count() const noexcept {
			return commit_count.load(std::memory_order_acquire);
		}

	protected:
		void complete() {
			size_t old = commit_count.exchange(0, std::memory_order_release);
			IRIS_ASSERT(old == max_await_count.load(std::memory_order_acquire));
			slot_count.store(0, std::memory_order_relaxed);

			size_t last_max_await_count = max_await_count.load(std::memory_order_relaxed);
			max_await_count.store(last_max_await_count - release_await_count.exchange(0, std::memory_order_relaxed), std::memory_order_release);

			// notify all coroutines
			if (callback) {
				callback(*this);
			}

			snapshot.clear();
			for (size_t i = 0; i < last_max_await_count; i++) {
				if (handles[i].handle != std::coroutine_handle<>()) {
					snapshot.push_back(handles[i]);
				}
				handles[i].handle = std::coroutine_handle<>();
				if constexpr (!std::is_same_v<warp_t, void>) {
					handles[i].warp = nullptr;
				}
			}

			for (auto&& info : snapshot) {
				iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
			}

			snapshot.clear();
		}

	protected:
		using info_t = typename iris_sync_t<warp_t, async_worker_t>::info_t;
		std::atomic<size_t> max_await_count;
		value_t value;
		// slot_count: monotonically allocates indices into handles[]; reset on complete().
		// commit_count: counts publications of handles[] (incl. release()-only); acq_rel
		//   ordered so that the thread firing complete() observes every peer's write.
		std::atomic<size_t> slot_count;
		std::atomic<size_t> commit_count;
		std::vector<info_t> handles;
		std::vector<info_t> snapshot;
		std::function<void(iris_barrier_t&)> callback;
		std::atomic<size_t> release_await_count;
	};

	// specify warp_t = void for warp-ignored dispatch
	template <typename warp_t, typename async_worker_t>
	auto iris_barrier(async_worker_t& worker, size_t max_await_count_count) {
		return iris_barrier_t<warp_t, async_worker_t>(worker, max_await_count_count);
	}

	// listen specified task completes
	template <typename async_dispatcher_t>
	struct iris_coroutine_dispatch_t : iris_sync_t<typename async_dispatcher_t::warp_t, typename async_dispatcher_t::async_worker_t> {
		using warp_t = typename async_dispatcher_t::warp_t;
		using async_worker_t = typename async_dispatcher_t::async_worker_t;
		using routine_handle_t = typename async_dispatcher_t::routine_handle_t;

		explicit iris_coroutine_dispatch_t(async_dispatcher_t& disp) : iris_sync_t<warp_t, async_worker_t>(disp.get_async_worker()), dispatcher(disp) {
			warp_t* warp = nullptr;
			if constexpr (!std::is_same_v<warp_t, void>) {
				warp = warp_t::get_current();
			}

			info.warp = warp;
			info.coro_identifier = IRIS_PROFILE_COROUTINE_CURRENT();
			routine = dispatcher.allocate(warp, [this](const async_dispatcher_t::routine_handle_t& routine) {
				iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
			});
		}

		~iris_coroutine_dispatch_t() noexcept {}

		template <typename... args_t>
		iris_coroutine_dispatch_t(async_dispatcher_t& disp, routine_handle_t&& first, args_t&&... args) : iris_coroutine_dispatch_t(disp, std::forward<args_t>(args)...) {
			dispatcher.order(first, routine);
			dispatcher.dispatch(std::move(first));
		}

		constexpr bool await_ready() const noexcept {
			return false;
		}

		void await_suspend(std::coroutine_handle<> handle) {
			IRIS_ASSERT(info.handle == std::coroutine_handle<>());
			info.handle = std::move(handle);

			if constexpr (!std::is_same_v<warp_t, void>) {
				IRIS_ASSERT(info.warp == warp_t::get_current());
			} else {
				IRIS_ASSERT(info.warp == nullptr);
			}

			dispatcher.dispatch(std::move(routine));
		}

		constexpr void await_resume() const noexcept {}

	protected:
		using info_t = typename iris_sync_t<warp_t, async_worker_t>::info_t;
		async_dispatcher_t& dispatcher;
		routine_handle_t routine;
		info_t info;
	};

	// coroutine -> dispatcher, to schedule a coroutine as a dispatcher task
	template <typename async_dispatcher_t, typename coroutine_t>
	auto iris_dispatch_coroutine(async_dispatcher_t& dispatcher, coroutine_t&& coroutine) {
		static_assert(std::is_rvalue_reference_v<coroutine_t&&>, "Coroutine must be rvalue.");
		using warp_t = typename async_dispatcher_t::warp_t;
		warp_t* warp = nullptr;
		if constexpr (!std::is_same_v<warp_t, void>) {
			warp = warp_t::get_current();
		}

		return dispatcher.allocate(warp, [&dispatcher, handle = coroutine.move_handle()](const typename async_dispatcher_t::routine_handle_t& post) mutable {
			coroutine_t coroutine(std::move(handle)); // reconstruct
			if constexpr (std::is_void_v<typename coroutine_t::return_type_t>) {
				coroutine.complete([&dispatcher, p = dispatcher.defer(post).move()](void* address) {
					dispatcher.dispatch(typename async_dispatcher_t::routine_handle_t(p));
				});
			} else {
				coroutine.complete([&dispatcher, p = dispatcher.defer(post).move()](void* address, typename coroutine_t::return_type_t&& value) {
					// return value is discarded
					dispatcher.dispatch(typename async_dispatcher_t::routine_handle_t(p));
				});
			}

			coroutine.run();
		});
	}

	// dispatcher -> coroutine, to co_await multiple dispatcher tasks
	template <typename async_dispatcher_t, typename... args_t>
	auto iris_coroutine_dispatch(async_dispatcher_t& dispatcher, args_t&&... args) {
		return iris_coroutine_dispatch_t<async_dispatcher_t>(dispatcher, std::forward<args_t>(args)...);
	}

	// get quota in coroutine
	template <typename quota_t, typename warp_t, typename async_worker_t = typename warp_t::async_worker_t>
	struct iris_quota_queue_t : iris_sync_t<warp_t, async_worker_t> {
		iris_quota_queue_t(async_worker_t& worker, quota_t& q) : iris_sync_t<warp_t, async_worker_t>(worker), quota(q) {}

		using amount_t = typename quota_t::amount_t;
		using info_t = typename iris_sync_t<warp_t, async_worker_t>::info_t;

		struct resource_t {
			resource_t() noexcept : host(nullptr) {}
			resource_t(iris_quota_queue_t& q, const amount_t& m) noexcept : host(&q), amount(m) {}
			resource_t(const resource_t&) = delete;
			resource_t(resource_t&& rhs) noexcept : host(rhs.host), amount(rhs.amount) { rhs.host = nullptr; }

			resource_t& operator = (const resource_t&) = delete;
			resource_t& operator = (resource_t&& rhs) noexcept {
				if (this != &rhs) {
					clear();

					host = rhs.host;
					amount = rhs.amount;
					rhs.host = nullptr;
				}

				return *this;
			}

			~resource_t() noexcept {
				clear();
			}

			void clear() noexcept {
				if (host != nullptr) {
					host->release(amount);
					host = nullptr;
				}
			}

			void merge(resource_t&& rhs) noexcept {
				if (host == nullptr) {
					*this = std::move(rhs);
				} else {
					IRIS_ASSERT(host == rhs.host);
					acquire(rhs.amount);
					rhs.host = nullptr;
				}
			}

			// add more
			void acquire(const amount_t& delta) noexcept {
				IRIS_ASSERT(host != nullptr);
				for (size_t i = 0; i < amount.size(); i++) {
					amount[i] += delta[i];
				}
			}

			// release part of them
			void release(const amount_t& delta) {
				IRIS_ASSERT(host != nullptr);
				for (size_t i = 0; i < amount.size(); i++) {
					IRIS_ASSERT(amount[i] >= delta[i]);
					amount[i] -= delta[i];
				}

				host->release(delta);
			}

			// move ownership of all quota
			amount_t move() noexcept {
				amount_t ret = get_amount();
				host = nullptr;
				for (size_t i = 0; i < amount.size(); i++) {
					amount[i] = 0;
				}

				return ret;
			}

			// this amount may be inaccurate
			const amount_t& get_amount() const noexcept {
				return amount;
			}

			iris_quota_queue_t* get_queue() const noexcept {
				return host;
			}
			
		protected:
			iris_quota_queue_t* host;
			amount_t amount;
		};

		struct awaitable_t {
			awaitable_t(iris_quota_queue_t& q, const amount_t& m, bool r) noexcept : host(q), amount(m), ready(r) {}

			bool await_ready() const noexcept {
				return ready;
			}

			void await_suspend(std::coroutine_handle<> handle) {
				info_t info;
				info.handle = std::move(handle);

				if constexpr (!std::is_same_v<warp_t, void>) {
					info.warp = warp_t::get_current();
				}

				host.acquire_queued(std::move(info), amount);
			}

			resource_t await_resume() noexcept {
				return resource_t(host, amount);
			}

		protected:
			iris_quota_queue_t& host;
			amount_t amount;
			bool ready;
		};

		awaitable_t guard(const amount_t& amount) {
			return awaitable_t(*this, amount, quota.acquire(amount));
		}

		bool acquire(const amount_t& amount) {
			return quota.acquire(amount);
		}

		void release(const amount_t& amount) {
			quota.release(std::move(amount));

			// Walk pending waiters and wake the next one whose request can
			// be satisfied.  Must hold the same lock used by acquire_queued()
			// so we cannot miss a waiter that is in the middle of pushing.
			while (true) {
				std::unique_lock<std::mutex> guard(lock);
				if (handles.empty()) {
					break;
				}

				auto& top = handles.top();
				if (quota.acquire(top.second)) {
					info_t h = std::move(top.first);
					handles.pop();
					guard.unlock();

					iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(h));
				} else {
					break;
				}
			}
		}

		amount_t get_amount() const noexcept {
			return quota.get();
		}

	protected:
		// Push a waiter into the queue using double-checked locking against
		// release().  Without the re-try inside the lock, the following
		// interleaving would leak a wakeup:
		//   T1 (consumer): quota.acquire() fails in awaitable_t ctor
		//   T2 (releaser): release() grabs lock, sees handles empty, exits
		//   T1 (consumer): pushes handle -- never woken
		void acquire_queued(info_t&& info, const amount_t& amount) {
			std::unique_lock<std::mutex> guard(lock);
			// retry: another thread may have released enough quota between
			// the initial guard() probe and our reaching this point.
			if (quota.acquire(amount)) {
				guard.unlock();
				iris_sync_t<warp_t, async_worker_t>::dispatch(std::move(info));
				return;
			}

			handles.push(std::make_pair(std::move(info), amount));
		}

	protected:
		quota_t& quota;
		std::mutex lock;
		iris_queue_list_t<std::pair<info_t, amount_t>> handles;
	};
}
