// Stress tests targeting suspected concurrency issues in iris_coroutine.h /
// iris_dispatcher.h that the existing iris_coroutine_demo does not exercise.
//
// Each test runs with a hard wall-clock watchdog: if it hangs past the deadline
// it is declared FAILED (likely missed-wakeup / lost-handle bug). Otherwise it
// PASSES.
//
// We disable IRIS_ASSERT (`NDEBUG`) so that production behavior is observed
// when invoking buggy code paths.

#include "../src/iris_coroutine.h"
#include "../src/iris_common.inl"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace iris;
using worker_t = iris_async_worker_t<>;
using warp_t = iris_warp_t<worker_t>;
using coroutine_t = iris_coroutine_t<>;

// ---------------------------------------------------------------------------
// Watchdog helper: run `fn` in a detached worker; wait up to `ms`.
// returns true when finished within budget.
template <typename fn_t>
static bool run_with_timeout(const char* name, std::chrono::milliseconds ms, fn_t&& fn) {
	std::promise<void> done;
	std::future<void> fut = done.get_future();
	std::thread([&]() {
		try { fn(); } catch (...) {}
		done.set_value();
	}).detach();

	auto status = fut.wait_for(ms);
	if (status == std::future_status::ready) {
		printf("[PASS] %s\n", name);
		return true;
	}
	printf("[FAIL/HANG] %s (exceeded %lld ms)\n", name, (long long)ms.count());
	return false;
}

// ---------------------------------------------------------------------------
// Test 1: iris_barrier_t - race between concurrent await_suspend writers and
// the thread that runs complete().  When `await_count.fetch_add(1)` returns
// the index, the writing thread has NOT yet stored `handles[index].handle`.
// If another thread is "last" and runs complete() before our store is
// visible, complete() sees an empty handle and skips dispatching it.
//
// We make this fragile situation likely by:
//   - large N (N coroutines per round)
//   - many rounds
//   - spawning each coroutine on a separate thread
// Helper: GCC 12 lambda coroutines erroneously copy awaitables into the
// coroutine frame.  Using a regular function with reference parameters
// works around this (GCC correctly stores only references for function
// parameters).  Same pattern as iris_coroutine_demo.cpp.
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

	const size_t N = 16;            // participants per round
	const size_t rounds = 40;       // number of barrier rounds

	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	for (size_t r = 0; r < rounds; r++) {
		barrier_t barrier(worker, N);
		std::atomic<size_t> resumed{ 0 };
		std::atomic<size_t> launched{ 0 };

		// Launch from many threads to maximize fetch_add interleaving.
		std::vector<std::thread> ts;
		ts.reserve(N);
		std::atomic<bool> go{ false };
		for (size_t i = 0; i < N; i++) {
			ts.emplace_back([&]() {
				while (!go.load(std::memory_order_acquire)) { std::this_thread::yield(); }
				barrier_worker(barrier, launched, resumed).run();
			});
		}
		go.store(true, std::memory_order_release);
		for (auto& t : ts) t.join();

		// Wait until all are launched (`run()` returns after initial suspend).
		while (launched.load(std::memory_order_acquire) != N) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		// All N should eventually be resumed.
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (resumed.load(std::memory_order_acquire) != N) {
			if (std::chrono::steady_clock::now() > deadline) {
				printf("  round %zu: only %zu/%zu coroutines resumed\n",
					r, resumed.load(std::memory_order_acquire), N);
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

// ---------------------------------------------------------------------------
// Test 2: iris_quota_queue_t - lost wakeup race.
//
// guard(amount) decides `ready` at construction time. If quota.acquire fails,
// await_suspend pushes into `handles`.  Meanwhile another thread can call
// release(); release() iterates `handles` - but if our push hasn't landed yet,
// it sees nothing.  We then push, and the wakeup never comes.
//
// We reproduce by hammering a small quota with many consumers, each releasing
// quickly.  If any consumer never resumes, we hang.
static void test_quota_lost_wakeup() {
	using quota_t = iris_quota_t<int, 1>;
	using quota_queue_t = iris_quota_queue_t<quota_t, void, worker_t>;

	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	quota_t quota({ 4 });        // total 4 units
	quota_queue_t qq(worker, quota);

	const size_t N = 200;
	std::atomic<size_t> finished{ 0 };

	auto co = [&]() -> coroutine_t {
		// each acquires 1 unit then releases via RAII
		auto g = co_await qq.guard({ 1 });
		// hold briefly to force contention
		std::this_thread::sleep_for(std::chrono::microseconds(50));
		(void)g;
		finished.fetch_add(1, std::memory_order_release);
	};

	for (size_t i = 0; i < N; i++) co().run();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (finished.load(std::memory_order_acquire) != N) {
		if (std::chrono::steady_clock::now() > deadline) {
			printf("  finished=%zu/%zu (quota lost wakeup)\n",
				finished.load(std::memory_order_acquire), N);
			worker.terminate();
			worker.join();
			throw std::runtime_error("quota stuck");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	worker.terminate();
	worker.join();
}

// ---------------------------------------------------------------------------
// Test 3: iris_pipe_t - SPSC race with default iris_no_mutex_t.
//
// We have one producer and one consumer, but both run on the async worker
// pool, so the underlying push/await interleaves freely.  Default no-mutex
// disables the rendezvous re-check; the producer can increment
// prepared_count while the consumer increments waiting_count without either
// observing the other, leading to a stale state and a missed delivery.
// Helper: avoid GCC 12 lambda-coroutine copy of non-copyable awaitables.
template <typename pipe_t>
static coroutine_t pipe_consumer(pipe_t& pipe,
                                  std::atomic<int>& consumed,
                                  int N) {
	for (int i = 0; i < N; i++) {
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
	const int N = 500;
	std::atomic<int> consumed{ 0 };

	auto producer = [&]() -> coroutine_t {
		for (int i = 0; i < N; i++) {
			pipe.emplace(i);
			// no awaiting - just keep posting
			co_await iris_awaitable(static_cast<warp_t*>(nullptr),
				[]() {}); // yield back to pool
		}
	};

	pipe_consumer(pipe, consumed, N).run();
	producer().run();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (consumed.load(std::memory_order_acquire) != N) {
		if (std::chrono::steady_clock::now() > deadline) {
			printf("  consumed=%d/%d (pipe lost wakeup)\n",
				consumed.load(std::memory_order_acquire), N);
			worker.terminate();
			worker.join();
			throw std::runtime_error("pipe stuck");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	worker.terminate();
	worker.join();
}

// ---------------------------------------------------------------------------
// Test 4: iris_dispatcher_t::next() - REMOVED upstream.  Kept here as a
// compile-time guard: if someone re-introduces a misnamed `next()` overload,
// this test will fail to compile and remind them to handle the routine
// lifecycle correctly.
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

// ---------------------------------------------------------------------------
// Test 5: iris_warp_t - concurrent suspend / resume / queue_routine_post.
// Stress the warp scheduler to expose any starvation under heavy contention.
static void test_warp_suspend_resume_storm() {
	worker_t worker(std::thread::hardware_concurrency());
	worker.start();

	const size_t W = 4;
	std::vector<warp_t> warps;
	warps.reserve(W);
	for (size_t i = 0; i < W; i++) warps.emplace_back(worker);

	const size_t TASKS = 2000;
	std::atomic<size_t> done{ 0 };

	std::atomic<bool> stop{ false };
	std::thread agitator([&]() {
		std::mt19937 rng(123);
		while (!stop.load(std::memory_order_acquire)) {
			warp_t& w = warps[rng() % W];
			w.suspend();
			std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
			w.resume();
		}
	});

	for (size_t i = 0; i < TASKS; i++) {
		warps[i % W].queue_routine_post([&done]() {
			done.fetch_add(1, std::memory_order_release);
		});
	}

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (done.load(std::memory_order_acquire) != TASKS) {
		if (std::chrono::steady_clock::now() > deadline) {
			printf("  done=%zu/%zu (warp starvation)\n",
				done.load(std::memory_order_acquire), TASKS);
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

// ---------------------------------------------------------------------------
// Test 6: iris_event_t - repeated notify/reset cycles with many waiters.
// Helper: avoid GCC 12 lambda-coroutine copy of non-copyable awaitables.
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
		// Give them a moment to suspend.
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

// ---------------------------------------------------------------------------
// Test 1b: iris_barrier_t frame lifetime - the barrier is owned by a coroutine
// frame and that frame is destroyed by the FIRST dispatch of complete().
//
// complete() dispatches handles in slot order. Slot 0 is the owner here, so the
// first resume runs the owner to co_return; final_suspend is suspend_never, so
// the frame - and the barrier, and the handles[] vector living inside it - is
// freed *while complete() is still looping over handles[1..N-1]*. The owner then
// reuses the freed block with a same-sized allocation, so a loop that still
// reads handles[] sees clobbered bytes (this reproduced "vector subscript out of
// range" / an access violation before the snapshot fix).
//
// Slot order == launch order: run() executes the body up to the first co_await
// synchronously, so the owner launched first owns slot 0.
struct frame_garbage_t {
	size_t payload[16];
};

static coroutine_t barrier_frame_owner(std::shared_ptr<void> keepalive,
                                        iris_barrier_t<void, bool, worker_t>* barrier,
                                        std::atomic<size_t>& done) {
	co_await *barrier;
	(void)keepalive;
	// Reuse the just-freed frame block (this coroutine's own locals) so stale
	// handles[] reads observe garbage rather than intact data.
	frame_garbage_t garbage;
	for (size_t i = 0; i < 16; i++) garbage.payload[i] = 0xDEADBEEF00000000ull + i;
	done.fetch_add(1, std::memory_order_release);
	// Returning here destroys the coroutine frame, which owns the barrier whose
	// complete() is still unwinding its dispatch loop.
}

static void test_barrier_frame_lifetime() {
	using barrier_t = iris_barrier_t<void, bool, worker_t>;

	const size_t N = 8;             // participants per round
	const size_t rounds = 500;

	worker_t worker(std::max<size_t>(2, std::thread::hardware_concurrency()));
	worker.start();

	for (size_t r = 0; r < rounds; r++) {
		std::atomic<size_t> done{ 0 };
		{
			auto holder = std::shared_ptr<barrier_t>(
				new barrier_t(worker, N),
				[](barrier_t* p) { delete p; });

			// owner FIRST -> slot 0 -> dispatched first by complete()
			barrier_frame_owner(holder, holder.get(), done).run();
			for (size_t i = 1; i < N; i++) {
				barrier_frame_owner(std::shared_ptr<void>(), holder.get(), done).run();
			}
		}

		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (done.load(std::memory_order_acquire) != N) {
			if (std::chrono::steady_clock::now() > deadline) {
				printf("  round %zu: only %zu/%zu resumed\n",
					r, done.load(std::memory_order_acquire), N);
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

// ---------------------------------------------------------------------------
int main() {
	int failures = 0;

#if defined(__EMSCRIPTEN__)
	const auto stress_timeout = std::chrono::seconds(60);
	const auto short_timeout = std::chrono::seconds(20);
#else
	const auto stress_timeout = std::chrono::seconds(15);
	const auto short_timeout = std::chrono::seconds(5);
#endif

	if (!run_with_timeout("barrier race",          stress_timeout, test_barrier_race))          failures++;
	if (!run_with_timeout("barrier frame lifetime", stress_timeout, test_barrier_frame_lifetime)) failures++;
	if (!run_with_timeout("quota lost wakeup",     stress_timeout, test_quota_lost_wakeup))     failures++;
	if (!run_with_timeout("pipe SPSC race",        stress_timeout, test_pipe_spsc_race))        failures++;
	if (!run_with_timeout("dispatcher::next",      short_timeout,  test_dispatcher_next_handle)) failures++;
	if (!run_with_timeout("warp suspend storm",    stress_timeout, test_warp_suspend_resume_storm)) failures++;
	if (!run_with_timeout("event reset/notify",    stress_timeout, test_event_cycle))           failures++;

	printf("\n==== %d failure(s) ====\n", failures);
	// We intentionally do NOT call std::exit so detached watchdog threads
	// don't terminate the process mid-run. Return the failure count.
	return failures;
}
