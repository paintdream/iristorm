/*
The Iristorm Concurrency Framework

This software is a C++ 11 Header-Only reimplementation of core part from project PaintsNow.

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

#include "iris_common.h"
#include <functional>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>

#if !defined(IRIS_NO_ATOMIC_WAIT) && defined(__cpp_lib_atomic_wait) && __cpp_lib_atomic_wait >= 201711L
#define IRIS_ATOMIC_WAIT 1
#else
#define IRIS_ATOMIC_WAIT 0
#endif

namespace iris {
	namespace impl {	
		// for exception safe, roll back atomic operations as needed
		enum class guard_operation_t : size_t {
			add, sub, invalidate
		};

		template <guard_operation_t operation>
		struct atomic_guard_t {
			atomic_guard_t(std::atomic<size_t>& var) : variable(&var) {}
			~atomic_guard_t() noexcept {
				if (variable != nullptr) {
					if /* constexpr */ (operation == guard_operation_t::add) {
						variable->fetch_add(1, std::memory_order_release);
					} else if /* constexpr */ (operation == guard_operation_t::sub) {
						variable->fetch_sub(1, std::memory_order_release);
					} else {
						variable->store(~size_t(0), std::memory_order_release);
					}
				}
			}

			void cleanup() {
				variable = nullptr;
			}

		private:
			std::atomic<size_t>* variable;
		};
	}

	// dispatch routines:
	//     1. from warp to warp. (queue_routine/queue_routine_post).
	//     2. from warp to external in parallel (queue_routine_parallel).
	// you can select implemention from warp/strand via 'strand' template parameter.
	template <typename worker_t, bool strand = true, typename base_t = void, template <typename...> class allocator_t = iris_default_block_allocator_t>
	struct iris_warp_t {
		// for exception safe!
		struct suspend_guard_t {
			suspend_guard_t(iris_warp_t* w) noexcept : warp(w) {}
			void cleanup() noexcept { warp = nullptr; }

			~suspend_guard_t() noexcept {
				// if compiler detects warp is nullptr
				// it can remove the ~suspend_guard_t() calling
				if (warp != nullptr) {
					warp->resume();
				}
			}

		private:
			iris_warp_t* warp;
		};

		struct preempt_guard_t {
			explicit preempt_guard_t(iris_warp_t& warp_instance, size_t level) noexcept : warp(warp_instance), suspend_level(level) {
#if IRIS_DEBUG
				current = iris_warp_t::get_current();
#endif
				state = warp.suspend_count.load(std::memory_order_acquire) <= suspend_level;
				if (state) {
					if (iris_warp_t::get_current() == &warp) {
						preempted = false;
					} else {
						preempted = warp.preempt();
						// recheck after preempted
						state = preempted && (warp.suspend_count.load(std::memory_order_relaxed) <= suspend_level);
					}
				} else {
					preempted = false;
				}
			}

			void cleanup() noexcept {
				state = preempted = false;
			}

			~preempt_guard_t() noexcept {
#if IRIS_DEBUG
				iris_warp_t* m = iris_warp_t::get_current();
				if (state) {
					IRIS_ASSERT(m == &warp);
				}
#endif

				if (preempted) {
					warp.yield();
#if IRIS_DEBUG
					iris_warp_t* n = iris_warp_t::get_current();
					IRIS_ASSERT(current == n);
#endif
				}
			}

			explicit operator bool() const noexcept {
				return state;
			}

			bool is_preempted() const noexcept {
				return preempted;
			}

		protected:
			iris_warp_t& warp;
			size_t suspend_level;
#if IRIS_DEBUG
			iris_warp_t* current;
#endif
			bool preempted;
			bool state;
		};

		static constexpr bool is_strand = strand;
		using async_worker_t = worker_t;
		using task_t = typename async_worker_t::task_base_t;
		using normal_task_t = typename async_worker_t::normal_task_t;

		struct alignas(alignof(normal_task_t)) function_t {
			template <typename initializer_t>
			function_t(initializer_t&& initializer) {
				initializer(get());
			}

			normal_task_t* get() noexcept {
				return reinterpret_cast<normal_task_t*>(storage);
			}

			uint8_t storage[sizeof(normal_task_t)];
		};

		using queue_buffer_t = iris_queue_list_t<function_t, allocator_t>;

		static constexpr size_t block_size = iris_extract_block_size<function_t, allocator_t>::value;
		enum class queue_state_t : size_t {
			idle, pending, executing
		};

		// moving capture is not supported until C++ 14
		// so we wrap some functors here

		struct execute_t {
			execute_t(iris_warp_t& w) noexcept : warp(w) {}
			void operator () () {
				warp.template execute<strand, false>();
			}

			iris_warp_t& warp;
		};

		template <typename callable_t>
		struct external_t {
			template <typename proc_t>
			external_t(iris_warp_t& w, proc_t&& c) noexcept : warp(w), callable(std::forward<proc_t>(c)) {}
			void operator () () {
				warp.queue_routine(std::move(callable));
			}

			iris_warp_t& warp;
			callable_t callable;
		};

		template <typename callable_t>
		struct suspend_t {
			template <typename proc_t>
			suspend_t(iris_warp_t& w, proc_t&& c) noexcept : warp(w), callable(std::forward<proc_t>(c)) {}
			void operator () () {
				suspend_guard_t guard(&warp);
				callable();
				guard.cleanup();

				warp.resume();
			}

			iris_warp_t& warp;
			callable_t callable;
		};

		// storage for queued tasks
		struct chain_storage_t {
			chain_storage_t() noexcept : executing_head(nullptr) {
				queueing_head.store(nullptr, std::memory_order_relaxed);
			}

			chain_storage_t(chain_storage_t&& rhs) noexcept : executing_head(rhs.executing_head) {
				rhs.executing_head = nullptr;
				rhs.queueing_head.store(rhs.queueing_head.load(std::memory_order_acquire), std::memory_order_release);
			}

			bool empty() const noexcept {
				return executing_head == nullptr && queueing_head.load(std::memory_order_acquire) == nullptr;
			}

			task_t* executing_head;
			std::atomic<task_t*> queueing_head;
		};

		struct grid_storage_t {
			grid_storage_t() noexcept : current_version(0), next_version(0) {
				barrier_version.store(0, std::memory_order_relaxed);
			}

			grid_storage_t(grid_storage_t&& rhs) noexcept {
				barrier_version.store(rhs.barrier_version.load(std::memory_order_acquire), std::memory_order_release);
				queue_buffers = std::move(rhs.queue_buffers);
				queue_versions = std::move(rhs.queue_versions);
				current_version = rhs.current_version;
				next_version = rhs.next_version;
			}

			bool empty() const noexcept {
				for (size_t i = 0; i < queue_buffers.size(); i++) {
					if (!queue_buffers[i].empty())
						return false;
				}

				return true;
			}

			std::vector<queue_buffer_t> queue_buffers;
			std::atomic<size_t> barrier_version;
			std::vector<size_t> queue_versions;
			size_t current_version;
			size_t next_version;
		};

		// do not copy this structure, only to move
		iris_warp_t(const iris_warp_t& rhs) = delete;
		iris_warp_t& operator = (const iris_warp_t& rhs) = delete;
		iris_warp_t& operator = (iris_warp_t&& rhs) = delete;

		// for strands, we prepare just one lock-free queue
		// for warps, we prepare one queue for each thread to remove mutex requirements

		template <bool s>
		typename std::enable_if<s>::type init_storage(size_t thread_count) noexcept {}

		template <bool s>
		typename std::enable_if<!s>::type init_storage(size_t thread_count) noexcept(noexcept(std::declval<iris_warp_t>().storage.queue_buffers.resize(thread_count))) {
			storage.queue_buffers.resize(thread_count);
			storage.queue_versions.resize(thread_count);
		}

		// initialize with specified priority, all tasks that runs on this warp will be scheduled with this priority
		explicit iris_warp_t(async_worker_t& worker, size_t prior = 0) : async_worker(worker), priority(prior), stack_next_warp(nullptr) {
			init_storage<strand>(worker.get_thread_count());

			thread_warp.store(nullptr, std::memory_order_relaxed);
			parallel_task_head.store(nullptr, std::memory_order_relaxed);
			suspend_count.store(0, std::memory_order_relaxed);
			queueing.store(queue_state_t::idle, std::memory_order_release);
		}

		iris_warp_t(iris_warp_t&& rhs) noexcept : async_worker(rhs.async_worker), priority(rhs.priority), stack_next_warp(rhs.stack_next_warp), storage(std::move(rhs.storage)) {
			thread_warp.store(rhs.thread_warp.load(std::memory_order_relaxed), std::memory_order_relaxed);
			parallel_task_head.store(rhs.parallel_task_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
			suspend_count.store(rhs.suspend_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
			queueing.store(rhs.queueing.load(std::memory_order_relaxed), std::memory_order_relaxed);

			rhs.stack_next_warp = nullptr;
			rhs.thread_warp.store(nullptr, std::memory_order_relaxed);
			rhs.parallel_task_head.store(nullptr, std::memory_order_relaxed);
			rhs.suspend_count.store(0, std::memory_order_relaxed);
			rhs.queueing.store(queue_state_t::idle, std::memory_order_release);
		}

		~iris_warp_t() noexcept {
			// a warp cannot be destructed in its context
			IRIS_ASSERT(get_current_internal() != this);

			// must poll manually before destruction!
			//
			// IMPORTANT lifetime rule: when multiple warps share the same
			// async_worker, tasks queued on one warp may post follow-up tasks
			// to a sibling warp.  Therefore *all* warps in such a group must
			// be fully drained (via iris_warp_t::poll(begin, end) over the
			// entire group) BEFORE the first of them is destroyed.  Destroying
			// one warp while siblings are still active is undefined behavior:
			// in debug the assertions below trip; in release nothing prevents
			// a still-running task from posting into this warp after its
			// queues are freed.  Recommended idiom:
			//
			//     while (iris_warp_t::poll(warps.begin(), warps.end())) {}
			//     warps.clear();   // safe
			IRIS_ASSERT(storage.empty());
			IRIS_ASSERT(!has_parallel_task());
		}

		// get stack warp pointer
		iris_warp_t* get_stack_next() const noexcept {
			return stack_next_warp;
		}

		// check if running, the result is meaningless for most calls in multithreaded context
		bool running() const noexcept {
			return thread_warp.load(std::memory_order_acquire) != nullptr;
		}

		// assert that current warp is just this
		void validate() const noexcept {
			IRIS_ASSERT(this == get_current_internal());
		}

		bool is_suspended() const noexcept {
			return suspend_count.load(std::memory_order_acquire) != 0;
		}

		bool empty() const noexcept {
			// must called while not running!
			return storage.empty();
		}

		// yield execution atomically, returns true on success.
		bool yield() noexcept(noexcept(std::declval<iris_warp_t>().flush())) {
			iris_warp_t** exp = &get_current_internal();
			if (thread_warp.load(std::memory_order_acquire) == exp) {
				invoke_leave_warp<iris_warp_t>();

				get_current_internal() = stack_next_warp;
				stack_next_warp = nullptr;
				thread_warp.store(nullptr, std::memory_order_release);

				if (queueing.exchange(queue_state_t::idle, std::memory_order_relaxed) == queue_state_t::pending) {
					flush();
				}

				return true;
			} else {
				IRIS_ASSERT(get_current_internal() == nullptr || exp == nullptr || *exp == nullptr);
				return false;
			}
		}

		// blocks all tasks preemptions, stacked with internally counting.
		bool suspend() noexcept {
			bool ret = suspend_count.fetch_add(1, std::memory_order_acquire) == 0;
			invoke_suspend_warp<iris_warp_t>();
			return ret;
		}

		// allows all tasks preemptions, stacked with internally counting.
		// returns true on final resume.
		bool resume() noexcept(noexcept(std::declval<iris_warp_t>().flush())) {
			bool ret = suspend_count.fetch_sub(1, std::memory_order_release) == 1;

			if (ret) {
				invoke_resume_warp<iris_warp_t>();
				// all suspend requests removed, try to flush me
				if (queueing.exchange(queue_state_t::idle, std::memory_order_relaxed) == queue_state_t::pending) {
					flush();
				}
			}

			return ret;
		}

		// send task to this warp. call it directly if we are on warp.
		template <typename callable_t>
		void queue_routine(callable_t&& func) noexcept(noexcept(func()) &&
			noexcept(std::declval<iris_warp_t>().template push<strand>(std::forward<callable_t>(func)))) {

			// can be executed immediately?
			// try to acquire execution, if it fails, just go posting
			preempt_guard_t preempt_guard(*this, 0);
			if (preempt_guard) {
				func();
			} else {
				queue_routine_post<callable_t>(std::forward<callable_t>(func));
			}
		}

		// send task to warp indicated by warp. always post it to queue.
		template <typename callable_t>
		void queue_routine_post(callable_t&& func) noexcept(noexcept(std::declval<iris_warp_t>().template push<strand>(std::forward<callable_t>(func)))) {
			if (!strand && async_worker.get_current_thread_index() == ~size_t(0)) {
				async_worker.queue(external_t<typename std::remove_reference<callable_t>::type>(*this, std::forward<callable_t>(func)), priority);
			} else {
				// always send to current thread slot of current warp.
				push<strand>(std::forward<callable_t>(func));
			}
		}

		// queue task parallelly to async_worker
		// it is useful to implement read-lock affairs about warp
		template <typename callable_t>
		void queue_routine_parallel(callable_t&& func) {
			queue_routine_parallel_internal<true>(std::forward<callable_t>(func));
		}

		template <typename callable_t>
		void queue_routine_parallel_post(callable_t&& func) {
			queue_routine_parallel_internal<false>(std::forward<callable_t>(func));
		}

		// queue a barrier here, any routines queued after this barrier must be scheduled after any routines before this barrier
		void queue_barrier() {
			queue_barrier_internal<strand>();
		}

		// cleanup the dispatcher, pass true to 'execute_remaining' to make sure all tasks are executed finally.
		template <bool poll_async_worker = true, typename iterator_t>
		static bool poll(iterator_t begin, iterator_t end) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			while (true) {
				// suspend all warps so we can take over tasks
				for (iterator_t p = begin; p != end; ++p) {
					iris_warp_t& warp = *p;
					warp.suspend();
				}

				size_t pending_count = 0;
				size_t executing_count = 0;

				for (iterator_t p = begin; p != end; ++p) {
					iris_warp_t& warp = *p;
					pending_count += warp.template invoke_enter_join_warp<iris_warp_t>();
				}

				for (iterator_t p = begin; p != end; ++p) {
					iris_warp_t& warp = *p;
					if (!warp.empty() || warp.has_parallel_task()) {
						pending_count++;
						preempt_guard_t preempt_guard(*p, ~size_t(0));
						if (preempt_guard) {
							executing_count++;
							warp.execute_parallel();
							// execute remaining
							warp.template execute_internal<strand, true>();
							break;
						}
					}
				}

				for (iterator_t p = begin; p != end; ++p) {
					iris_warp_t& warp = *p;
					pending_count += warp.template invoke_leave_join_warp<iris_warp_t>();
				}

				for (iterator_t p = begin; p != end; ++p) {
					iris_warp_t& warp = *p;
					warp.resume();
				}

				if /* constexpr */ (poll_async_worker) {
					async_worker_t* last = nullptr;
					for (iterator_t p = begin; p != end; ++p) {
						iris_warp_t& warp = *p;
						async_worker_t* t = &warp.get_async_worker();
						if (t != last) {
							if (t->poll() || !t->empty()) {
								pending_count++;
							}

							last = t;
						}
					}
				}

				if (pending_count == 0) {
					IRIS_ASSERT(executing_count == 0);
					return false;
				} else if (executing_count == 0) {
					return true;
				}
			}
		}

		template <bool poll_async_worker = true>
		static bool poll(std::initializer_list<std::reference_wrapper<iris_warp_t>>&& container) {
			return poll<poll_async_worker>(std::begin(container), std::end(container));
		}

		template <bool poll_async_worker = true>
		bool poll() {
			return poll<poll_async_worker>({ std::ref(*this) });
		}

		// get current thread's warp binding instance
		static iris_warp_t* get_current() noexcept {
			return get_current_internal();
		}

		async_worker_t& get_async_worker() noexcept {
			return async_worker;
		}

		const async_worker_t& get_async_worker() const noexcept {
			return async_worker;
		}

	protected:
		bool has_parallel_task() const noexcept {
			return parallel_task_head.load(std::memory_order_acquire) != nullptr;
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_flush_warp() {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_flush_warp() {
			static_cast<base_t*>(this)->flush_warp();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_enter_warp() {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_enter_warp() {
			static_cast<base_t*>(this)->enter_warp();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_leave_warp() {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_leave_warp() {
			static_cast<base_t*>(this)->leave_warp();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_suspend_warp() {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_suspend_warp() {
			static_cast<base_t*>(this)->suspend_warp();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_resume_warp() {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_resume_warp() {
			static_cast<base_t*>(this)->resume_warp();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value, size_t>::type invoke_enter_join_warp() {
			return 0;
		}

		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value, size_t>::type invoke_enter_join_warp() {
			return static_cast<base_t*>(this)->enter_join_warp();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value, size_t>::type invoke_leave_join_warp() {
			return 0;
		}

		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value, size_t>::type invoke_leave_join_warp() {
			return static_cast<base_t*>(this)->leave_join_warp();
		}

		// take execution atomically, returns true on success.
		bool preempt() noexcept {
			iris_warp_t** expected = nullptr;
			iris_warp_t* current = get_current_internal();
			if (thread_warp.compare_exchange_strong(expected, &get_current_internal(), std::memory_order_acquire)) {
				get_current_internal() = this;
				stack_next_warp = current;
				invoke_enter_warp<iris_warp_t>();

				return true;
			} else {
				IRIS_ASSERT(get_current_internal() != this);
				return false;
			}
		}

		template <bool self_execute, typename callable_t>
		void queue_routine_parallel_internal(callable_t&& func) {
			suspend();

			suspend_guard_t guard(this);
			if (thread_warp.load(std::memory_order_acquire) == nullptr) {
				if /* constexpr */ (self_execute) {
					func();

					// let guard resume
				} else {
					async_worker.queue(suspend_t<typename std::remove_reference<callable_t>::type>(*this, std::forward<callable_t>(func)), priority);
					guard.cleanup();
				}
			} else {
				// wait for current warp finish
				task_t* task = async_worker.new_task(suspend_t<typename std::remove_reference<callable_t>::type>(*this, std::forward<callable_t>(func)));
				
				// avoid legacy compiler bugs
				// see https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
				task_t* node = parallel_task_head.load(std::memory_order_relaxed);
				do {
					task->next = node;
				} while (!parallel_task_head.compare_exchange_weak(node, task, std::memory_order_acq_rel, std::memory_order_relaxed));

				guard.cleanup();
				flush();
			}
		}

		// get current warp index (saved in thread_local storage)
		static iris_warp_t*& get_current_internal() noexcept {
			return iris_static_instance_t<iris_warp_t*>::get_thread_local();
		}

		template <bool enable_strand>
		typename std::enable_if<enable_strand>::type queue_barrier_internal() {}

		template <bool enable_strand>
		typename std::enable_if<!enable_strand>::type queue_barrier_internal() {
			size_t counter = storage.barrier_version.fetch_add(1, std::memory_order_acquire) + 1;
			queue_routine_post([this, counter]() noexcept {
				storage.next_version = counter;
			});
		}

		// execute all tasks scheduled at once.
		template <bool enable_strand, bool force>
		typename std::enable_if<enable_strand>::type execute_internal() {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			// mark for queueing, avoiding flush me more than once.
			queueing.store(queue_state_t::executing, std::memory_order_release);
			iris_warp_t** warp_ptr = &get_current_internal();
			IRIS_ASSERT(*warp_ptr == this);

			size_t execute_counter;
			do {
				execute_counter = 0;
				task_t* p = storage.executing_head;
				if (p == nullptr) {
					p = storage.queueing_head.exchange(nullptr, std::memory_order_relaxed);
					task_t* q = nullptr;

					while (p != nullptr) {
						task_t* t = p->next;
						p->next = q;
						q = p;
						p = t;
					}

					p = q;
				}

				while (p != nullptr) {
					storage.executing_head = p->next; // mark next for exception safety
					p->next = nullptr;
					async_worker.execute_task(p);
					execute_counter++;

					if ((!force && is_suspended()) || *warp_ptr != this) {
						// interrupted: the queue may still hold tasks, so re-publish
						// as pending -- otherwise a concurrent final resume() can
						// exchange executing -> idle and neither side re-flushes
						// (lost wakeup, the remaining tasks would starve forever).
						queueing.store(queue_state_t::pending, std::memory_order_relaxed);
						return;
					}

					// go over next task
					p = storage.executing_head;
				}
			} while (execute_counter != 0);
		}

		template <bool enable_strand, bool force>
		typename std::enable_if<!enable_strand>::type execute_internal() {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			// mark for queueing, avoiding flush me more than once.
			queueing.store(queue_state_t::executing, std::memory_order_release);
			iris_warp_t** warp_ptr = &get_current_internal();
			IRIS_ASSERT(*warp_ptr == this);

			// execute tasks in queue_buffer until suspended 
			std::vector<queue_buffer_t>& queue_buffers = storage.queue_buffers;
			std::vector<size_t>& queue_versions = storage.queue_versions;
			size_t& current_version = storage.current_version;
			size_t& next_version = storage.next_version;
			size_t execute_counter;

			do {
				execute_counter = 0;
				size_t step_version = current_version;
				for (size_t i = 0; i < queue_buffers.size(); i++) {
					queue_buffer_t& buffer = queue_buffers[i];
					size_t& counter = queue_versions[i];

					next_version = counter;
					while (static_cast<ptrdiff_t>(current_version - counter) >= 0 && !buffer.empty()) {
						typename queue_buffer_t::element_t func = std::move(buffer.top());
						buffer.pop(); // pop up before calling

						async_worker.execute_task(func.get()); // may throws exceptions
						execute_counter++;
						counter = next_version;

						if ((!force && is_suspended()) || *warp_ptr != this) {
							// interrupted: the queue may still hold tasks, so re-publish
							// as pending -- otherwise a concurrent final resume() can
							// exchange executing -> idle and neither side re-flushes
							// (lost wakeup, the remaining tasks would starve forever).
							queueing.store(queue_state_t::pending, std::memory_order_relaxed);
							return;
						}
					}

					if (current_version + 1 == counter) {
						step_version = counter;
					} else if (static_cast<ptrdiff_t>(current_version - counter) > 0) { // in case of counter overflow
						counter = current_version;
					}
				}

				current_version = step_version;
			} while (execute_counter != 0);
		}

		template <bool enable_strand, bool force>
		void execute() noexcept(noexcept(std::declval<iris_warp_t>().template execute_internal<enable_strand, force>())) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			if (!is_suspended()) {
				// try to acquire execution, if it fails, there must be another thread doing the same thing
				// and it's ok to return immediately.
				preempt_guard_t preempt_guard(*this, 0);
				if (preempt_guard) {
					execute_parallel();

					if (!is_suspended()) { // double check for suspend_count
						execute_internal<enable_strand, force>();
						
						bool preempted = preempt_guard.is_preempted();
						preempt_guard.cleanup();
						if (preempted && !yield()) {
							// already yielded? try to repost me to process remaining tasks.
							flush();
						}
					} else {
						queueing.store(queue_state_t::pending, std::memory_order_relaxed);
					}
				}
			}
			
			// always try to execute parallel tasks
			if (has_parallel_task()) {
				preempt_guard_t preempt_guard(*this, ~size_t(0));
				if (preempt_guard) {
					execute_parallel();
				}
			}
		}

		// parallel tasks are not guaranteed to be queued sequentially as input order
		void execute_parallel() {
			while (has_parallel_task()) {
				task_t* p = parallel_task_head.exchange(nullptr, std::memory_order_acquire);

				while (p != nullptr) {
					IRIS_ASSERT(is_suspended());
					task_t* q = p->next;
					p->next = nullptr;
					async_worker.queue_task(p, priority);
					p = q;
				}
			}
		}

		// commit execute request to specified worker.
		void flush() {
			// if current state is executing, the executing routine will reinvoke flush() if it detected pending state while exiting
			// so we just need to queue a flush routine as soon as current state is idle
			if (queueing.load(std::memory_order_acquire) != queue_state_t::pending) {
				if (queueing.exchange(queue_state_t::pending, std::memory_order_acq_rel) == queue_state_t::idle) {
					invoke_flush_warp<iris_warp_t>();
					async_worker.queue(execute_t(*this), priority);
				}
			}
		}

		// queue task from specified thread.
		template <bool s, typename callable_t>
		typename std::enable_if<s>::type push(callable_t&& func) {
			task_t* task = async_worker.new_task(std::forward<callable_t>(func));

			// avoid legacy compiler bugs
			// see https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
			task_t* node = storage.queueing_head.load(std::memory_order_relaxed);
			do {
				task->next = node;
			} while (!storage.queueing_head.compare_exchange_weak(node, task, std::memory_order_acq_rel, std::memory_order_relaxed));

			flush();
		}

		template <bool s, typename callable_t>
		typename std::enable_if<!s>::type push(callable_t&& func) {
			size_t thread_index = async_worker.get_current_thread_index();
			auto initializer = [this, &func](normal_task_t* storage) {
				if /* constexpr */ (std::is_rvalue_reference<callable_t&&>::value) {
					async_worker.construct_task_at(storage, std::move(func));
				} else {
					async_worker.construct_task_at(storage, func);
				}
			};

			if (thread_index != ~size_t(0)) {
				std::vector<queue_buffer_t>& queue_buffers = storage.queue_buffers;
				IRIS_ASSERT(thread_index < queue_buffers.size());
				queue_buffer_t& buffer = queue_buffers[thread_index];
				buffer.push(std::move(initializer));

				// flush the task immediately
				flush();
			} else {
				IRIS_ASSERT(async_worker.is_terminated());
				IRIS_ASSERT(!storage.queue_buffers.empty());
				storage.queue_buffers[0].push(std::move(initializer));
			}
		}

	protected:
		// immutable descriptor: read by any thread that posts tasks into this warp
		// (queue_routine_post/flush read priority, push/flush read async_worker).
		// It never changes after construction, so keep it apart from the hot atomics
		// below to prevent remote readers from ping-ponging the writers' cache line.
		async_worker_t& async_worker; // host async worker
		size_t priority;

		// hot scheduling/execution state, mutated from the executing thread and from
		// remote posters. Pinned to a fresh cache line (via alignas) so that:
		//   1. it no longer shares a line with the read-only descriptor above, and
		//   2. combined with the type-level alignas(default_cache_line_size), sibling
		//      warps stored contiguously (e.g. std::vector<iris_warp_t>) never straddle
		//      the same cache line.
		size_t _cacheline_paddings[6];
		/* alignas(default_cache_line_size) */ iris_warp_t* stack_next_warp;
		std::atomic<iris_warp_t**> thread_warp; // save the running thread warp address.
		std::atomic<size_t> suspend_count; // current suspend count
		std::atomic<queue_state_t> queueing; // is flush request sent to async_worker? 0 : not yet, 1 : yes, 2 : is to flush right away.
		std::atomic<task_t*> parallel_task_head; // linked-list for pending parallel tasks
		typename std::conditional<strand, chain_storage_t, grid_storage_t>::type storage; // task storage
	};

	// dispatcher based-on directed-acyclic graph
	template <typename base_warp_t, typename base_t = void, template <typename...> typename function_template_t = std::function>
	struct iris_dispatcher_t {
		using warp_t = base_warp_t;
		// wraps task data
		struct routine_t;
		struct routine_handle_t;
		using function_t = function_template_t<void(const routine_handle_t&)>;

		struct routine_t {
		protected:
			template <typename func_t>
			routine_t(warp_t* w, func_t&& func, size_t prior) noexcept : routine(std::forward<func_t>(func)), priority(prior), warp(w), next(nullptr) {
				lock_count.store(1, std::memory_order_relaxed);
				std::memset(next_tasks, 0, sizeof(next_tasks));
			}

			friend struct iris_dispatcher_t;
			function_t routine;
			std::atomic<size_t> lock_count;
			size_t priority;
			warp_t* warp;
			routine_t* next;
			routine_t* next_tasks[4];
		};

		struct routine_handle_t {
			routine_handle_t(routine_t* r = nullptr) noexcept : routine(r) {}
			~routine_handle_t() noexcept { IRIS_ASSERT(routine == nullptr); }
			routine_handle_t(routine_handle_t&& rhs) noexcept : routine(rhs.routine) { rhs.routine = nullptr; }
			routine_handle_t& operator = (routine_handle_t&& rhs) noexcept { std::swap(routine, rhs.routine); return *this; }
			routine_t* move() noexcept {
				return std::exchange(routine, nullptr);
			}

			void clear() noexcept {
				routine = nullptr;
			}

			explicit operator bool() const noexcept {
				return routine != nullptr;
			}

			size_t get_lock_count() const noexcept {
				return routine->lock_count.load(std::memory_order_relaxed);
			}

			friend struct iris_dispatcher_t;
		protected:
			routine_t* routine;
		};

		// on execution of tasks
		struct execute_t {
			execute_t(iris_dispatcher_t& d, routine_t* r) noexcept : dispatcher(d), routine(r) {}

			void operator () () {
				dispatcher.execute(routine);
			}

			iris_dispatcher_t& dispatcher;
			routine_t* routine;
		};

		using async_worker_t = typename warp_t::async_worker_t;
		using allocator_t = typename async_worker_t::template general_allocator_t<routine_t>;

		iris_dispatcher_t(async_worker_t& worker) noexcept : async_worker(worker) {
			pending_count.store(0, std::memory_order_relaxed);
		}

		~iris_dispatcher_t() noexcept {
			IRIS_ASSERT(get_pending_count() == 0);
		}

		async_worker_t& get_async_worker() noexcept {
			return async_worker;
		}

		// queue a routine, notice that priority takes effect if and only if warp == nullptr
		template <typename func_t>
		routine_handle_t allocate(warp_t* warp, func_t&& func, size_t priority = 0) {
			routine_t* routine = routine_allocator.allocate(1);
			new (routine) routine_t(warp, std::forward<func_t>(func), priority);
			pending_count.fetch_add(1, std::memory_order_acquire);
			return routine;
		}

		routine_handle_t allocate(warp_t* warp) {
			return allocate(warp, function_t());
		}

		// set routine dependency [from] -> [to]
		void order(const routine_handle_t& from_handle, const routine_handle_t& to_handle) {
			return order_internal(from_handle.routine, to_handle.routine);
		}

		// delay a task temporarily, must called before it actually runs
		routine_handle_t defer(const routine_handle_t& routine_handle) noexcept {
			IRIS_ASSERT(pending_count.load(std::memory_order_acquire) != 0);
			// IRIS_ASSERT(routine_handle.routine->lock_count.load(std::memory_order_relaxed) != 0);
			routine_handle.routine->lock_count.fetch_add(1, std::memory_order_relaxed);
			return routine_handle_t(routine_handle.routine);
		}

		// dispatch a task
		void dispatch(routine_handle_t&& routine_handle) {
			routine_t* routine = std::exchange(routine_handle.routine, nullptr);
			impl::atomic_guard_t<impl::guard_operation_t::add> guard(routine->lock_count);
			if (routine->lock_count.fetch_sub(1, std::memory_order_relaxed) == 1) {
				if (routine->routine) {
					// if not a warped routine, queue it to worker directly.
					if (routine->warp == nullptr) {
						async_worker.queue(execute_t(*this, routine), routine->priority);
					} else {
						routine->warp->queue_routine(execute_t(*this, routine));
					}
				} else {
					execute(routine);
				}
			}

			guard.cleanup();
		}

		size_t get_pending_count() const noexcept {
			return pending_count.load(std::memory_order_acquire);
		}

	protected:
		void order_internal(routine_t* from, routine_t* to) {
			IRIS_ASSERT(from != nullptr);
			IRIS_ASSERT(to != nullptr);
			IRIS_ASSERT(from->lock_count.load(std::memory_order_relaxed) != 0);
			IRIS_ASSERT(to->lock_count.load(std::memory_order_relaxed) != 0);

#if IRIS_DEBUG
			validate(from, to);
#endif

			for (size_t i = 0; i < sizeof(from->next_tasks) / sizeof(from->next_tasks[0]); i++) {
				routine_t*& p = from->next_tasks[i];
				if (p == nullptr) {
					to->lock_count.fetch_add(1, std::memory_order_relaxed);
					p = to;
					return;
				}
			}

			if (!from->next_tasks[0]->routine) { // is junction node?
				order_internal(from->next_tasks[0], to);
			} else {
				to->lock_count.fetch_add(1, std::memory_order_relaxed);
				routine_t* p = allocate(from->warp).move();
				p->next_tasks[0] = from->next_tasks[0];
				p->next_tasks[1] = to;
				from->next_tasks[0] = p;
			}
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_complete() {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_complete() {
			static_cast<base_t*>(this)->dispatcher_complete();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_enter_execute(routine_t* routine) {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_enter_execute(routine_t* routine) {
			routine_handle_t handle(routine);
			static_cast<base_t*>(this)->dispatcher_enter_execute(handle);
			handle.clear();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_leave_execute(routine_t* routine) {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_leave_execute(routine_t* routine) {
			routine_handle_t handle(routine);
			static_cast<base_t*>(this)->dispatcher_leave_execute(handle);
			handle.clear();
		}

		template <typename type_t>
		typename std::enable_if<!std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_cleanup() {}
		template <typename type_t>
		typename std::enable_if<std::is_base_of<type_t, base_t>::value>::type invoke_dispatcher_cleanup() {
			static_cast<base_t*>(this)->dispatcher_cleanup();
		}

		void execute(routine_t* routine) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);
			IRIS_ASSERT(routine->lock_count.load(std::memory_order_relaxed) == 0);

			if (routine->routine) {
				invoke_dispatcher_enter_execute<iris_dispatcher_t>(routine);
				routine_handle_t handle(routine);
				auto r = std::move(routine->routine);
				routine->routine = {};
				r(handle);
				handle.clear();
				invoke_dispatcher_leave_execute<iris_dispatcher_t>(routine);
			}

			if (routine->lock_count.load(std::memory_order_relaxed) != 0) {
				// deferred by routine
				return;
			}

			routine->lock_count.store(~size_t(0), std::memory_order_relaxed);
			for (size_t i = 0; i < sizeof(routine->next_tasks) / sizeof(routine->next_tasks[0]); i++) {
				routine_t*& p = routine->next_tasks[i];
				if (p != nullptr) {
					IRIS_ASSERT(p != routine);
					dispatch(p);
					p = nullptr;
				}
			}

			routine->~routine_t();
			routine_allocator.deallocate(routine, 1);
			// all pending routines finished?
			if (pending_count.fetch_sub(1, std::memory_order_release) == 1) {
				invoke_dispatcher_complete<iris_dispatcher_t>();
			}
		}

		static void validate(routine_t* from, routine_t* to) noexcept {
			IRIS_ASSERT(from != to);

			for (size_t i = 0; i < sizeof(to->next_tasks) / sizeof(to->next_tasks[0]); i++) {
				routine_t* p = to->next_tasks[i];
				if (p != nullptr) {
					validate(from, p);
				}
			}
		}

	protected:
		async_worker_t& async_worker;
		allocator_t routine_allocator;
		std::atomic<size_t> pending_count;
	};

	// here we code a trivial worker demo
	// could be replaced by your implementation
	template <typename thread_t = std::thread, typename large_callable_t = std::function<void()>, template <typename...> class allocator_t = iris_default_object_allocator_t, size_t default_task_duplicate_count = 4, size_t default_sub_allocator_count = 4>
	struct iris_async_worker_t {
		// task wrapper
		struct task_base_t {
			task_base_t(task_base_t* n, void (*e)(task_base_t*), size_t index) : next(n), executor(e), alloc_index(index) {}

			task_base_t* next;
			void (*executor)(task_base_t* instance);
			size_t alloc_index;

			task_base_t(task_base_t&& rhs) noexcept = delete;
			task_base_t& operator = (task_base_t&& rhs) noexcept = delete;
		};

		template <typename callback_t>
		struct alignas(sizeof(size_t) * 8) task_t : task_base_t {
			template <typename func_t>
			task_t(func_t&& func, task_t* n, size_t index) noexcept(noexcept(callback_t(std::forward<func_t>(func))))
				: task(std::forward<func_t>(func)), task_base_t(n, &task_t::execute, index) {}

			struct task_guard_t {
				task_guard_t(task_t* t) : task(t) {}
				~task_guard_t() noexcept {
					task->~task_t();
				}

				task_t* task;
			};

			static void execute(task_base_t* instance) {
				task_t* task = static_cast<task_t*>(instance);
				task_guard_t guard(task);
				task->task();
			}

			callback_t task;
		};

		static constexpr size_t task_head_duplicate_count = default_task_duplicate_count;
		static constexpr size_t sub_allocator_count = default_sub_allocator_count;

		template <typename element_t>
		using general_allocator_t = allocator_t<element_t>;
		using normal_task_t = task_t<void (*)()>;
		using large_task_t = task_t<large_callable_t>;
		using task_allocator_t = allocator_t<normal_task_t>;
		using large_task_allocator_t = allocator_t<large_task_t>;
		using root_allocator_t = typename task_allocator_t::root_allocator_t;

		iris_async_worker_t() : internal_thread_count(0), priority_task_threshold(0), waiting_thread_count(0), limit_count(0), timed_waiters(0) {
			proxy_get_current_thread_index = &iris_async_worker_t::get_current_thread_index_internal;
			priority_task_handler = [](task_base_t*, size_t&) { return false; };
			running_count.store(0, std::memory_order_relaxed);
			task_count.store(0, std::memory_order_relaxed);
			terminated.store(1, std::memory_order_release);
#if IRIS_ATOMIC_WAIT
			wake_epoch.store(0, std::memory_order_relaxed);
#endif
		}

		explicit iris_async_worker_t(size_t thread_count) : iris_async_worker_t() {
			resize(thread_count);
		}

		void resize(size_t thread_count) {
			IRIS_ASSERT(task_heads.empty()); // must not started

			threads.resize(thread_count);
			internal_thread_count = thread_count;
		}

		// optional OS-affinity hook for core pinning (see test/iris_affinity_demo.cpp):
		// invoked once at the very beginning of every internal thread procedure with
		// the internal slot index in [0, internal_thread_count), so that a whole
		// worker (one hardware placement domain: a chiplet/LLC domain, or a
		// big.LITTLE class) can be pinned onto its core set. Pin from *inside* the
		// thread (GetCurrentThread()/pthread_self()); attaching from the spawner
		// races with thread startup. The handler must be installed before start()
		// and must not throw; external threads appended via append() run user
		// procedures and pin themselves.
		void set_thread_pinner(std::function<void(size_t)>&& pinner) {
			IRIS_ASSERT(task_heads.empty()); // must not started
			thread_pinner = std::move(pinner);
		}

		// initialize and start thread poll
		void start() {
			IRIS_ASSERT(task_heads.empty()); // must not started

			std::vector<std::atomic<task_base_t*>> heads(threads.size() * task_head_duplicate_count);
			for (size_t i = 0; i < heads.size(); i++) {
				heads[i].store(nullptr, std::memory_order_relaxed);
			}

			task_heads = std::move(heads);
			terminated.store(0, std::memory_order_release);

			for (size_t i = 0; i < internal_thread_count; i++) {
				threads[i] = thread_t([this, i]() {
					IRIS_PROFILE_THREAD("iris_async_worker", i);
					thread_loop(i);
				});
			}
		}

		void make_current(size_t i) noexcept {
			proxy_get_current_thread_index() = i;
		}

		void thread_loop(size_t i) {
			if (thread_pinner) {
				thread_pinner(i);
			}

			make_current(i);

			while (!is_terminated()) {
				if (!poll_one()) {
					delay();
				}
			}

			make_current(~size_t(0));
		}

		// guard for exception on wait_for
		struct waiting_guard_t {
			waiting_guard_t(iris_async_worker_t* w) noexcept : worker(w) {
				worker->waiting_thread_count.fetch_add(1, std::memory_order_release);
			}

			~waiting_guard_t() noexcept {
				worker->waiting_thread_count.fetch_sub(1, std::memory_order_release);
			}

		private:
			iris_async_worker_t* worker;
		};

		// guard for exception on wait_for (timed poll_one() sleeps on the legacy condvar)
		struct timed_waiting_guard_t {
			timed_waiting_guard_t(iris_async_worker_t* w) noexcept : worker(w) {
				worker->timed_waiters.fetch_add(1, std::memory_order_release);
			}

			~timed_waiting_guard_t() noexcept {
				worker->timed_waiters.fetch_sub(1, std::memory_order_release);
			}

		private:
			iris_async_worker_t* worker;
		};

		friend struct waiting_guard_t;
		friend struct timed_waiting_guard_t;

		// append new customized thread to worker
		// must be called before start()
		// notice that we allow a dummy thread placeholder here (if you just want to do polling outside the internal threads)
		template <typename... args_t>
		size_t append(args_t&&... args) {
			IRIS_ASSERT(is_terminated());
			size_t id = threads.size();
			threads.emplace_back(std::forward<args_t>(args)...);
			return id;
		}

		// get thread instance of given id
		thread_t& get(size_t i) noexcept {
			return threads[i];
		}

		// poll any task from thread pool manually
		bool poll_one() {
			size_t inv_priority = running_count.fetch_add(1, std::memory_order_acquire);
			running_guard_t guard(running_count);
			return poll_one_internal(threads.size() + 1 - std::min(inv_priority + 1, threads.size()));
		}

		// poll any task from thread poll manually with given priority
		bool poll_one(size_t priority) {
			running_count.fetch_add(1, std::memory_order_acquire);
			running_guard_t guard(running_count);
			return poll_one_internal(std::min(priority + 1, threads.size()));
		}

		// poll any task from thread poll manually with given priority in specified duration
		// usually used in your customized thread procedures
		template <typename duration_t>
		bool poll_one(size_t priority, duration_t&& delay) {
			if (!poll_one(priority)) {
#if IRIS_ATOMIC_WAIT
				// lost-wakeup guard: the waker only notifies the condvar when it
				// observes timed_waiters != 0, so a bump occurring before our
				// registration would go unnoticed. snapshot the epoch first and
				// re-check it under the mutex after registering: any waker that
				// missed our registration (its timed_waiters load precedes it)
				// must have bumped the epoch before that load, hence before this
				// re-check, and we skip the sleep and re-poll instead.
				size_t epoch = wake_epoch.load(std::memory_order_acquire);
#endif
				timed_waiting_guard_t guard(this);
				std::unique_lock<std::mutex> lock(mutex);
#if IRIS_ATOMIC_WAIT
				if (wake_epoch.load(std::memory_order_acquire) == epoch) {
					condition.wait_for(lock, std::forward<duration_t>(delay));
				}
#else
				condition.wait_for(lock, std::forward<duration_t>(delay));
#endif
				lock.unlock();

				return poll_one(priority);
			} else {
				return true;
			}
		}

		// guard for exception on running
		struct running_guard_t {
			std::atomic<size_t>& count;
			running_guard_t(std::atomic<size_t>& var) noexcept : count(var) {}
			~running_guard_t() noexcept { count.fetch_sub(1, std::memory_order_release); }
		};

		~iris_async_worker_t() noexcept {
			terminate();
			join();

			IRIS_ASSERT(task_count.load(std::memory_order_acquire) == 0);
		}

		// get current thread index
		size_t get_current_thread_index() const noexcept { return proxy_get_current_thread_index(); }

		// get the count of threads in worker, including customized threads
		size_t get_thread_count() const noexcept {
			return threads.size();
		}

		// get the count of waiting task
		size_t get_task_count() const noexcept {
			return task_count.load(std::memory_order_acquire);
		}

		// limit the count of running thread. e.g. 0 is not limited, 1 is to pause one thread from running, etc.
		void limit(size_t count) noexcept {
			limit_count = count;
		}

		// queue a task to worker with given priority [0, thread_count - 1], which 0 is the highest priority
		template <typename callable_t>
		struct is_large_task {
			static constexpr bool value = sizeof(task_t<callable_t>) > sizeof(normal_task_t);
		};

		template <typename callable_t>
		typename std::enable_if<is_large_task<typename std::remove_reference<callable_t>::type>::value, task_base_t*>::type new_task(callable_t&& func) {
			large_task_t* task = large_task_allocator.allocate(1);
			new (task) large_task_t(std::forward<callable_t>(func), nullptr, 0);
			task_count.fetch_add(1, std::memory_order_relaxed);

			return task;
		}

		template <typename callable_t>
		typename std::enable_if<!is_large_task<typename std::remove_reference<callable_t>::type>::value, task_base_t*>::type new_task(callable_t&& func) {
			static thread_local size_t allocator_spin = 0;
			size_t index = (get_current_thread_index() + allocator_spin++) % sub_allocator_count;
			task_allocator_t& current_allocator = task_allocators[index];
			task_t<callable_t>* task = reinterpret_cast<task_t<callable_t>*>(current_allocator.allocate(1));
			static_assert(sizeof(task_t<callable_t>) == sizeof(normal_task_t), "Task size mismatch!");
			new (task) task_t<callable_t>(std::forward<callable_t>(func), nullptr, index + 1);
			task_count.fetch_add(1, std::memory_order_relaxed);

			return task;
		}

		template <typename callable_t>
		typename std::enable_if<is_large_task<typename std::remove_reference<callable_t>::type>::value, task_base_t*>::type construct_task_at(normal_task_t* task, callable_t&& func) {
			task_base_t* p = new_task(std::forward<callable_t>(func));
			auto adapter = [this, p]() {
				execute_task(p);
			};

			static_assert(!is_large_task<decltype(adapter)>::value, "Incompatible platform, please increase the size of normal_task_t!");
			return construct_task_at(task, std::move(adapter));
		}

		template <typename callable_t>
		typename std::enable_if<!is_large_task<typename std::remove_reference<callable_t>::type>::value, task_base_t*>::type construct_task_at(normal_task_t* task, callable_t&& func) {
			return new (task) task_t<callable_t>(std::forward<callable_t>(func), nullptr, ~(size_t)0);
		}

		// guard for exceptions on polling
		struct poll_guard_t {
			poll_guard_t(iris_async_worker_t& w, task_base_t* t) noexcept : worker(w), task(t) {}
			~poll_guard_t() noexcept {
				// do cleanup work
				size_t index = task->alloc_index;

				if (index != ~size_t(0)) {
					if (index == 0) {
						worker.large_task_allocator.deallocate(static_cast<large_task_t*>(task), 1);
					} else {
						worker.task_allocators[index - 1].deallocate(static_cast<normal_task_t*>(task), 1);
					}
					
					worker.task_count.fetch_sub(1, std::memory_order_release);
				}
			}

			iris_async_worker_t& worker;
			task_base_t* task;
		};

		void execute_task(task_base_t* task) {
			poll_guard_t guard(*this, task);
			task->executor(task);
		}

		// task with priority == ~(size_t)0 will be filterred
		void queue_task(task_base_t* task, size_t priority = 0) {
			IRIS_ASSERT(task != nullptr && task->next == nullptr);
			if (static_cast<ptrdiff_t>(priority) < static_cast<ptrdiff_t>(priority_task_threshold) && priority_task_handler(task, priority)) {
				return;
			}

			if (!is_terminated()) {
				IRIS_ASSERT(!threads.empty());
				priority = std::min(priority, std::max(internal_thread_count, (size_t)1) - 1u);

				// try empty slots first
				size_t index = 0;
				ptrdiff_t max_diff = std::numeric_limits<ptrdiff_t>::min();
				size_t thread_count = threads.size();
				size_t current_thread_index = get_current_thread_index();
				current_thread_index = current_thread_index == ~size_t(0) ? 0 : current_thread_index;

				for (size_t n = 0; n < task_head_duplicate_count; n++) {
					size_t k = (n + current_thread_index) % task_head_duplicate_count;
					std::atomic<task_base_t*>& task_head = task_heads[priority + k * thread_count];
					task_base_t* expected = nullptr;
					if (task_head.compare_exchange_strong(expected, task, std::memory_order_release)) {
						// dispatch immediately
						wakeup_one_with_priority(priority);
						return;
					} else {
						ptrdiff_t diff = task - expected;
						if (diff >= max_diff) {
							max_diff = diff;
							index = k;
						}
					}
				}

				// full, chain to farest one
				std::atomic<task_base_t*>& task_head = task_heads[priority + index * thread_count];

				// avoid legacy compiler bugs
				// see https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
				task_base_t* node = task_head.load(std::memory_order_relaxed);
				do {
					task->next = node;
				} while (!task_head.compare_exchange_weak(node, task, std::memory_order_acq_rel, std::memory_order_relaxed));

				// dispatch immediately
				wakeup_one_with_priority(priority);
			} else if (task_heads.empty()) {
				// not started
				execute_task(task); // execute immediately
			} else {
				// terminate requested, chain to default task_head at 0
				std::atomic<task_base_t*>& task_head = task_heads[0];
				task_base_t* node = task_head.load(std::memory_order_relaxed);
				do {
					task->next = node;
				} while (!task_head.compare_exchange_weak(node, task, std::memory_order_acq_rel, std::memory_order_relaxed));
			}
		}

		template <typename callable_t>
		void queue(callable_t&& callable, size_t priority = 0) {
			queue_task(new_task(std::forward<callable_t>(callable)), priority);
		}

		// mark as terminated
		void terminate() {
			terminated.store(1, std::memory_order_release);
			wakeup_all();
		}

		// is about to terminated
		bool is_terminated() const noexcept {
			return terminated.load(std::memory_order_acquire) != 0;
		}

		// wait for all threads in worker to be finished.
		void join() {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			for (size_t i = 0; i < threads.size(); i++) {
				if (threads[i].joinable()) {
					threads[i].join();
				}
			}

			IRIS_ASSERT(running_count.load(std::memory_order_acquire) == 0);
			IRIS_ASSERT(waiting_thread_count.load(std::memory_order_acquire) == 0);
			while (poll()) {}

			task_heads.clear();
			threads.clear();
		}

		bool empty() const noexcept {
			return get_task_count() == 0;
		}

		// cleanup all pending tasks
		bool poll() {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			bool empty = true;
			for (size_t i = 0; i < task_heads.size(); i++) {
				std::atomic<task_base_t*>& task_head = task_heads[i];
				task_base_t* task = task_head.exchange(nullptr, std::memory_order_acquire);
				empty = empty && (task == nullptr);

				while (task != nullptr) {
					task_base_t* p = task;
					task = task->next;
					execute_task(p);
				}
			}

			return !empty;
		}

		// notify threads in thread pool, usually used for customized threads
		void wakeup_one() {
#if IRIS_ATOMIC_WAIT
			// release bump pairs with the acquire load inside wait(): a waiter that
			// observes the bumped epoch also observes every task pushed before it,
			// and wait() re-checks the word under the kernel wait, so a bump/notify
			// pair is never lost
			wake_epoch.fetch_add(1, std::memory_order_release);
			wake_epoch.notify_one();
			if (timed_waiters.load(std::memory_order_acquire) != 0) {
				std::lock_guard<std::mutex> lock(mutex);
				condition.notify_all(); // timed poll_one() sleeps here (no deadline on atomic wait)
			}
#else
			std::lock_guard<std::mutex> lock(mutex);
			condition.notify_one();
#endif
		}

		void wakeup_all() {
#if IRIS_ATOMIC_WAIT
			wake_epoch.fetch_add(1, std::memory_order_release);
			wake_epoch.notify_all();
			if (timed_waiters.load(std::memory_order_acquire) != 0) {
				std::lock_guard<std::mutex> lock(mutex);
				condition.notify_all();
			}
#else
			std::lock_guard<std::mutex> lock(mutex);
			condition.notify_all();
#endif
		}

		// blocked delay for any task
		void delay() {
			if (!is_terminated()) {
				waiting_guard_t guard(this);

#if IRIS_ATOMIC_WAIT
				// adaptive spin absorbs in-flight pushes before parking
				static constexpr size_t delay_spin_rounds = 32;
				static constexpr size_t delay_spin_pauses = 16;

				for (size_t spins = 0; !is_terminated(); ) {
					if (task_count.load(std::memory_order_acquire) != 0) {
						if (fetch(waiting_thread_count.load(std::memory_order_relaxed)).first != ~size_t(0)) {
							return;
						}
					}

					if (spins < delay_spin_rounds) {
						spins++;
						for (size_t i = 0; i < delay_spin_pauses; i++) {
							IRIS_CPU_PAUSE();
						}

						continue;
					}

					// park side of the Dekker pair: orders our registration store
					std::atomic_thread_fence(std::memory_order_seq_cst);
					if (fetch(waiting_thread_count.load(std::memory_order_relaxed)).first != ~size_t(0)) {
						return;
					}

					size_t epoch = wake_epoch.load(std::memory_order_acquire);
					if (is_terminated()) {
						return;
					}

					if (fetch(waiting_thread_count.load(std::memory_order_relaxed)).first != ~size_t(0)) {
						return;
					}

					wake_epoch.wait(epoch, std::memory_order_acquire);
					spins = 0;
				}
#else
				std::unique_lock<std::mutex> lock(mutex);

				if (fetch(waiting_thread_count.load(std::memory_order_relaxed)).first == ~size_t(0)) {
					if (!is_terminated()) {
						condition.wait(lock);
					}
				}
#endif
			}
		}

		void set_priority_task_handler(std::function<bool(task_base_t*, size_t&)>&& handler, size_t threshold) noexcept {
			priority_task_handler = std::move(handler);
			priority_task_threshold = threshold;
		}

		struct thread_index_t {
			thread_index_t() noexcept : value(~size_t(0)) {}
			size_t value;
		};

	protected:
		static size_t& get_current_thread_index_internal() noexcept {
			return iris_static_instance_t<thread_index_t>::get_thread_local().value;
		}

		void wakeup_one_with_priority(size_t priority) {
#if IRIS_ATOMIC_WAIT && !defined(__x86_64__) && !defined(_M_X64) && !defined(__i386__) && !defined(_M_IX86)
			// consumer side of the Dekker pair (see delay()): on x86 the push CAS is a
			// locked instruction and already orders the push before this gate load
			std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
			if (waiting_thread_count.load(std::memory_order_seq_cst) > priority + limit_count) {
				wakeup_one();
			}
		}

		// try fetching a task with given priority
		std::pair<size_t, size_t> fetch(size_t priority_size) const noexcept {
			size_t thread_count = threads.size();
			if (thread_count != 0) {
				size_t current_thread_index = get_current_thread_index();
				current_thread_index = current_thread_index == ~size_t(0) ? 0 : current_thread_index;

				for (size_t k = 0; k < task_head_duplicate_count; k++) {
					size_t m = (k + current_thread_index) % task_head_duplicate_count;
					for (size_t n = 0; n < priority_size; n++) {
						size_t i = m * thread_count + n;
						if (task_heads[i].load(std::memory_order_acquire) != nullptr) {
							return std::make_pair(i, n);
						}
					}
				}
			}

			return std::make_pair(~size_t(0), ~size_t(0));
		}

		// poll with given priority
		bool poll_one_internal(size_t priority_size) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			std::pair<size_t, size_t> slot = fetch(priority_size);
			size_t index = slot.first;

			if (index != ~size_t(0)) {
				size_t priority = slot.second;
				std::atomic<task_base_t*>& task_head = task_heads[index];
				if (task_head.load(std::memory_order_acquire) != nullptr) {
					// fetch a task atomically
					task_base_t* task = task_head.exchange(nullptr, std::memory_order_acquire);
					if (task != nullptr) {
						task_base_t* org = task_head.exchange(task->next, std::memory_order_release);

						// return the remaining
						if (org != nullptr) {
							do {
								task_base_t* next = org->next;

								// avoid legacy compiler bugs
								// see https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
								task_base_t* node = task_head.load(std::memory_order_relaxed);
								do {
									org->next = node;
								} while (!task_head.compare_exchange_weak(node, org, std::memory_order_relaxed, std::memory_order_relaxed));

								org = next;
							} while (org != nullptr);

							std::atomic_thread_fence(std::memory_order_acq_rel);
							wakeup_one_with_priority(priority);
						} else {
							IRIS_ASSERT(!threads.empty());
							if (waiting_thread_count.load(std::memory_order_seq_cst) > priority + limit_count) {
								if (fetch(priority_size).first != ~size_t(0)) {
									wakeup_one_with_priority(priority);
								}
							}
						}

						// in case task->task() throws exceptions
						execute_task(task);
					}
				}

				return true;
			} else {
				return false;
			}
		}

	protected:
		// read-mostly / immutable control block
		size_t& (*proxy_get_current_thread_index)();
		std::vector<thread_t> threads; // worker
		std::vector<std::atomic<task_base_t*>> task_heads; // task pointer list
		size_t internal_thread_count; // the count of internal thread
		size_t priority_task_threshold;
		std::function<bool(task_base_t*, size_t&)> priority_task_handler;
		std::function<void(size_t)> thread_pinner; // optional core-pinning hook, invoked with the slot index at internal thread entry
		std::atomic<size_t> terminated; // is to terminate

		// hot, independently updated counters
		alignas(default_cache_line_size) std::atomic<size_t> running_count; // running_count
		alignas(default_cache_line_size) std::atomic<size_t> task_count; // the count of total waiting tasks

		// task allocators (sharded, written on every alloc/free)
		// Started on a fresh cache line so allocator bookkeeping never collides with
		// the counters above.
		alignas(default_cache_line_size) large_task_allocator_t large_task_allocator;
		task_allocator_t task_allocators[sub_allocator_count]; // default task allocator

		// idle-wait slow path + fields mutated around the wake protocol
#if IRIS_ATOMIC_WAIT
		// futex-style park word: waiters sleep on wait(epoch), wakers bump + notify.
		alignas(default_cache_line_size) std::atomic<size_t> wake_epoch;
#endif
		alignas(default_cache_line_size) std::mutex mutex; // legacy wake path; also the timed poll_one() deadline wait
		std::condition_variable condition;
		alignas(default_cache_line_size) std::atomic<size_t> waiting_thread_count; // threads registered in delay()
		alignas(default_cache_line_size) std::atomic<size_t> timed_waiters; // threads sleeping in the timed poll_one()
		size_t limit_count; // limit the count of concurrently running thread
	};

	template <typename async_worker_t>
	struct iris_async_balancer_t {
		iris_async_balancer_t(async_worker_t& worker, size_t size = 4u) : async_worker(worker), current_limit(0), window_size(static_cast<ptrdiff_t>(size)) {
			async_worker.limit(current_limit);
			balance.store(0, std::memory_order_release);
		}

		void down() noexcept {
			if (current_limit + 1 < async_worker.get_thread_count() && async_worker.get_task_count() == 0) {
				ptrdiff_t size = balance.load(std::memory_order_acquire);
				if (size + window_size < 0) {
					async_worker.limit(++current_limit);
					balance.fetch_add(window_size, std::memory_order_relaxed);
				} else {
					balance.fetch_sub(1, std::memory_order_relaxed);
				}
			}
		}

		void up() noexcept {
			if (current_limit != 0 && async_worker.get_task_count() > 0) {
				ptrdiff_t size = balance.load(std::memory_order_acquire);
				if (size > window_size) {
					async_worker.limit(--current_limit);
					balance.fetch_sub(window_size, std::memory_order_relaxed);
				} else {
					balance.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

	private:
		async_worker_t& async_worker;
		size_t current_limit;
		ptrdiff_t window_size;
		std::atomic<ptrdiff_t> balance;
	};
}
