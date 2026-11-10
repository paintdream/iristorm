#include "../src/iris_coroutine.h"
#include "../src/iris_common.inl"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace iris;

using worker_t = iris_async_worker_t<>;
using warp_t = iris_warp_t<worker_t>;
using barrier_t = iris_barrier_t<void, bool, worker_t>;
using barrier_warp_t = iris_barrier_t<warp_t>; // worker_t implicit deduced from warp_t
using coroutine_t = iris_coroutine_t<>;
using coroutine_int_t = iris_coroutine_t<int>;
using quota_t = iris_quota_t<int, 2>;
using quota_queue_t = iris_quota_queue_t<quota_t, warp_t>;
static std::atomic<size_t> pending_count = 0;

static int run_demo_checks(void);
template <typename fn_t>
static bool run_with_timeout(const char* name, std::chrono::milliseconds ms, fn_t&& fn);
static void test_barrier_race();
static void test_barrier_frame_lifetime();
static void test_quota_lost_wakeup();
static void test_pipe_spsc_race();
static void test_dispatcher_next_handle();
static void test_warp_suspend_resume_storm();
static void test_event_cycle();
static void test_inline_completion_chains();
static void test_coroutine_completion_lifetime();
static void test_coroutine_warp_context();
static void test_coroutine_completion_race();

static coroutine_t cascade(warp_t* warp) {
	warp_t* w = co_await iris_switch(warp);
	printf("Cascaded!\n");
	co_await iris_switch(w);
}

static coroutine_int_t cascade_ret(warp_t* warp) {
	warp_t* w = co_await iris_switch(warp);
	printf("Cascaded int!\n");
	co_await iris_switch(w);
	co_return 1234;
}

static coroutine_t example(warp_t::async_worker_t& async_worker, warp_t* warp, warp_t* warp_end, int value) {
	if (warp != nullptr) {
		IRIS_ASSERT(warp_end - warp >= 3);
		warp_t* current = co_await iris_switch(warp);
		printf("Switch to warp %p\n", warp);
		co_await iris_switch<warp_t>(nullptr);
		printf("Detached\n");
		co_await iris_switch(warp);
		printf("Attached\n");
		co_await iris_switch(current);
		IRIS_ASSERT(current == warp_t::get_current());

		// randomly select warp
		IRIS_ASSERT(warp_end != nullptr);
		co_await iris_switch<warp_t>(nullptr); // iris_select requires warp_t::get_current() == nullptr
		warp_t* selected = co_await iris_select(warp, warp_end);
		printf("Select warp: %d\n", iris_verify_cast<int>(selected - warp));

		IRIS_ASSERT(warp_end - warp > 1);
		if (warp == selected) {
			co_await iris_switch(warp + 1, warp + 2);
		} else {
			co_await iris_switch(warp, warp + 1);
		}

		printf("Paired!\n");
		co_await iris_switch(current);
	}
	pending_count.fetch_sub(1, std::memory_order_release);

	co_await cascade(warp);
	int cascade_int_result = co_await cascade_ret(warp);
	pending_count.fetch_sub(1, std::memory_order_release);

	// Step 1: test single await
	co_await iris_awaitable(warp, []() {});
	int v = co_await iris_awaitable(warp, [value]() { return value; });
	warp_t* current = warp_t::get_current();
	printf("Value: %d %p\n", v, warp_t::get_current());

	// Step 2: test multiple await by incrementally construction
	std::function<void()> v1 = [value]() {};
	std::function<void()> v2 = [value]() {};
	std::function<void()> v3 = [value]() {};
	pending_count.fetch_sub(1, std::memory_order_release);

	// Step 3: test multiple await by poll-like construction
	if (warp == nullptr) {
		auto w2 = iris_awaitable(warp, std::move(v2));
		auto w3 = iris_awaitable(warp, std::move(v3));
		w2.dispatch();

		co_await w2;
		co_await w3;
	} else {
		co_await iris_awaitable_parallel(warp, []() {});

		auto w2 = iris_awaitable_parallel(warp, std::move(v2));
		auto w3 = iris_awaitable_parallel(warp, std::move(v3), 1);

		w3.dispatch();

		co_await w2;
		co_await w3;
	}

	pending_count.fetch_sub(1, std::memory_order_release);

	if (warp != nullptr) {
		warp_t* current = co_await iris_switch(warp);
		printf("Another switch to warp %p\n", warp);
		co_await iris_switch(current);
		IRIS_ASSERT(current == warp_t::get_current());
		printf("I'm back %d\n", (int)pending_count.load(std::memory_order_acquire));
	}

	// if all tests finished, terminate the thread pool and exit the program
	if (pending_count.fetch_sub(1, std::memory_order_release) == 1) {
		async_worker.terminate();
	}
}

static coroutine_int_t example_empty() {
	printf("Empty finished!\n");
	co_return 1;
}

template <typename barrier_type_t>
static coroutine_t example_barrier(warp_t::async_worker_t& async_worker, barrier_type_t& barrier, int index) {
	printf("Example barrier %d begin running!\n", index);

	co_await barrier;
	std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 50));
	printf("Example barrier %d mid running!\n", index);

	co_await barrier;
	std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 50));
	printf("Example barrier %d end running!\n", index);

	if (pending_count.fetch_sub(1, std::memory_order_release) == 1) {
		async_worker.terminate();
	}
}

struct dispatcher_t : iris_dispatcher_t<warp_t, dispatcher_t> {
	dispatcher_t(iris_async_worker_t<>& w) : iris_dispatcher_t<warp_t, dispatcher_t>(w) {}
	void dispatcher_complete() {
		async_worker.terminate();
	}

	void dispatcher_resurrect() {}
	void dispatcher_cleanup() {}
	void dispatcher_enter_execute(const routine_handle_t&) {}
	void dispatcher_leave_execute(const routine_handle_t&) {}
};

static void example_dispatch_coroutine(worker_t& worker) {
	std::atomic<bool> finished = false;
	dispatcher_t dispatcher(worker);
	std::atomic<bool> finished2 = false;

	auto pre_node = dispatcher.allocate(warp_t::get_current(), [](const auto& handle) {
		printf("example_from_coroutine 0000!\n");
	});

	auto post_node = dispatcher.allocate(warp_t::get_current(), [&finished2, &finished](const auto& handle) {
		IRIS_ASSERT(finished.load(std::memory_order_acquire));
		printf("example_from_coroutine 2222!\n");

		finished2.store(true, std::memory_order_release);
	});

	auto coro_node = iris_dispatch_coroutine(dispatcher, [](dispatcher_t& dispatcher, std::atomic<bool>& finished) -> coroutine_t {
		printf("example_from_coroutine 1111!\n");
		co_await iris_awaitable(warp_t::get_current(), []() {});
		finished.store(true, std::memory_order_release);
		co_return;
	}(dispatcher, finished));
	dispatcher.order(pre_node, coro_node);
	dispatcher.order(coro_node, post_node);
	dispatcher.dispatch(std::move(coro_node));
	dispatcher.dispatch(std::move(post_node));
	dispatcher.dispatch(std::move(pre_node));

	while (!finished2.load(std::memory_order_acquire) || dispatcher.get_pending_count() != 0) {
		worker.poll_one(0, std::chrono::milliseconds(50));
	}
}

static coroutine_t example_listen(dispatcher_t& dispatcher) {
	auto prev = dispatcher.allocate(nullptr, [](const auto& handle) {
		printf("prev task!");
	});

	co_await iris_coroutine_dispatch(dispatcher, std::move(prev));
	printf("next task!");

	if (pending_count.fetch_sub(1, std::memory_order_release) == 1) {
		dispatcher.get_async_worker().terminate();
	}
}

static coroutine_t example_quota(quota_queue_t& q) {
	{
		auto guard = co_await q.guard({ 1, 3 });
		std::array<int, 2> req = { 2, 2 };
		bool b = q.acquire(req);
		IRIS_ASSERT(b);
		q.get_async_worker().queue([&q, req]() mutable {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			printf("Release quota holder!\n");
			q.release(req);
		});
	}

	auto guard2 = co_await q.guard({ 3, 4 });
	guard2.acquire({ 1, 1 });
	guard2.release({ 1, 1 });
	guard2.clear();
	printf("Acquire quota holder!\n");
	auto guard3 = co_await q.guard({ 1, 1 });
	guard3 = std::move(guard2);

	if (pending_count.fetch_sub(1, std::memory_order_release) == 1) {
		q.get_async_worker().terminate();
	}
}

// functional demo, originally the whole file's main
static int run_demo_checks(void) {
	static constexpr size_t thread_count = 8;
	static constexpr size_t warp_count = 16;

	iris_async_worker_t<> worker(thread_count);
	size_t main_thread_index = worker.append(std::thread());
	worker.start();
	pending_count.fetch_add(1, std::memory_order_release);
	pending_count.fetch_add(1, std::memory_order_release);

	example_dispatch_coroutine(worker);

	dispatcher_t dispatcher(worker);
	example_listen(dispatcher).run();
	quota_t quota({ 4, 5 });

	pending_count.fetch_add(1, std::memory_order_release);
	quota_queue_t quota_queue(worker, quota);
	example_quota(quota_queue).run();

	std::vector<warp_t> warps;
	warps.reserve(warp_count);
	for (size_t i = 0; i < warp_count; i++) {
		warps.emplace_back(worker);
	}

	// test for barrier
	pending_count.fetch_add(8, std::memory_order_release);

	barrier_t barrier(worker, 4);
	example_barrier(worker, barrier, 0).run();
	example_barrier(worker, barrier, 1).run();
	example_barrier(worker, barrier, 2).run();
	example_barrier(worker, barrier, 3).run();

	barrier_warp_t barrier_warp(worker, 4);
	warps[0].queue_routine_post([&worker, &barrier_warp]() {
		example_barrier(worker, barrier_warp, 5).run();
		example_barrier(worker, barrier_warp, 6).run();
		example_barrier(worker, barrier_warp, 7).run();
		example_barrier(worker, barrier_warp, 8).run();
	});

	// initialize pending count with `example` call count
	pending_count.fetch_add(30, std::memory_order_acq_rel);

	// test for running example from an external thread
	example(worker, &warps[0], &warps[3], 1).then([]() {
		printf("Complete!\n");
	}).run();

	example_empty().then([](int value) {
		printf("Complete empty %d!\n", value);
	}).run();

	example_empty().run();
	example(worker, nullptr, nullptr, 2).run();

	warps[0].queue_routine_post([&worker, &warps]() {
		// test for running example from an warp
		example(worker, &warps[0], &warps[3], 3).run();
		example(worker, nullptr, nullptr, 4).run();
	});

	// test for running example from thread pool
	worker.queue([&worker, &warps]() {
		// test for running example from an warp
		example(worker, &warps[0], &warps[3], 5).run();
		example(worker, nullptr, nullptr, 6).run();
	});

	if (pending_count.fetch_sub(1, std::memory_order_release) == 1) {
		worker.terminate();
	}

	worker.thread_loop(main_thread_index);
	worker.join();

	// finished!
	while (warp_t::poll(warps.begin(), warps.end())) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	return 0;
}

// stress tests, was iris_coroutine_stress.cpp

// watchdog helper: run fn in a detached worker, fail if it exceeds the deadline
template <typename fn_t>
static bool run_with_timeout(const char* name, std::chrono::milliseconds ms, fn_t&& fn) {
	std::promise<void> done;
	std::future<void> fut = done.get_future();
	std::thread([&]() {
		try {
			fn();
			done.set_value();
		} catch (...) {
			done.set_exception(std::current_exception());
		}
	}).detach();

	auto status = fut.wait_for(ms);
	if (status == std::future_status::ready) {
		try {
			fut.get();
		} catch (const std::exception& e) {
			printf("[FAIL] %s: %s\n", name, e.what());
			return false;
		} catch (...) {
			printf("[FAIL] %s: unknown exception\n", name);
			return false;
		}
		printf("[PASS] %s\n", name);
		return true;
	}
	printf("[FAIL/HANG] %s (exceeded %lld ms)\n", name, (long long)ms.count());
	return false;
}

struct coroutine_stack_sample_t {
	std::uintptr_t low = 0;
	std::uintptr_t high = 0;
	size_t count = 0;
	size_t fiber_errors = 0;

	size_t span() const { return static_cast<size_t>(high - low); }
};

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static void sample_coroutine_stack(coroutine_stack_sample_t& sample) {
	volatile unsigned char marker = 0;
	auto address = reinterpret_cast<std::uintptr_t>(&marker);
	if (sample.count++ == 0) {
		sample.low = sample.high = address;
	} else {
		sample.low = std::min(sample.low, address);
		sample.high = std::max(sample.high, address);
	}
}

static coroutine_t inline_child(coroutine_stack_sample_t& sample) {
	sample_coroutine_stack(sample);
	co_return;
}

static coroutine_t inline_completion_chain(coroutine_stack_sample_t& sample, size_t count, bool task) {
	for (size_t i = 0; i < count; i++) {
		if (task) {
			co_await iris_awaitable(warp_t::get_current(), [&sample]() { sample_coroutine_stack(sample); });
		} else {
			co_await inline_child(sample).named("test::inline_child");
		}
		const char* fiber = IRIS_PROFILE_COROUTINE_CURRENT();
		if (fiber != nullptr && std::strcmp(fiber, "test::inline_chain") != 0) {
			sample.fiber_errors++;
		}
	}
}

static void test_inline_completion_chains() {
	auto check = []() {
		for (bool task : { false, true }) {
			auto measure = [task](size_t count) {
				coroutine_stack_sample_t sample;
				bool done = false;
				inline_completion_chain(sample, count, task).named("test::inline_chain").then([&done]() { done = true; }).run();
				if (!done || sample.count != count) {
					throw std::runtime_error("synchronous chain unexpectedly suspended");
				}
				if (sample.fiber_errors != 0) {
					throw std::runtime_error("synchronous chain changed its profiler fiber");
				}
				return sample.span();
			};
			size_t short_span = measure(32);
			size_t long_span = measure(256);
			printf("  %s stack span: 32=%zu, 256=%zu\n", task ? "awaitable" : "coroutine", short_span, long_span);
			// check the bound before the large case can overflow an unfixed build
			if (long_span > short_span + 16 * 1024 || long_span > 64 * 1024) {
				throw std::runtime_error("synchronous completion stack grows with chain length");
			}
			if (measure(20000) > 64 * 1024) {
				throw std::runtime_error("long synchronous completion chain exceeded stack budget");
			}
		}
	};
	check();
	worker_t worker;
	worker.append(std::thread());
	warp_t warp(worker);
	warp.queue_routine(check);
}

struct coroutine_local_guard_t {
	bool& alive;
	explicit coroutine_local_guard_t(bool& flag) : alive(flag) { alive = true; }
	~coroutine_local_guard_t() { alive = false; }
};

struct coroutine_move_value_t {
	std::unique_ptr<int> value;
	explicit coroutine_move_value_t(int v) : value(std::make_unique<int>(v)) {}
	coroutine_move_value_t(coroutine_move_value_t&&) = default;
	coroutine_move_value_t(const coroutine_move_value_t&) = delete;
};

static iris_coroutine_t<coroutine_move_value_t> guarded_child(bool& alive, int value) {
	coroutine_local_guard_t guard(alive);
	co_return coroutine_move_value_t(value);
}

static coroutine_t guarded_completion_chain(bool& alive, size_t& errors) {
	for (int i = 0; i < 20000; i++) {
		auto value = co_await guarded_child(alive, i);
		if (alive || !value.value || *value.value != i) {
			errors++;
		}
	}
}

static void test_coroutine_completion_lifetime() {
	bool alive = false;
	bool done = false;
	size_t errors = 0;
	guarded_completion_chain(alive, errors).then([&done]() { done = true; }).run();
	if (!done || alive || errors != 0) {
		throw std::runtime_error("synchronous co_await result/local lifetime mismatch");
	}
	guarded_child(alive, 42).complete([&](void* address, coroutine_move_value_t&& value) {
		if (!address || !alive || !value.value || *value.value != 42) {
			errors++;
		}
	}).run();
	guarded_child(alive, 43).then([&](coroutine_move_value_t value) {
		if (!alive || !value.value || *value.value != 43) {
			errors++;
		}
	}).run();
	if (alive || errors != 0) {
		throw std::runtime_error("user completion no longer runs before local destruction");
	}
}

static bool warp_is_held(warp_t& warp) {
	bool acquired = false;
	std::thread([&]() {
		warp_t::preempt_guard_t guard(warp, 0);
		acquired = static_cast<bool>(guard);
	}).join();
	return !acquired;
}

static coroutine_t nested_warp_child(warp_t& warp, coroutine_stack_sample_t& sample, size_t& errors) {
	co_await inline_child(sample);
	if (warp_t::get_current() != &warp || !warp_is_held(warp)) {
		errors++;
	}
}

static coroutine_t nested_warp_chain(warp_t& a, warp_t& b, coroutine_stack_sample_t& sample, bool& b_done, size_t& errors) {
	for (size_t i = 0; i < 32; i++) {
		co_await inline_child(sample);
		if (i == 5) {
			// anchors the downstream depth-6 TLS trampoline's cross-warp regression
			b.queue_routine([&]() {
				nested_warp_child(b, sample, errors).then([&b_done]() { b_done = true; }).run();
			});
		}
		if (warp_t::get_current() != &a) {
			errors++;
		}
	}
}

static coroutine_t posted_warp_flow(warp_t& a, warp_t& b, coroutine_stack_sample_t& sample, size_t& errors) {
	co_await iris_switch(&a);
	co_await iris_switch(&a);
	co_await iris_switch(&a, &b);
	warp_t* paired = warp_t::get_current();
	co_await inline_child(sample);
	co_await iris_awaitable(paired, []() {});
	if ((paired != &a && paired != &b) || warp_t::get_current() != paired || !warp_is_held(a) || !warp_is_held(b)) {
		errors++;
	}
	co_await iris_switch(&a);
	co_await iris_awaitable(&b, [&]() {
		if (warp_t::get_current() != &b) {
			errors++;
		}
	});
	if (warp_t::get_current() != &a) {
		errors++;
	}
	auto predispatched = iris_awaitable(&b, [&]() {
		if (warp_t::get_current() != &b) {
			errors++;
		}
	});
	predispatched.dispatch();
	co_await predispatched;
	if (warp_t::get_current() != &a) {
		errors++;
	}
	co_await iris_switch<warp_t>(nullptr);
	co_await iris_awaitable(&b, [&]() {
		if (warp_t::get_current() != &b) {
			errors++;
		}
	});
	if (warp_t::get_current() != nullptr) {
		errors++;
	}
}

static coroutine_t queued_awaitable_flow(warp_t& target, warp_t* caller, size_t& work, size_t& errors) {
	co_await iris_awaitable(&target, [&]() {
		work++;
		if (warp_t::get_current() != &target) {
			errors++;
		}
	});
	if (warp_t::get_current() != caller) {
		errors++;
	}
}

static void test_coroutine_warp_context() {
	worker_t worker;
	worker.append(std::thread());
	worker.start();
	worker.make_current(0);
	warp_t a(worker);
	warp_t b(worker);
	coroutine_stack_sample_t sample;
	size_t errors = 0;
	bool a_done = false;
	bool b_done = false;
	a.queue_routine([&]() {
		nested_warp_chain(a, b, sample, b_done, errors).then([&a_done]() { a_done = true; }).run();
	});
	if (!a_done || !b_done || errors != 0 || warp_t::get_current() != nullptr) {
		throw std::runtime_error("inline continuation escaped its warp acquisition");
	}
	bool posted_done = false;
	posted_warp_flow(a, b, sample, errors).then([&posted_done]() { posted_done = true; }).run();
	if (posted_done) {
		throw std::runtime_error("cross-warp switch unexpectedly ran inline");
	}
	while (!posted_done) {
		if (!worker.poll_one()) {
			throw std::runtime_error("posted warp continuation lost");
		}
	}
	size_t work = 0;
	bool warp_done = false;
	bool pool_done = false;
	queued_awaitable_flow(b, nullptr, work, errors).then([&pool_done]() { pool_done = true; }).run();
	a.queue_routine([&]() {
		queued_awaitable_flow(b, &a, work, errors).then([&warp_done]() { warp_done = true; }).run();
	});
	if (work != 0 || warp_done || pool_done) {
		throw std::runtime_error("different-warp awaitable bypassed queue/post");
	}
	while (!warp_done || !pool_done) {
		if (!worker.poll_one()) {
			throw std::runtime_error("queued awaitable return lost");
		}
	}
	if (work != 2) {
		throw std::runtime_error("queued awaitable executed more than once");
	}
	worker.terminate();
	worker.join();
	worker.make_current(~size_t(0));
	if (errors != 0) {
		throw std::runtime_error("warp-pair/queued-return scheduling contract violated");
	}
}

struct coroutine_thread_resume_t {
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) const {
		// force completion on another thread before the parent's run() returns
		std::thread([handle]() { handle.resume(); }).join();
	}
	void await_resume() const noexcept {}
};

struct coroutine_queued_resume_t {
	worker_t& worker;
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) const {
		worker.queue([handle]() { handle.resume(); });
	}
	void await_resume() const noexcept {}
};

static iris_coroutine_t<coroutine_move_value_t> concurrent_child(worker_t* worker, int value) {
	if (worker == nullptr) {
		co_await coroutine_thread_resume_t();
	} else {
		co_await coroutine_queued_resume_t{ *worker };
	}
	co_return coroutine_move_value_t(value);
}

static coroutine_t concurrent_void_child(worker_t* worker, size_t& errors) {
	auto value = co_await concurrent_child(worker, 42);
	if (!value.value || *value.value != 42) {
		errors++;
	}
}

static coroutine_t concurrent_completion_chain(worker_t* worker, size_t count, size_t& errors) {
	for (size_t i = 0; i < count; i++) {
		auto value = co_await concurrent_child(worker, static_cast<int>(i));
		if (!value.value || *value.value != static_cast<int>(i) || warp_t::get_current() != nullptr) {
			errors++;
		}
		co_await concurrent_void_child(worker, errors);
	}
}

static void test_coroutine_completion_race() {
	size_t errors = 0;
	bool early_done = false;
	concurrent_completion_chain(nullptr, 32, errors).then([&early_done]() { early_done = true; }).run();
	if (!early_done || errors != 0) {
		throw std::runtime_error("completion before suspension lost its owned result");
	}
	worker_t worker(4);
	worker.start();
	std::atomic<bool> done{ false };
	concurrent_completion_chain(&worker, 20000, errors).then([&done]() { done.store(true, std::memory_order_release); }).run();
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	worker.terminate();
	worker.join();
	if (!done.load(std::memory_order_acquire) || errors != 0) {
		throw std::runtime_error("completion/suspension race lost or corrupted a continuation");
	}
}

// Test 1: iris_barrier_t - race between concurrent await_suspend writers and complete():
// the last writer may not have stored its handle slot yet when complete() iterates it.
// launch from many threads and rounds to make the interleaving likely.
// helper as a regular function: GCC 12 lambda coroutines copy awaitables into the frame.
template <typename barrier_t>
static coroutine_t barrier_worker(barrier_t& barrier,
                                   std::atomic<size_t>& launched,
                                   std::atomic<size_t>& resumed) {
	launched.fetch_add(1, std::memory_order_release);
	co_await barrier;
	resumed.fetch_add(1, std::memory_order_release);
}

static void test_barrier_race() {
	using barrier_t = iris_barrier_t<void, bool, worker_t>;

	const size_t participants = 16;
	const size_t rounds = 40;

	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	for (size_t r = 0; r < rounds; r++) {
		barrier_t barrier(worker, participants);
		std::atomic<size_t> resumed{ 0 };
		std::atomic<size_t> launched{ 0 };

		// launch from many threads to maximize fetch_add interleaving
		std::vector<std::thread> ts;
		ts.reserve(participants);
		std::atomic<bool> go{ false };
		for (size_t i = 0; i < participants; i++) {
			ts.emplace_back([&]() {
				while (!go.load(std::memory_order_acquire)) { std::this_thread::yield(); }
				barrier_worker(barrier, launched, resumed).run();
			});
		}
		go.store(true, std::memory_order_release);
		for (auto& t : ts) t.join();

		// wait until all are launched, run() returns after initial suspend
		while (launched.load(std::memory_order_acquire) != participants) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		// all participants should eventually be resumed
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (resumed.load(std::memory_order_acquire) != participants) {
			if (std::chrono::steady_clock::now() > deadline) {
				printf("  round %zu: only %zu/%zu coroutines resumed\n",
					r, resumed.load(std::memory_order_acquire), participants);
				worker.terminate();
				worker.join();
				throw std::runtime_error("barrier stuck");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	worker.terminate();
	worker.join();
}

// Test 2: iris_quota_queue_t - lost wakeup race: release() can iterate handles
// before a blocked guard() push lands, so the wakeup never comes.
static void test_quota_lost_wakeup() {
	using quota_t = iris_quota_t<int, 1>;
	using quota_queue_t = iris_quota_queue_t<quota_t, void, worker_t>;

	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	quota_t quota({ 4 }); // total 4 units
	quota_queue_t qq(worker, quota);

	const size_t count = 200;
	std::atomic<size_t> finished{ 0 };

	auto co = [&]() -> coroutine_t {
		// each acquires 1 unit then releases via RAII
		auto g = co_await qq.guard({ 1 });
		// hold briefly to force contention
		std::this_thread::sleep_for(std::chrono::microseconds(50));
		(void)g;
		finished.fetch_add(1, std::memory_order_release);
	};

	for (size_t i = 0; i < count; i++) co().run();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (finished.load(std::memory_order_acquire) != count) {
		if (std::chrono::steady_clock::now() > deadline) {
			printf("  finished=%zu/%zu (quota lost wakeup)\n",
				finished.load(std::memory_order_acquire), count);
			worker.terminate();
			worker.join();
			throw std::runtime_error("quota stuck");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	worker.terminate();
	worker.join();
}

// Test 3: iris_pipe_t - SPSC race with default iris_no_mutex_t: push and await
// interleave freely on the worker pool without the rendezvous re-check.
// helper as a regular function: GCC 12 lambda coroutines copy awaitables into the frame.
template <typename pipe_t>
static coroutine_t pipe_consumer(pipe_t& pipe,
                                  std::atomic<int>& consumed,
                                  int count) {
	for (int i = 0; i < count; i++) {
		int v = co_await pipe;
		(void)v;
		consumed.fetch_add(1, std::memory_order_release);
	}
}

static void test_pipe_spsc_race() {
	using pipe_t = iris_pipe_t<int, void, worker_t>; // mutex_t defaults to iris_no_mutex_t

	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	pipe_t pipe(worker);
	const int count = 500;
	std::atomic<int> consumed{ 0 };

	auto producer = [&]() -> coroutine_t {
		for (int i = 0; i < count; i++) {
			pipe.emplace(i);
			// no awaiting - just keep posting
			co_await iris_awaitable(static_cast<warp_t*>(nullptr),
				[]() {}); // yield back to pool
		}
	};

	pipe_consumer(pipe, consumed, count).run();
	producer().run();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (consumed.load(std::memory_order_acquire) != count) {
		if (std::chrono::steady_clock::now() > deadline) {
			printf("  consumed=%d/%d (pipe lost wakeup)\n",
				consumed.load(std::memory_order_acquire), count);
			worker.terminate();
			worker.join();
			throw std::runtime_error("pipe stuck");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	worker.terminate();
	worker.join();
}

// Test 4: iris_dispatcher_t::next() - removed upstream, kept as a compile-time
// guard against a misnamed next() overload
static void test_dispatcher_next_handle() {
	worker_t worker(2);
	worker.start();
	{
		iris_dispatcher_t<warp_t> d(worker);
		auto h = d.allocate(nullptr, [](const auto&) {});
		// `next()` is gone - drive the routine to completion via dispatch()
		// to prove the dispatcher still pumps pending_count back to 0.
		d.dispatch(std::move(h));

		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (d.get_pending_count() != 0) {
			if (std::chrono::steady_clock::now() > deadline) {
				printf("  pending_count never drained\n");
				worker.terminate();
				worker.join();
				throw std::runtime_error("dispatch stuck");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	worker.terminate();
	worker.join();
}

// Test 5: iris_warp_t - concurrent suspend / resume / queue_routine_post
static void test_warp_suspend_resume_storm() {
	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	const size_t warp_count = 4;
	std::vector<warp_t> warps;
	warps.reserve(warp_count);
	for (size_t i = 0; i < warp_count; i++) warps.emplace_back(worker);

	const size_t task_count = 2000;
	std::atomic<size_t> done{ 0 };

	std::atomic<bool> stop{ false };
	std::thread agitator([&]() {
		std::mt19937 rng(123);
		while (!stop.load(std::memory_order_acquire)) {
			warp_t& w = warps[rng() % warp_count];
			w.suspend();
			std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
			w.resume();
		}
	});

	for (size_t i = 0; i < task_count; i++) {
		warps[i % warp_count].queue_routine_post([&done]() {
			done.fetch_add(1, std::memory_order_release);
		});
	}

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (done.load(std::memory_order_acquire) != task_count) {
		if (std::chrono::steady_clock::now() > deadline) {
			printf("  done=%zu/%zu (warp starvation)\n",
				done.load(std::memory_order_acquire), task_count);
			stop.store(true, std::memory_order_release);
			agitator.join();
			worker.terminate();
			worker.join();
			while (warp_t::poll(warps.begin(), warps.end())) {}
			throw std::runtime_error("warp stuck");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	stop.store(true, std::memory_order_release);
	agitator.join();
	worker.terminate();
	worker.join();
	while (warp_t::poll(warps.begin(), warps.end())) {}
}

// Test 5b: iris_warp_t suspend/resume queueing contract, deterministic part.
//   * tasks posted while nested-suspended must not run before the FINAL resume
//   * the final resume must drain the whole backlog
//   * a post landing while the warp is parked in `pending` (the parked
//     execute_t skipped, queueing left pending) must not be swallowed
//   * afterwards the warp must accept fresh work at once (terminal-state
//     liveness: queueing back to idle, nothing stranded in the buffers)
static bool warp_wait_for(std::chrono::milliseconds timeout, const std::function<bool()>& pred) {
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!pred()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

static void test_warp_suspend_backlog() {
	worker_t worker(2);
	worker.start();
	warp_t warp(worker);
	const size_t count = 100;
	std::atomic<size_t> done{ 0 };

	// nested suspend: only the resume that brings suspend_count to zero
	// is allowed to (re-)start execution
	warp.suspend();
	warp.suspend();
	for (size_t i = 0; i < count; i++) {
		warp.queue_routine_post([&done]() {
			done.fetch_add(1, std::memory_order_release);
		});
	}

	// non-final resume: nothing may run (the posted backlog stays parked)
	warp.resume();
	if (!warp_wait_for(std::chrono::milliseconds(100), [&done]() { return false; }) &&
		done.load(std::memory_order_acquire) != 0) {
		throw std::runtime_error("non-final resume executed parked tasks");
	}

	// while parked in pending, an extra post must not be swallowed
	warp.queue_routine_post([&done]() {
		done.fetch_add(1, std::memory_order_release);
	});

	// final resume: the whole backlog (count + 1) must drain
	warp.resume();
	if (!warp_wait_for(std::chrono::seconds(2), [&done, count]() {
		return done.load(std::memory_order_acquire) == count + 1;
	})) {
		printf("  backlog drained %zu/%zu\n", done.load(std::memory_order_acquire), count + 1);
		throw std::runtime_error("suspend backlog lost tasks on final resume");
	}

	// terminal-state liveness probe: a fresh task must run immediately;
	// a warp stranded in pending/executing with no executor swallows it
	std::atomic<bool> probed{ false };
	warp.queue_routine_post([&probed]() { probed.store(true, std::memory_order_release); });
	if (!warp_wait_for(std::chrono::milliseconds(500), [&probed]() { return probed.load(std::memory_order_acquire); })) {
		throw std::runtime_error("warp lost terminal liveness after backlog drain");
	}

	worker.terminate();
	worker.join();
	while (warp_t::poll({ std::ref(warp) })) {}
}

// Test 5c: iris_warp_t interrupt/drain race, targeted regression for the
// lost wakeup fixed in "Re-publish warp queue as pending when execution is
// interrupted by suspend": execute_internal() early-returns between tasks
// when an external suspend() lands on a check point while the buffers still
// hold work; if the final resume() slips into the window before the
// executor's yield(), neither side re-flushed and the remaining tasks
// starved forever. The storm test above hits it with ~5% per run; here the
// agitator is dense (1us period) and every round ends with a liveness probe
// so the stale-pending state is caught within milliseconds instead of a
// 5s deadline.
static void test_warp_interrupt_drain() {
	const size_t rounds = 20;
	const size_t warp_count = 4;
	const size_t count_per_warp = 500;

	for (size_t r = 0; r < rounds; r++) {
		worker_t worker(4);
		worker.start();
		std::vector<warp_t> warps;
		warps.reserve(warp_count);
		for (size_t i = 0; i < warp_count; i++) warps.emplace_back(worker);

		const size_t total = warp_count * count_per_warp;
		std::atomic<size_t> done{ 0 };
		std::atomic<bool> stop{ false };

		std::thread agitator([&]() {
			std::mt19937 rng(123);
			while (!stop.load(std::memory_order_acquire)) {
				warp_t& w = warps[rng() % warp_count];
				w.suspend();
				std::this_thread::sleep_for(std::chrono::microseconds(rng() % 10));
				w.resume();
			}
		});

		// rescue: a dense agitator can park an executor in `pending` for a
		// long time (starvation -- recoverable), while the lost wakeup leaves
		// the warp in `idle` with no executor at all (permanent). Stopping
		// the agitator and forcing one FINAL resume cycle flushes the former;
		// the latter stays dead, which is exactly what we report.
		auto rescue = [&]() {
			if (!stop.exchange(true, std::memory_order_acq_rel)) {
				agitator.join();
			}

			for (size_t i = 0; i < warp_count; i++) {
				warps[i].suspend();
				warps[i].resume();
			}
		};

		for (size_t i = 0; i < total; i++) {
			warps[i % warp_count].queue_routine_post([&done]() {
				done.fetch_add(1, std::memory_order_release);
			});
		}

		// phase 1: drain with stall detection
		size_t last_done = 0;
		auto last_progress = std::chrono::steady_clock::now();
		while (done.load(std::memory_order_acquire) != total) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			size_t d = done.load(std::memory_order_acquire);
			if (d != last_done) {
				last_done = d;
				last_progress = std::chrono::steady_clock::now();
			} else if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(1)) {
				rescue();
				if (!warp_wait_for(std::chrono::seconds(2), [&done, total]() {
					return done.load(std::memory_order_acquire) == total;
				})) {
					printf("  round %zu: stalled at %zu/%zu after rescue (lost wakeup)\n", r,
						done.load(std::memory_order_acquire), total);
					worker.terminate();
					worker.join();
					while (warp_t::poll(warps.begin(), warps.end())) {}
					throw std::runtime_error("interrupted drain lost its wakeup");
				}

				break;
			}
		}

		// phase 2: terminal liveness probe on every warp, with rescue
		std::atomic<size_t> probed{ 0 };
		for (size_t i = 0; i < warp_count; i++) {
			warps[i].queue_routine_post([&probed]() { probed.fetch_add(1, std::memory_order_release); });
		}
		if (!warp_wait_for(std::chrono::seconds(1), [&probed, warp_count]() {
			return probed.load(std::memory_order_acquire) == warp_count;
		})) {
			rescue();
			if (!warp_wait_for(std::chrono::seconds(1), [&probed, warp_count]() {
				return probed.load(std::memory_order_acquire) == warp_count;
			})) {
				printf("  round %zu: %zu/%zu probes ran after rescue\n", r,
					probed.load(std::memory_order_acquire), warp_count);
				worker.terminate();
				worker.join();
				while (warp_t::poll(warps.begin(), warps.end())) {}
				throw std::runtime_error("interrupted drain stranded a warp");
			}
		}

		if (!stop.exchange(true, std::memory_order_acq_rel)) {
			agitator.join();
		}

		worker.terminate();
		worker.join();
		while (warp_t::poll(warps.begin(), warps.end())) {}
	}
}

// Test 5d: iris_warp_t same race from the poster side: several worker
// threads push while an external agitator suspends/resumes, so the
// push-side flush() competes with resume()/yield() for the pending flag
// instead of a single external poster routing everything through the pool.
static void test_warp_concurrent_post_interrupt() {
	worker_t worker(4);
	worker.start();
	warp_t warp(worker);
	const size_t per_poster = 500;
	std::atomic<size_t> done{ 0 };
	std::atomic<bool> stop{ false };

	std::thread agitator([&]() {
		std::mt19937 rng(456);
		while (!stop.load(std::memory_order_acquire)) {
			warp.suspend();
			std::this_thread::sleep_for(std::chrono::microseconds(rng() % 10));
			warp.resume();
		}
	});

	// rescue (see test_warp_interrupt_drain): a final resume cycle after the
	// agitator stops recovers a merely starved warp (pending parked), but
	// cannot recover a lost wakeup (idle, no executor)
	auto rescue = [&]() {
		if (!stop.exchange(true, std::memory_order_acq_rel)) {
			agitator.join();
		}

		warp.suspend();
		warp.resume();
	};

	for (int p = 0; p < 2; p++) {
		worker.queue([&warp, &done, per_poster]() {
			for (size_t i = 0; i < per_poster; i++) {
				warp.queue_routine_post([&done]() {
					done.fetch_add(1, std::memory_order_release);
				});
			}
		});
	}

	// phase 1: drain with stall detection + rescue
	size_t last_done = 0;
	auto last_progress = std::chrono::steady_clock::now();
	while (done.load(std::memory_order_acquire) != 2 * per_poster) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		size_t d = done.load(std::memory_order_acquire);
		if (d != last_done) {
			last_done = d;
			last_progress = std::chrono::steady_clock::now();
		} else if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(1)) {
			rescue();
			if (!warp_wait_for(std::chrono::seconds(2), [&done, per_poster]() {
				return done.load(std::memory_order_acquire) == 2 * per_poster;
			})) {
				printf("  stalled at %zu/%zu after rescue (lost wakeup)\n",
					done.load(std::memory_order_acquire), 2 * per_poster);
				worker.terminate();
				worker.join();
				while (warp_t::poll({ std::ref(warp) })) {}
				throw std::runtime_error("concurrent post drain lost its wakeup");
			}

			break;
		}
	}

	// phase 2: terminal liveness probe with rescue
	std::atomic<bool> probed{ false };
	warp.queue_routine_post([&probed]() { probed.store(true, std::memory_order_release); });
	if (!warp_wait_for(std::chrono::seconds(1), [&probed]() { return probed.load(std::memory_order_acquire); })) {
		rescue();
		if (!warp_wait_for(std::chrono::seconds(1), [&probed]() { return probed.load(std::memory_order_acquire); })) {
			worker.terminate();
			worker.join();
			while (warp_t::poll({ std::ref(warp) })) {}
			throw std::runtime_error("concurrent post drain stranded the warp");
		}
	}

	if (!stop.exchange(true, std::memory_order_acq_rel)) {
		agitator.join();
	}

	worker.terminate();
	worker.join();
	while (warp_t::poll({ std::ref(warp) })) {}
}

// Test 6: iris_event_t - repeated notify/reset cycles with many waiters.
// helper as a regular function: GCC 12 lambda coroutines copy awaitables into the frame.
template <typename event_t>
static coroutine_t event_waiter(event_t& ev,
                                 std::atomic<size_t>& resumed) {
	co_await ev;
	resumed.fetch_add(1, std::memory_order_release);
}

static void test_event_cycle() {
	using event_t = iris_event_t<void, worker_t>;

	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	event_t ev(worker);
	const size_t rounds = 50;
	const size_t waiters_per_round = 16;
	std::atomic<size_t> resumed{ 0 };

	for (size_t r = 0; r < rounds; r++) {
		ev.reset();
		size_t base = resumed.load(std::memory_order_acquire);
		for (size_t i = 0; i < waiters_per_round; i++) event_waiter(ev, resumed).run();
		// give them a moment to suspend
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		ev.notify();

		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (resumed.load(std::memory_order_acquire) - base != waiters_per_round) {
			if (std::chrono::steady_clock::now() > deadline) {
				printf("  round %zu: only %zu/%zu resumed\n", r,
					resumed.load(std::memory_order_acquire) - base, waiters_per_round);
				worker.terminate();
				worker.join();
				throw std::runtime_error("event stuck");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	worker.terminate();
	worker.join();
}

// Test 1b: iris_barrier_t frame lifetime - the barrier is owned by a coroutine frame
// freed by the first dispatch of complete() while it still loops over the remaining
// handles, so the loop must read a snapshot, not the freed vector
struct frame_garbage_t {
	size_t payload[16];
};

static coroutine_t barrier_frame_owner(std::shared_ptr<void> keepalive,
                                        iris_barrier_t<void, bool, worker_t>* barrier,
                                        std::atomic<size_t>& done) {
	co_await *barrier;
	(void)keepalive;
	// reuse the just-freed frame block (this coroutine's own locals) so stale
	// handles[] reads observe garbage rather than intact data
	frame_garbage_t garbage;
	for (size_t i = 0; i < 16; i++) garbage.payload[i] = 0xDEADBEEF00000000ull + i;
	done.fetch_add(1, std::memory_order_release);
	// returning here destroys the coroutine frame, which owns the barrier whose
	// complete() is still unwinding its dispatch loop
}

static void test_barrier_frame_lifetime() {
	using barrier_t = iris_barrier_t<void, bool, worker_t>;

	const size_t participants = 8;
	const size_t rounds = 500;

	worker_t worker(std::max<size_t>(2, std::thread::hardware_concurrency()));
	worker.start();

	for (size_t r = 0; r < rounds; r++) {
		std::atomic<size_t> done{ 0 };
		{
			auto holder = std::shared_ptr<barrier_t>(
				new barrier_t(worker, participants),
				[](barrier_t* p) { delete p; });

			// owner FIRST -> slot 0 -> dispatched first by complete()
			barrier_frame_owner(holder, holder.get(), done).run();
			for (size_t i = 1; i < participants; i++) {
				barrier_frame_owner(std::shared_ptr<void>(), holder.get(), done).run();
			}
		}

		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (done.load(std::memory_order_acquire) != participants) {
			if (std::chrono::steady_clock::now() > deadline) {
				printf("  round %zu: only %zu/%zu resumed\n",
					r, done.load(std::memory_order_acquire), participants);
				worker.terminate();
				worker.join();
				throw std::runtime_error("barrier frame lifetime stuck");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	worker.terminate();
	worker.join();
}

int main() {
	int failures = run_demo_checks() == 0 ? 0 : 1;

#if defined(__EMSCRIPTEN__)
	const auto stress_timeout = std::chrono::seconds(60);
	const auto short_timeout = std::chrono::seconds(20);
#else
	const auto stress_timeout = std::chrono::seconds(15);
	const auto short_timeout = std::chrono::seconds(5);
#endif

	if (!run_with_timeout("inline completion chains", short_timeout, test_inline_completion_chains)) failures++;
	if (!run_with_timeout("coroutine result lifetime", short_timeout, test_coroutine_completion_lifetime)) failures++;
	if (!run_with_timeout("coroutine warp context", short_timeout, test_coroutine_warp_context)) failures++;
	if (!run_with_timeout("completion/suspension race", stress_timeout, test_coroutine_completion_race)) failures++;
	if (!run_with_timeout("barrier race", stress_timeout, test_barrier_race)) failures++;
	if (!run_with_timeout("barrier frame lifetime", stress_timeout, test_barrier_frame_lifetime)) failures++;
	if (!run_with_timeout("quota lost wakeup", stress_timeout, test_quota_lost_wakeup)) failures++;
	if (!run_with_timeout("pipe SPSC race", stress_timeout, test_pipe_spsc_race)) failures++;
	if (!run_with_timeout("dispatcher::next", short_timeout, test_dispatcher_next_handle)) failures++;
	if (!run_with_timeout("warp suspend storm", stress_timeout, test_warp_suspend_resume_storm)) failures++;
	if (!run_with_timeout("warp suspend backlog", short_timeout, test_warp_suspend_backlog)) failures++;
	if (!run_with_timeout("warp interrupt drain", stress_timeout, test_warp_interrupt_drain)) failures++;
	if (!run_with_timeout("warp concurrent post", stress_timeout, test_warp_concurrent_post_interrupt)) failures++;
	if (!run_with_timeout("event reset/notify", stress_timeout, test_event_cycle)) failures++;

	printf("\n==== %d failure(s) ====\n", failures);
	// std::exit is intentionally not called so detached watchdog threads
	// don't terminate the process mid-run, return the failure count
	return failures;
}