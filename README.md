# Iristorm

Iristorm is an extensible asynchronous **header-only** framework written in pure modern C++. It provides:

- **M:N Warp-based Task Scheduler** — a flexible task scheduling system inspired by [Boost.Asio](https://www.boost.org/) strands, mapping N logical warps to M worker threads with automatic mutual exclusion.
- **C++20 Coroutine Integration** — first-class `co_await` support for warp switching, task awaiting, barriers, events, and resource quotas.
- **Lua Binding System** — a reflection-based C++17 binding layer for exposing C++ types, methods, properties, and coroutines to Lua with minimal boilerplate.
- **DAG-based Task Dispatcher** — a task graph for dispatching tasks with partial-order dependencies.

## Table of Contents

- [Build](#build)
- [License](#license)
- [Concepts](#concepts)
- [Quick Start: Warp System](#quick-start)
- [Step Further](#step-further)
  - [In-Warp Parallel](#in-warp-parallel)
  - [Coroutines](#coroutines)
  - [DAG-based Task Dispatcher](#dag-based-task-dispatcher)
  - [Polling from External Thread](#polling-from-external-thread)
  - [Warp Priority and Task Priority](#warp-priority-and-task-priority)
  - [Exiting](#exiting)
- [Lua Binding](#lua-binding)
  - [Registering a Type](#registering-a-type)
  - [Custom Type Conversion](#custom-type-conversion)
  - [Inheritance](#inheritance)
  - [Overloaded Methods](#overloaded-methods)
  - [Working with Tables and References](#working-with-tables-and-references)
  - [Calling Lua from C++](#calling-lua-from-c)
  - [Object Holding: Placement vs View](#object-holding-placement-vs-view)
- [Lua Coroutine Integration](#lua-coroutine-integration)
  - [Exposing Coroutine Methods to Lua](#exposing-coroutine-methods-to-lua)
  - [Async Wait from Lua](#async-wait-from-lua)
  - [Warp Scheduling from Lua Coroutines](#warp-scheduling-from-lua-coroutines)
  - [Resource Quotas from Lua](#resource-quotas-from-lua)
- [Hosted Objects: Manager/Unit Lifetime](#hosted-objects-managerunit-lifetime)
- [Tutorials](#tutorials)
- [Files](#files)

## Build

Iristorm is header-only. The only thing you need to do is to include the corresponding header files.

Most Iristorm classes work with C++11-compatible compilers, except for some optional features:

* Lua Binding support requires the C++17 if-constexpr feature. (Visual Studio 2017+, GCC 7+, Clang 3.9+)
* Coroutine support for the thread pool scheduler requires the C++20 standard coroutine feature. (Visual Studio 2019+, GCC 11+, Clang 14+)

All examples can be built by [CMake build system](https://cmake.org/), see CMakeLists.txt for more details.

## License

Iristorm is distributed under MIT License.

## Concepts

Iristorm provides a simple M:N task scheduler called **Warp System** which is inspired by [Boost](https://www.boost.org/) Strand System. Let's start illustrating it from basic concepts.

#### Task

A task is the **logical** execution unit in the concept of application development. Usually it is represented by a function pointer.

#### Thread

A thread is a **native** execution unit provided by operating system. **Tasks must be run in threads**. Different threads are considered to be possibly running at the same time.

**Multi-threading**, which aims to run several threads within a program, is an effective approach to making full use of CPUs in many-core systems. Usually it's very hard to code and debug. Therefore, there are many data structures, programming patterns, and frameworks to simplify the coding process and make it easier for developers. This project is one of them.

#### Thread Pool

Threads are heavy. It is not efficient to run every task by invoking a brand-new thread. A thread pool is a type of multi-threading framework that can make this more efficient. A thread pool maintains a set of threads called "Worker Threads" **reused** for running tasks. When a new task is required to be run, the thread pool can schedule it to a proper worker thread if there is an idle one, or queue it until any worker becomes idle.

#### Warp

Some tasks are going to read/write at the same objects, or visiting the same thread-unsafe interfaces, indicating that they are not able to run at the same time. See [RACE Condition](https://en.wikipedia.org/wiki/Race_condition) for details. Here we just call them **conflicting** tasks.

To make our programs run correctly, we must establish some techniques to prevent unexpected conflicts. Here we introduce a new concept: **Warp**.

A warp is a logical container of a series of conflicting tasks. Tasks belonging to the **same warp** are guaranteed to be **mutually exclusive** automatically, and thus **no two of them** can be run at the same time, avoiding race conditions proactively. This feature is called **warp restriction**. To make coding easier, we can bind all tasks related to a specific object to a specific warp. In this case, we say that this object is fully bound to a warp context.

Besides, tasks among **different** warps **can** be run at the same time respectively.

#### Warp System

The Warp System is a bridge between **warps** and the **thread pool**. That is, programmers commit tasks labeled by warp to the system, which then schedules them to a thread pool. With some magic techniques applied internally, we finally construct a conflict-free task flow.

The thread count **M** of Warp System is **fixed** when it starts. But the warp count **N** can be dynamically adjusted by programmers at will. So the warp system is a type of flexible M:N task mapping system.

## Quick Start

Let's start with simple programs in [iris_dispatcher_demo.cpp](test/iris_dispatcher_demo.cpp). 

#### Basic Example: simple explosion

The Warp System runs on a thread pool, and the first step is to create it. There is a built-in thread pool written in C++11 std::thread in [iris_dispatcher.h](src/iris_dispatcher.h), you can replace it with your own platform-specific implementation.

```C++
static const size_t thread_count = 4;
iris_async_worker_t<> worker(thread_count);
```

Then we initialize the warps. There is no "warp system class". Each warp is **individual**, just create a vector of them. We call them warp 0, warp 1, etc. 

Different from boost strands, the tasks in a warp are **NOT** ordered by default, which means the **final execution order** is not the same as the order of committing. You can still enable ordering as you like anyway (see declaration of "strand_t" in the following code), which is not recommended because ordering may be slightly less efficient than the default setting.

```C++
static const size_t warp_count = 8;
using warp_t = iris_warp_t<iris_async_worker_t<>>;
using strand_t = iris_warp_t<iris_async_worker_t<>, true>; // behaves like a strand

std::vector<warp_t> warps;
warps.reserve(warp_count);
for (size_t i = 0; i < warp_count; i++) {
	warps.emplace_back(worker); // calls iris_warp_t::iris_warp_t(iris_async_worker_t<>&)
}
```

Then we can schedule a task into the warp you want. Just call **queue_routine**.

```C++
warps[0].queue_routine([]() {/* operations on warps[0] */});
warps[0].queue_routine([]() {/* operations on warps[0] */});
```

That's all you need to do. According to warp restrictions, operation A and operation B are **never executed at the same time**, since they are in the **same** warp.

Otherwise, if we queue_routine tasks to different warps, like:

```C++
warps[0].queue_routine([]() { /* do operation C */});
warps[1].queue_routine([]() { /* do operation D */});
```

According to warp restrictions, operation C and operation D **could be executed at the same time**.

Here is an "explosion" example. In this example, we code a function called "explosion", which randomly forks multiple recursions of writing operations on an integer array described here:

```C++
static int32_t warp_data[warp_count] = { 0 };
```

The restriction is that warp 0 can only write warp_data[0], warp 1 can only write warp_data[1]:

```C++
std::function<void()> explosion;
static constexpr size_t split_count = 4;
static constexpr size_t terminate_factor = 100;

explosion = [&warps, &explosion, &worker]() {
	if (worker.is_terminated())
		return;

	warp_t& current_warp = *warp_t::get_current_warp();
	size_t warp_index = &current_warp - &warps[0];
	warp_data[warp_index]++;

	// simulate working
	std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 40));
	warp_data[warp_index]++;

	if (rand() % terminate_factor == 0) {
		// randomly terminates
		worker.terminate();
	}

	warp_data[warp_index]++;
	// randomly dispatch to warp
	for (size_t i = 0; i < split_count; i++) {
		warps[rand() % warp_count].queue_routine(std::function<void()>(explosion));
	}

	warp_data[warp_index] -= 3;
};
```

Though there are no locks or atomics on operating warp_data, we can still assert that the final value of each warp_data must be 0. The execution of the same warp never overlaps in the timeline.

#### Advanced Example: garbage collection

There is a function named garbage_collection, which simulates a multi-threaded mark-sweep [garbage collection](http://en.wikipedia.org/wiki/Garbage_collection_(computer_science) ) process.

Garbage collection is a technique for collecting unreferenced objects and deleting them. Mark-sweep is a basic approach for garbage collection. It contains three steps:

1. Scanning all objects and mark them **unvisited**.
2. Traverse from root objects through reference relationships, mark all objects that can be directly or indirectly referenced to **visited**.
3. Rescanning all objects, delete the objects with **unvisited** mark. Thus all objects that are not linked to root objects (i.e. garbage) are deleted.

Now suppose we got the definition of basic object node as follows:

```C++
struct node_t {
	size_t warp_index = 0;
	size_t visit_count = 0; // we do not use std::atomic<> here.
	std::vector<size_t> references;
};

struct graph_t {
	std::vector<node_t> nodes;
};
```

To apply garbage collection, we need to record every **references** from the current node, and traverse them from root object as collecting. We use **visit_count** to record whether the current node is **visited**.

If you are experienced in multi-threaded programming, you may figure out that **visit_count** should be of type std::atomic<size_t> because there may be several threads performing modification during the collection process.

But we have decided to make things different.

We are splitting the node visiting operations into multiple warps (recorded by member **warp_index**). For example, node 1-10 are grouped into warp 0, node 11-20 are grouped into warp 1, or just randomly assigned. Any task operations on the nodes in the same warp will be protected by the warp system. As a result, the variable **visit_count** is guaranteed to **never** be operated on by multiple threads, and no atomics or locks are required.

In order to obey the warp restriction, all we need to do is to invoke a task with related node's warp when we are planning to do something on it:

```C++
warps[target_node.warp_index].queue_routine([]() {
	// operations on target_node
});
```

Since we have visited a new node, all **references** should be added into next collection process. To preserve the warp restriction, we schedule them into their own warps: (see the line commented with <------)

```C++
graph_t graph;
std::function<void(size_t)> collector;
std::atomic<size_t> collecting_count;
collecting_count.store(0, std::memory_order_release);

collector = [&warps, &collector, &worker, &graph, &collecting_count](size_t node_index) {
	warp_t& current_warp = *warp_t::get_current_warp();
	size_t warp_index = &current_warp - &warps[0];

	node_t& node = graph.nodes[node_index];
	assert(node.warp_index == warp_index);

	if (node.visit_count == 0) {
		node.visit_count++; // SAFE: only one thread can visit it

		for (size_t i = 0; i < node.references.size(); i++) {
			size_t next_node_index = node.references[i];
			size_t next_node_warp = graph.nodes[next_node_index].warp_index;
			collecting_count.fetch_add(1, std::memory_order_acquire);
			warps[next_node_warp].queue_routine(std::bind(collector, next_node_index)); // <------
		}
	}

	if (collecting_count.fetch_sub(1, std::memory_order_release) == 1) {
		// all work finished.
		// ...
	}
};
```

That's all, there are **no** explicit locks or atomics. All dangerous multi-threaded work is done by the Warp System. See the full source code of garbage_collection for more details.

#### Discussion

Now let's get back to the beginning, what's the meaning of warps? What if we just use atomics or locks?

The answer contains three aspects:

1. Convenient: The only thing you must remember is the rule that **always schedule tasks according to warp**. There is no lock-order requirement, dead-locking, busy-waiting, memory order problem, or atomic myths.
2. High performance: If we abuse locks and atomics everywhere, for example, allocating separate locks on each object, performing lock or atomic operations whenever we need to access objects, then the program will get stuck on bus-locking, kernel-switching, and thread-switching, which leads to low performance. The warp concept wraps a series of operations or a number of objects into a logical "scheduling package", reducing switching cost and busy-wait cost, making them more friendly for multi-threaded systems.
3. Flexible: You can easily adjust the object/task warping rules as you like. For example, allocating more warps and splitting objects with smaller granularity if you have more CPUs. The system allows programmers to transport an object or a group of tasks from one warp to another dynamically, if they are working on some dynamic load-balancing features.



## Step Further

### In-Warp Parallel

In the common case, there is only one thread running in a warp context. But what if we want to break the rule temporarily by local code and do some parallelized operations while the warp restriction is held for other code? I know it's unsafe, but I just want to do it.

Open the [iris_dispatcher_demo.cpp](test/iris_dispatcher_demo.cpp) and you can find a piece of code in function "simple_explosion":

```C++
static constexpr size_t parallel_factor = 11;
static constexpr size_t parallel_count = 6;
if (rand() % parallel_factor == 0) {
	// read-write lock example: multiple reading blocks writing
	std::shared_ptr<std::atomic<int32_t>> shared_value = std::make_shared<std::atomic<int32_t>>(-0x7fffffff);
	for (size_t i = 0; i < parallel_count; i++) {
		current_warp.queue_routine_parallel([shared_value, warp_index]() {
			// only read operations
			std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 40));
			int32_t v = shared_value->exchange(warp_data[warp_index], std::memory_order_release);
			assert(v == warp_data[warp_index] || v == -0x7fffffff);
		});
	}
}
```

The function **queue_routine_parallel** invokes a special parallelized task on current_warp, which can be run at the same time. While a parallelized task is running, other normal tasks on current_warp remain **blocked**. After all parallelized tasks finish, the normal tasks can then be scheduled.

**Parallelized tasks to normal tasks is what read locks to write locks** if you **NEVER** call warp_t::poll() manually. It's an advanced feature and you must be careful when using them.

### Coroutines

In C++20, we can use coroutines to simplify asynchronous program development.

Warp system supports coroutine integration, you can find an example at [iris_coroutine_demo.cpp](test/iris_coroutine_demo.cpp):

To start with a coroutine, just write a function with return value type `iris_coroutine_t`:

```C++
iris_coroutine_t<return_type> example(warp_t::async_worker_t& async_worker, warp_t* warp, int value) {}
```

In this coroutine function, you can `co_await` **iris_switch** to switch to another warp context:

```C++
if (warp != nullptr) {
	warp_t* current = co_await iris_switch(warp);
	printf("Switch to warp %p\n", warp);
	co_await iris_switch((warp_t*)nullptr);
	printf("Detached\n");
	co_await iris_switch(warp);
	printf("Attached\n");
	co_await iris_switch(current);
	assert(current == warp_t::get_current_warp());
}
```

`co_await iris_switch` returns the previous warp. Notice that we can switch to a `nullptr` warp, which means that we want to detach from the current warp. Switching from a `nullptr` warp to a valid warp is also allowed.

And we can create and wait for an asynchronous task on the target warp:

```C++
co_await iris_awaitable(warp, []() {});
```

It is equivalent to switching to warp and switching back. But **iris_awaitable** allows early dispatching before waiting:

```C++
auto awaitable = iris_awaitable(warp, []() {});
awaitable.dispatch();
// do something other
co_await awaitable;
```

`iris_coroutine_t<return_type>` is not only a coroutine but also an awaitable object. You could also `co_await` it to chain your coroutine pipeline:

```C++
static iris_coroutine_t<int> cascade_ret(warp_t* warp) {
	warp_t* w = co_await iris_switch(warp);
	printf("Cascaded int!\n");
	co_await iris_switch(w);
	co_return 1234;
}

// In another coroutine:
int result = co_await cascade_ret(warp);
// result == 1234
```

Additional coroutine utilities include:

- **iris_barrier_t**: synchronization barrier for N coroutines

  ```C++
  iris_barrier_t<void, bool, worker_t> barrier(worker, 4);
  // In each coroutine:
  co_await barrier;  // all 4 must reach here before any proceeds
  ```

- **iris_event_t**: event signaling for coroutines

  ```C++
  iris_event_t<warp_t> event(async_worker);
  // Waiter: co_await event;
  // Signaler: event.notify();
  ```

- **iris_quota_t / iris_quota_queue_t**: resource quota management

  ```C++
  iris_quota_t<int, 2> quota({ 4, 5 });
  iris_quota_queue_t<iris_quota_t<int, 2>, warp_t> quota_queue(worker, quota);
  auto guard = co_await quota_queue.guard({ 1, 3 });
  // quota automatically released when guard goes out of scope
  ```

- **iris_select**: randomly select an available warp from a range

  ```C++
  co_await iris_switch<warp_t>(nullptr);  // detach first
  warp_t* selected = co_await iris_select(warp_begin, warp_end);
  ```

### DAG-based Task Dispatcher

DAG-based Task Dispatcher, also well-known as Task Graph, is a widely used task dispatching technique for tasks with partial order dependency.

We also provide a DAG-based Task Dispatcher called iris_dispatcher_t (see function "graph_dispatch" at [iris_dispatcher_demo](test/iris_dispatcher_demo.cpp)):

You can create a dispatcher with:

```C++
iris_dispatcher_t<warp_t> dispatcher(worker);
```

The second parameter is an optional function, called after all tasks in dispatcher graph finished.

To add a task to dispatcher, call **allocate**. 

```C++
auto d = dispatcher.allocate(&warps[2], []() { std::cout << "Warp 2 task [4]" << std::endl; });
auto a = dispatcher.allocate(&warps[0], []() { std::cout << "Warp 0 task [1]" << std::endl; });
auto b = dispatcher.allocate(&warps[1], []() { std::cout << "Warp 1 task [2]" << std::endl; });
```

Notice that there is a return value with internal type routine_t*. You can call the **order** function to order them later.

```C++
dispatcher.order(a, b);
// dispatcher.order(b, a); // will trigger validate assertion

auto c = dispatcher.allocate(nullptr, []() { std::cout << "Warp nil task [3]" << std::endl; });
dispatcher.order(b, c);
// dispatcher.order(c, a); // will trigger validate assertion
dispatcher.order(b, d);
```

Then call **dispatch** to run them.

```C++
dispatcher.dispatch(a);
dispatcher.dispatch(b);
dispatcher.dispatch(c);
dispatcher.dispatch(d);
```

To dispatch more flexibly, you can **defer/dispatch** a task dynamically. Notice that **defer** must be called during dispatcher running and **BEFORE** the target task actually runs.

```c++
auto b = dispatcher.allocate(&warps[1], [&dispatcher, d]() {
	dispatcher.defer(d);
	std::cout << "Warp 1 task [2]" << std::endl;
	dispatcher.dispatch(d);
});
```

### Polling from External Thread

It is a common case that a thread has to be blocked to wait for some signals to arrive. For example, suppose you are spinning to wait for an atomic variable to reach the expected value (spin lock, for example), and there is nothing to do but spin. In this case, we can try to "borrow" some tasks from the thread pool and execute them if our atomic variable is not ready yet.

```C++
while (some_variable.load(std::memory_order_acquire) != expected_value) {
	// delay at most 20ms or poll tasks with priority 0 if possible 
	worker.poll_one(0, std::chrono::milliseconds(20));
}
```

### Warp Priority and Task Priority

Iristorm supports a priority system that controls the scheduling order of tasks in the thread pool. There are two levels of priority: **warp priority** and **task priority**. Together they allow fine-grained control over which tasks get executed first when multiple tasks are competing for worker threads.

#### Task Priority

The thread pool (`iris_async_worker_t`) organizes its internal task queues by priority levels ranging from **0** to **thread_count - 1**, where **0 is the highest priority**. When a worker thread looks for the next task to execute, it scans from the highest priority (0) downward, so higher-priority tasks are always picked up first.

You can queue a task with a specific priority directly:

```C++
// Queue a task with priority 0 (highest)
worker.queue([]() { /* critical work */ }, 0);

// Queue a task with priority 2 (lower)
worker.queue([]() { /* background work */ }, 2);
```

Priority also affects thread wake-up behavior. When a task is queued at priority level P, only a waiting thread whose index satisfies `waiting_thread_count > P + limit_count` will be woken up. This means higher-priority tasks are more aggressive at waking idle threads, while lower-priority tasks may wait for an already-running thread to pick them up naturally. This avoids unnecessary context switches for low-priority work.

When polling tasks from an external thread, you can specify the maximum priority level to poll:

```C++
// Poll tasks with priority 0 only (highest priority)
worker.poll_one(0);

// Poll tasks with priority up to 2
worker.poll_one(2);

// Poll with timeout
worker.poll_one(0, std::chrono::milliseconds(20));
```

The internal worker threads use an automatic priority scheme: the more threads that are currently running, the lower the effective priority each thread polls at. This is controlled by `running_count` — the first thread to start polling gets the highest effective priority (can see all tasks), while subsequent threads see progressively fewer priority levels. This naturally load-balances work and prevents low-priority tasks from starving when there is heavy contention.

#### Warp Priority

Each warp has a **fixed priority** that is set at construction time:

```C++
// Create a warp with default priority 0 (highest)
warp_t high_priority_warp(worker);

// Create a warp with priority 2 (lower)
warp_t low_priority_warp(worker, 2);
```

When a warp flushes its pending tasks to the thread pool, it uses its own priority value. This means **all tasks queued to a warp inherit the warp's priority level**. A warp with priority 0 will have its tasks scheduled before tasks from a warp with priority 2, assuming both are competing for the same worker threads.

Warp priority affects three scheduling paths:
1. **Normal task flushing**: When the warp's internal queue is flushed via `queue_routine`, the flush operation is dispatched to the thread pool with the warp's priority.
2. **Parallel task dispatching**: When parallel tasks (via `queue_routine_parallel`) are sent to the thread pool, they also use the warp's priority.
3. **External thread submissions**: When a task is submitted from a non-worker thread, it is queued to the thread pool directly with the warp's priority.

#### Task Priority in DAG Dispatcher

The DAG-based task dispatcher (`iris_dispatcher_t`) also supports per-task priority. However, **task priority only takes effect for tasks with no associated warp** (i.e., `warp == nullptr`). For warped tasks, the warp's own priority is used instead.

```C++
iris_dispatcher_t<warp_t> dispatcher(worker);

// Task with no warp — priority 0 (highest), dispatched directly to worker
auto a = dispatcher.allocate(nullptr, []() { /* critical */ }, 0);

// Task with no warp — priority 2 (lower)
auto b = dispatcher.allocate(nullptr, []() { /* background */ }, 2);

// Warped task — priority parameter is ignored, warp's own priority is used
auto c = dispatcher.allocate(&warps[0], []() { /* uses warps[0].priority */ });
```

#### Priority Task Handler

For advanced use cases, you can install a custom priority task handler to intercept tasks with special priority values. Tasks queued with a negative priority (i.e., `priority == ~(size_t)0`) are routed to this handler before being placed into the normal task queue:

```C++
worker.set_priority_task_handler([](iris_async_worker_t<>::task_base_t* task, size_t& priority) -> bool {
	// Return true to consume the task (it won't be queued normally)
	// Return false to let it proceed with the (possibly modified) priority
	// You can modify 'priority' to reassign the task's priority level
	return false;
});
```

This is useful for implementing custom scheduling policies, such as deferred execution or task filtering.

### Exiting

Use iris_warp_t::poll to poll all tasks from all warps (including their async_worker's tasks) while exiting.

```C++
async_worker.terminate();
async_worker.join();
while (iris_warp_t::poll({ warp1, warp2, ... })) {
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
```

## Lua Binding

Iristorm includes a powerful Lua binding system in [iris_lua.h](src/iris_lua.h) that lets you expose C++ types to Lua with minimal boilerplate. It supports methods, properties, constructors, lambdas, overloaded functions, custom type conversions, inheritance, and coroutines. Requires C++17.

> **Note for custom MSVC projects (Windows x64):** Lua's error handling relies on `longjmp` (via `luaL_error`), which on MSVC x64 uses `RtlUnwindEx` to unwind the stack. If `longjmp` passes through a C++ frame compiled with the default `/EHsc` flag, `__CxxFrameHandler4` may call `terminate()` when it encounters a non-C++ (SEH) unwind — even if the frame has no RAII objects. To prevent this, you must do **one** of the following when integrating Lua in your own build system:
>
> 1. **Enable `/EHa`** (asynchronous exception handling) for all C++ translation units that contain Lua stub functions, e.g. add `/EHa` to your compiler flags and remove the default `/EHsc`. CMake example:
>    ```cmake
>    if (MSVC)
>        string(REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
>        if (NOT CMAKE_CXX_FLAGS MATCHES "/EHa")
>            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa")
>        endif()
>    endif()
>    ```
> 2. **Compile the Lua C sources as C++** (e.g. rename `.c` to `.cpp`, or use `/TP` in MSVC), so that Lua's internal `longjmp`-based error handling is replaced by C++ exception-based error handling (`LUA_USE_LONGJMP` not defined), avoiding the cross-frame unwind issue entirely.
>
> This project's `CMakeLists.txt` files already apply option 1 automatically when building with MSVC.

### Registering a Type

Define a C++ class with a static `lua_registar` method and a `lua_typename` to expose it to Lua:

```C++
struct example_t {
	int value = 10;

	static constexpr const char* lua_typename() noexcept {
		return "example_t";
	}

	static void lua_registar(iris_lua_t lua, std::nullptr_t) {
		// Constructor: example_t.new() from Lua
		lua.set_current_new<&iris_lua_t::place_new_object<example_t>>("new");

		// Bind a member variable (read/write property from Lua)
		lua.set_current<&example_t::value>("value");

		// Bind a member function
		lua.set_current<&example_t::get_value>("get_value");

		// Bind a static function
		lua.set_current<&example_t::accum_value>("accum_value");

		// Bind a lambda
		lua.set_current("lambda", [](int v) { return v + 1; });
	}

	// Optional: called when object is created in Lua
	static void lua_initialize(iris_lua_t lua, int index, example_t* p) {
		printf("Object created!\n");
	}

	// Optional: called when object is garbage-collected
	static void lua_finalize(lua_State* L, int index, example_t* p) noexcept {
		printf("Object destroyed!\n");
	}

	int get_value() noexcept { return value; }
	int accum_value(int init) noexcept { return value += init; }
};
```

Register and use from C++:

```C++
lua_State* L = luaL_newstate();
luaL_openlibs(L);
iris_lua_t lua(L);

// Register the type and make it globally available
auto example_type = lua.make_registry_type<example_t>();
lua.set_global("example_t", std::move(example_type));
```

Then use it from Lua:

```lua
local obj = example_t.new()
print(obj.value)         -- 10
obj.value = 42
print(obj:get_value())   -- 42
obj:accum_value(8)
print(obj.value)         -- 50
print(obj.lambda(3))     -- 4
```

### Custom Type Conversion

You can teach the binding system how to convert custom types between C++ and Lua by specializing `iris_lua_traits_t`:

```C++
struct vector3 {
	float x, y, z;
};

template <>
struct iris::iris_lua_traits_t<vector3> : std::true_type {
	// Push a vector3 onto the Lua stack as a table {x, y, z}
	static int lua_tostack(lua_State* L, vector3&& v) noexcept {
		lua_newtable(L);
		lua_pushnumber(L, v.x); lua_rawseti(L, -2, 1);
		lua_pushnumber(L, v.y); lua_rawseti(L, -2, 2);
		lua_pushnumber(L, v.z); lua_rawseti(L, -2, 3);
		return 1;
	}

	// Read a vector3 from the Lua stack
	static vector3 lua_fromstack(lua_State* L, int index) noexcept {
		lua_pushvalue(L, index);
		lua_rawgeti(L, -1, 1);
		lua_rawgeti(L, -2, 2);
		lua_rawgeti(L, -3, 3);
		float x = (float)lua_tonumber(L, -3);
		float y = (float)lua_tonumber(L, -2);
		float z = (float)lua_tonumber(L, -1);
		lua_pop(L, 4);
		return vector3{ x, y, z };
	}
};
```

Now `vector3` can be used transparently in bound functions:

```C++
// In lua_registar:
lua.set_current<&example_t::get_vector3>("get_vector3");

// C++ method:
vector3 get_vector3(const vector3& input) noexcept { return input; }
```

```lua
local v = obj:get_vector3({1.0, 2.0, 3.0})
print(v[1], v[2], v[3])  -- 1.0  2.0  3.0
```

### Inheritance

Derived types can inherit bindings from a base type. When binding member functions or variables from parent classes, use the explicit type parameter:

```C++
template <typename type_t>
struct crtp_t {
	void crtp_foo() { printf("CRTP foo()!\n"); }
	int crtp_member = 5;
};

struct example_base_t : crtp_t<example_base_t> {
	int base_value = 2222;

	template <typename traits_t>
	static void lua_registar(iris_lua_t lua, traits_t) {
		// Must specify the derived type for members
		lua.set_current<&example_base_t::crtp_foo, example_base_t>("crtp_foo");
		lua.set_current<&example_base_t::crtp_member, example_base_t>("crtp_member");
		lua.set_current<&example_base_t::base_value>("base_value");
	}

	static constexpr const char* lua_typename() noexcept {
		return "example_base_t";
	}
};

struct example_derived_t : example_base_t {
	static void lua_registar(iris_lua_t lua, std::nullptr_t) {
		lua.set_current_new<&iris_lua_t::place_new_object<example_derived_t>>("new");
		lua.set_current<&example_derived_t::derived_method>("derived_method");
	}
	// ...
};

// Register with inheritance:
auto base_type = lua.make_type<example_base_t>();
auto derived_type = lua.make_registry_type<example_derived_t>(std::move(base_type));
lua.set_global("example_derived_t", std::move(derived_type));
```

Now Lua objects of `example_derived_t` also have access to `crtp_foo`, `crtp_member`, and `base_value`.

### Overloaded Methods

Use `iris_overload_cast` to disambiguate overloaded methods:

```C++
struct example_t {
	int overload_func() { return 1; }
	int overload_func(int) { return 2; }

	static void lua_registar(iris_lua_t lua, std::nullptr_t) {
		// Register both overloads under the same Lua name
		lua.set_current_overload<iris_overload_cast<int>(&example_t::overload_func)>("overload_func");
		lua.set_current_overload<iris_overload_cast<int, int>(&example_t::overload_func)>("overload_func");
	}
};
```

The binding system automatically selects the correct overload based on the argument count from Lua.

### Working with Tables and References

Create and manipulate Lua tables from C++:

```C++
// Create a table
iris_lua_t::ref_t table = lua.make_table([](iris_lua_t lua) noexcept {
	lua.set_current("name", "prime");
	lua.set_current(1, 2);
	lua.set_current(2, 3);
	lua.set_current(3, 5);
});

// Read values from a table reference
auto value = table.get<int>(lua, "name");

// Iterate a table
table.for_each(lua, [](iris_lua_t lua) {
	// key at stack[-2], value at stack[-1]
});
```

Compound types like `std::vector`, `std::map`, `std::pair`, and `std::tuple` are automatically converted:

```C++
// C++ function returning a map
std::map<std::string, int> forward_map(std::map<std::string, int>&& v) {
	v["abc"] = 123;
	return v;
}

// C++ function returning a tuple (becomes multiple return values in Lua)
std::tuple<int, std::string> forward_tuple(std::tuple<int, std::string>&& v) {
	std::get<0>(v) = std::get<0>(v) + 1;
	return v;
}
```

### Calling Lua from C++

Call Lua functions from C++ using `ref_t`:

```C++
// Load and call a Lua chunk
auto func = lua.load("return function(a, b) return a + b end");
auto add = lua.call<iris_lua_t::ref_t>(func);
auto result = lua.call<int>(add, 10, 20);
// result.value() == 30

// Call a bound method with a callback
int call(iris_lua_t lua, iris_lua_t::ref_t&& callback, int value) {
	auto result = lua.call<int>(callback, value);
	lua.deref(std::move(callback));
	return result.value_or(0);
}
```

```lua
local obj = example_t.new()
local result = obj:call(function(v) return v * 2 end, 21)
print(result)  -- 42
```

### Object Holding: Placement vs View

When exposing C++ objects to Lua, Iristorm provides two fundamentally different strategies for how the object's memory is managed: **placement** and **view**. Understanding the distinction is essential for writing correct and efficient bindings.

#### Placement (Owned Objects)

With placement, the C++ object is **constructed directly inside the Lua userdata memory**. Lua owns the object — it is created when the userdata is allocated and destroyed (via the C++ destructor) when Lua's garbage collector collects the userdata.

Use `place_new_object` in the constructor binding:

```C++
struct my_object_t {
	int value = 0;

	static void lua_registar(iris_lua_t lua, std::nullptr_t) {
		// Placement: object lives inside Lua userdata
		lua.set_current_new<&iris_lua_t::place_new_object<my_object_t>>("new");
		lua.set_current<&my_object_t::value>("value");
	}

	static constexpr const char* lua_typename() noexcept { return "my_object_t"; }
};
```

You can also create placement objects from C++:

```C++
// Create a Lua-owned object from C++, returning a reference
auto obj_ref = lua.make_registry_object<my_object_t>();
```

**Key characteristics of placement:**
- The object is **fully owned by Lua**. Its lifetime is determined by Lua's garbage collector.
- The C++ destructor (`~my_object_t()`) is called automatically when the userdata is collected.
- The optional `lua_initialize` callback is invoked when the object is created, and `lua_finalize` is called before the destructor runs during garbage collection.
- The object's memory is part of the Lua userdata block, so **no separate heap allocation** is needed.
- This is the best choice when the object's lifecycle is purely driven from Lua scripts.

#### View (Non-Owning References)

With view, the Lua userdata stores only a **pointer** to an existing C++ object. Lua does **not** own the object — it merely provides a reference (a "view") into C++-managed memory. The C++ side is responsible for ensuring the object remains alive as long as Lua might access it.

Create a view from C++:

```C++
my_object_t cpp_object;
cpp_object.value = 42;

// Create a view into an existing C++ object
auto view_ref = lua.make_registry_object_view<my_object_t>(&cpp_object);
lua.set_global("cpp_obj", std::move(view_ref));
```

**Key characteristics of view:**
- The Lua userdata holds a **pointer** to the C++ object, not the object itself.
- Lua does **not** call the destructor when the userdata is garbage-collected. The C++ side manages the object's lifetime.
- If the C++ object is destroyed before Lua finishes using the view, accessing the view from Lua leads to **undefined behavior**. You must ensure the C++ object outlives all Lua references to it.
- The optional `lua_view_initialize` callback is invoked when the view is created, and `lua_view_finalize` is called when the view userdata is garbage-collected. These callbacks are useful for custom reference counting.
- Views are the right choice when the object is managed externally (e.g., by a C++ engine, a resource manager, or a shared ownership system) and Lua only needs to interact with it temporarily.

#### Internally Distinguishing Placement and View

Iristorm uses a bit flag (`size_mask_view`) in the userdata's raw length to distinguish the two modes at runtime. When extracting an object pointer from Lua:

- If the view bit is **not set**, the userdata is treated as a placement object, and the pointer is computed directly from the userdata memory.
- If the view bit **is set**, the userdata is treated as a view, and the pointer is read by dereferencing the stored pointer (or via a custom `lua_view_extract` callback).

This distinction is transparent to Lua scripts — both placement objects and views expose the same methods and properties. The difference only matters on the C++ side.

#### Shared Objects

For objects that need shared ownership across multiple Lua states or between C++ and Lua, Iristorm provides the `shared_object_t` base class. Shared objects use **views** internally but add **reference counting** to manage lifetime:

```C++
struct my_shared_t : iris_lua_t::shared_object_t<my_shared_t> {
	int data = 0;

	static void lua_registar(iris_lua_t lua, std::nullptr_t) {
		// Shared: object is heap-allocated and reference-counted
		lua.set_current_new<&iris_lua_t::shared_new_object<my_shared_t>>("new");
		lua.set_current<&my_shared_t::data>("data");
	}

	static constexpr const char* lua_typename() noexcept { return "my_shared_t"; }
};
```

With `shared_new_object`, the object is **heap-allocated** and managed via an intrusive reference count. Each Lua view increments the reference count (`lua_shared_acquire`), and when a view is garbage-collected, the count is decremented (`lua_shared_release`). The object is deleted when the count reaches zero.

There is also `shared_local_object_t`, a variant that tracks a single "canonical" Lua reference and avoids incrementing/decrementing the reference count for every additional view within the same Lua state. This is more efficient when many short-lived views are created.

#### Summary

| Feature | Placement | View | Shared |
|---------|-----------|------|--------|
| Memory location | Inside Lua userdata | External C++ memory | Heap-allocated |
| Ownership | Lua GC | C++ side | Reference-counted |
| Constructor | `place_new_object` | `make_object_view` / `make_registry_object_view` | `shared_new_object` |
| Destructor called by Lua | Yes (`~T()` + `lua_finalize`) | No (`lua_view_finalize` only) | When ref count → 0 |
| Use case | Lua-owned objects | Temporary references to C++ objects | Cross-state or shared-lifetime objects |

## Lua Coroutine Integration

When C++20 coroutines are available, Iristorm bridges **C++ coroutines** with **Lua coroutines** seamlessly. A C++ method returning `iris_coroutine_t<T>` automatically **yields** the calling Lua coroutine and **resumes** it when the C++ coroutine completes — no manual coroutine management needed on the Lua side.

See the full tutorial at [tutorial/lua_co_await](tutorial/lua_co_await), with
systematic docs in [tutorial/lua_co_await/docs/](tutorial/lua_co_await/docs/README.md).

### Exposing Coroutine Methods to Lua

Any C++ method that returns `iris_coroutine_t<T>` is automatically treated as a yielding function in Lua:

```C++
struct tutorial_async_t {
	static void lua_registar(iris_lua_t&& lua, std::nullptr_t) {
		lua.set_current_new<&iris_lua_t::place_new_object<tutorial_async_t>>("new");
		lua.set_current<&tutorial_async_t::wait>("wait");
	}

	iris_coroutine_t<void> wait(size_t milliseconds) {
		// Switch to a worker thread (detach from current warp)
		auto* current = co_await iris_switch(
			static_cast<iris_warp_t<iris_async_worker_t<>>*>(nullptr));

		// Do blocking work on worker thread
		std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));

		// Switch back to the original warp (resumes Lua coroutine)
		co_await iris_switch(current);
	}
};
```

Coroutines with overloaded signatures are also supported:

```C++
static void lua_registar(iris_lua_t lua, std::nullptr_t) {
	lua.set_current_overload<
		iris_overload_cast<iris::iris_coroutine_t<int>, const std::string&>
		(&example_t::coro_get_int)>("coro_get_int");
	lua.set_current_overload<
		iris_overload_cast<iris::iris_coroutine_t<int>, const std::string&, int>
		(&example_t::coro_get_int)>("coro_get_int");
}

static iris::iris_coroutine_t<int> coro_get_int(const std::string& s) noexcept {
	co_return 1;
}

static iris::iris_coroutine_t<int> coro_get_int(const std::string& s, int) noexcept {
	co_return 2;
}
```

### Async Wait from Lua

From the Lua side, calling a coroutine method looks like a normal function call when wrapped in a Lua coroutine:

```lua
-- Wrap in a Lua coroutine to allow yielding
coroutine.wrap(function()
    local async = tutorial_async_t.new()
    print("Waiting 1000ms...")
    async:wait(1000)           -- yields Lua, sleeps on C++ worker thread
    print("Wait complete!")    -- resumes here automatically
end)()

-- The main thread must poll to drive execution:
while co_await:poll(1000) do end
```

The C++ `iris_coroutine_t<void>` return type causes the Lua call to yield. When the C++ coroutine finishes and switches back to the original warp, the Lua coroutine is automatically resumed.

### Warp Scheduling from Lua Coroutines

Warp-protected scheduling works naturally with Lua coroutines. Here is a C++ class that demonstrates warp-safe operations driven from Lua:

```C++
struct tutorial_warp_t {
	iris_warp_t<iris_async_worker_t<>> stage_warp;
	int warp_variable = 0;
	int free_variable = 0;

	tutorial_warp_t(iris_async_worker_t<>& worker) : stage_warp(worker) {}

	static void lua_registar(iris_lua_t&& lua, std::nullptr_t) {
		lua.set_current<&tutorial_warp_t::pipeline>("pipeline");
		lua.set_current<&tutorial_warp_t::warp_variable>("warp_variable");
		lua.set_current<&tutorial_warp_t::free_variable>("free_variable");
	}

	iris_coroutine_t<void> pipeline() {
		// Switch to stage_warp — operations here are mutually exclusive
		auto* current = co_await iris_switch(&stage_warp);
		int v = warp_variable;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		warp_variable = v + 1;
		v = warp_variable;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		warp_variable = v - 1;

		// Detach from warp — operations here may run on any thread
		co_await iris_switch(
			static_cast<iris_warp_t<iris_async_worker_t<>>*>(nullptr));
		int fv = free_variable;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		free_variable = fv + 1;
		fv = free_variable;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		free_variable = fv - 1;

		co_await iris_switch(current);  // switch back
	}
};
```

Spawn concurrent Lua coroutines all calling the same pipeline. The warp system guarantees `warp_variable` stays consistent (always 0 at the end), while `free_variable` may race:

```lua
local warp_obj = tutorial_warp_t.new()
local running = coroutine.running()
local complete_count = 0
local loop_count = 20

for i = 1, loop_count do
    coroutine.wrap(function()
        warp_obj:pipeline()           -- yields into C++ coroutine
        complete_count = complete_count + 1
        if complete_count == loop_count then
            coroutine.resume(running)
        end
    end)()
end

if complete_count ~= loop_count then
    coroutine.yield()  -- wait for all workers to complete
end

print("warp_variable = " .. tostring(warp_obj.warp_variable))  -- always 0
print("free_variable = " .. tostring(warp_obj.free_variable))  -- may not be 0
```

### Resource Quotas from Lua

Iristorm's quota system also integrates with Lua coroutines for resource-limited concurrency:

```C++
struct tutorial_quota_t {
	iris_quota_t<size_t, 1> quota;
	iris_quota_queue_t<iris_quota_t<size_t, 1>,
		iris_warp_t<iris_async_worker_t<>>> quota_queue;

	tutorial_quota_t(iris_async_worker_t<>& worker, size_t capacity)
		: quota({ capacity }), quota_queue(worker, quota) {}

	static void lua_registar(iris_lua_t&& lua, std::nullptr_t) {
		lua.set_current<&tutorial_quota_t::pipeline>("pipeline");
		lua.set_current<&tutorial_quota_t::get_remaining>("get_remaining");
	}

	size_t get_remaining() const noexcept { return quota.get()[0]; }

	iris_coroutine_t<void> pipeline(size_t cost) {
		auto* current = co_await iris_switch(
			static_cast<iris_warp_t<iris_async_worker_t<>>*>(nullptr));

		// Acquire quota — waits if insufficient
		auto occupy = co_await quota_queue.guard({ cost });

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		co_await iris_switch(current);
		// quota released automatically when `occupy` goes out of scope
	}
};
```

```lua
local quota_obj = tutorial_quota_t.new()  -- capacity = 100
local running = coroutine.running()
local complete_count = 0

for i = 1, 20 do
    coroutine.wrap(function()
        print("remaining: " .. tostring(quota_obj:get_remaining()))
        quota_obj:pipeline(33)      -- each worker costs 33 units
        complete_count = complete_count + 1
        if complete_count == 20 then
            coroutine.resume(running)
        end
    end)()
end

if complete_count ~= 20 then
    coroutine.yield()
end
```

At most 3 workers run concurrently (3 × 33 = 99 ≤ 100), while a 4th must wait for one to finish.

## Hosted Objects: Manager/Unit Lifetime

A recurring pattern in iris-based projects is a
**Manager** class that manages objects of another class (call them **Unit**),
with the requirement that the Manager instance must **always outlive** every
Unit instance. Instead of hand-enforcing "destroy units before the manager"
(which is error-prone at `lua_close` or when reference cycles exist), the
ordering can be made **structural**, entirely on the Lua side:

1. Register the **Manager metatable FIRST**, then create the Manager instance.
2. Register the **Unit metatable SECOND**, with a `__host` field in the unit
   type table pointing at the Manager instance.
3. Every Unit userdata strongly references its metatable, so while any Unit
   (or the Unit type) lives, the Manager lives:
   `unit userdata → metatable → __host → manager instance`. Dropping the
   script's direct Manager reference changes nothing.
4. When everything dies together (a reference cycle, or `lua_close`), Lua's
   GC finalizes objects in the **reverse order of becoming finalizable** (the
   order their metatables were set; verified on Lua 5.5.1): units (metatable
   set later) are destroyed **before** the Manager (metatable set first).

> **Scope — one-way only:** the `__host` chain is a **one-way** reference
> (unit → manager). In pure Lua this distinction is invisible: the GC
> collects bidirectional cycles naturally, so nobody thinks twice. In a
> Lua/C++ project it is a trap: the C++-side hold is not explicit in the
> script, and a `refptr_t` / `luaL_ref` reference is a GC **root** — a
> "silent" bidirectional binding (the manager holds its units, nothing ever
> removes them) keeps every unit alive until `lua_close`. If the manager
> must hold its units, make it an explicit **add/remove** protocol — see
> below.

Registration (see [tutorial_manager.h](tutorial/lua_co_await/src/tutorial_manager.h)):

```C++
// 1. the Manager (host) metatable is registered FIRST
auto manager_type = lua.make_registry_type<tutorial_manager_t>();
// 2. the host instance is created before any unit exists
auto host = lua.make_registry_object<tutorial_manager_t>();
// 3. the Unit metatable is registered SECOND, carrying __host -> the host
auto unit_type = lua.make_registry_type<tutorial_unit_t>().with(lua, [&](iris_lua_t lua) {
	lua.set_current("__host", host);
});
```

From Lua:

```lua
local unit = unit_type.new()
assert(getmetatable(unit).__host == host)  -- the unit reaches its host
host = nil                                 -- dropping the direct reference...
collectgarbage()                           -- ...changes nothing: the unit
assert(unit:get_host() ~= nil)             -- keeps the host alive
```

The Manager's `lua_finalize` can then assert that every registered unit was
already destroyed — if the order were ever wrong, the assert fires:

```C++
void tutorial_manager_t::lua_finalize(iris_lua_t lua, int index, tutorial_manager_t* p) noexcept {
	// units are finalized BEFORE the host (reverse finalization order)
	IRIS_ASSERT(p->live_registered_units.load() == 0);
}
```

The guarantees above are **independent of how the manager references its
units**: even a cycle anchored through registry references (GC roots) only
dies at `lua_close` — and the destruction order at close is still correct
(verified: units are always finalized before the host). A registry-based
hold simply means "the developer chose to keep the cycle alive until close"
— a resource-management decision, not a defect of this technique.

### The manager → units direction is an ownership decision

The technique only covers the **unit → manager** direction. The reverse
direction is an ownership decision with its own rules:

- The manager does **not** hold its units (one-way binding) — no cycle
  exists at all: units are owned by their natural owners, and the `__host`
  chain alone guarantees the order.
- The manager **does** hold its units (bidirectional binding) — holding a
  unit is an explicit "keep alive" decision, so the system needs an explicit
  **add/remove** protocol: a unit stays alive until it is removed from the
  manager or the manager dies.
- The implementation of the hold decides **when** the cycle can be
  collected: registry references (`refptr_t` / `luaL_ref`) are GC roots, so
  the cycle survives until `lua_close` (the order at close is still
  correct); a GC-visible edge (e.g. a units table in the manager userdata's
  **uservalue slot**, `lua_uservalue_count()`, requires Lua 5.4+; on
  5.1-5.3 the equivalent is the userdata environment table via
  `lua_setfenv`) lets a mid-run `collectgarbage()` collect the whole cycle —
  again units first, host last.
- **Bidirectional without registry roots (per-instance types):** give each
  manager instance its **own** unit type (per-instance types, not global
  registry types) and keep the units ref table in the unit metatable; C++
  then addresses units by an **integer index** into that table, looked up
  on demand through the metatable — no persistent Lua reference is held in
  C++, so no new reference type is needed. (Concept only; the tutorial
  demonstrates the uservalue variant.)

One script-level note when verifying a mid-run teardown from Lua: the final
`collectgarbage()` must run **outside** the function that received the
bundle — while a Lua function runs, its `...` vararg tuple still references
the call arguments, so an in-function collect cannot collect the objects the
function itself is holding.

Full writeup: [docs/05-framework-patterns.md](tutorial/lua_co_await/docs/05-framework-patterns.md),
pattern 7. Runnable demo: `co_await:tutorial_manager():run()` (module
[tutorial_manager](tutorial/lua_co_await/src/tutorial_manager.cpp)).

## Tutorials

- [tutorial/lua_co_await/](tutorial/lua_co_await/) — the fundamental tutorial:
  binding, async wait, warp mutual exclusion, resource quotas, read/write
  fences, plus framework patterns, ECS, frame engines, and DAG dispatch.
  Run with `require("lua_co_await").new():run_tutorials()`.

  The 16 modules (`co_await:tutorial_xxx().new():run()`, all launched together
  by `run_tutorials()`):

  - `tutorial_binding` — binding basics: multi-type parameters, `ref_t` load/save, `refptr_t` object holders
  - `tutorial_async` — async wait: yields the Lua coroutine, sleeps on a worker thread
  - `tutorial_warp` — warp mutual exclusion, contrasted with a deliberate (atomic) race
  - `tutorial_quota` — resource quotas: at most `capacity` units of work in flight
  - `tutorial_readwrite` — read/write fences as race detectors, plus phase barriers for stage switches
  - `tutorial_result` — `result_t<T>` / `result_error_t` error convention (failed results are raised as Lua errors, catch with `pcall`)
  - `tutorial_callback` — embedded callbacks that never yield: the event-sender + two-step (send → poll → apply) pattern
  - `tutorial_module` — module type registry: a `types()` table of registered sub-types
  - `tutorial_object` — object with an automatic method reentrancy guard (`lua_method_begin` / `lua_method_end`)
  - `tutorial_event` — `iris_event_t`: N coroutines waiting for a single `notify()`
  - `tutorial_barrier` — `iris_barrier_t`: N participants released together (rendezvous)
  - `tutorial_luabridge` — cross-VM bridge: values, functions, objects, and cyclic tables between two Lua states
  - `tutorial_system` — ECS: entity allocator, archetype-based component storage, `iterate<...>()` queries
  - `tutorial_engine` — frame-loop engine: barrier frame gate, `iris_pipe_t` frame data, audio → script → render stage warps
  - `tutorial_dispatcher` — DAG dispatcher: `allocate` / `order` / `dispatch`, CRTP completion hooks
  - `tutorial_manager` — Manager/Unit lifetime: the unit metatable's `__host` points at the manager instance, so units keep the manager alive and Lua GC destroys units before the host (asserted by the host's `lua_finalize`; see [Hosted Objects: Manager/Unit Lifetime](#hosted-objects-managerunit-lifetime))

  Systematic documentation (project conventions, design principles, the
  script execution model, workflows, framework patterns):
  [tutorial/lua_co_await/docs/](tutorial/lua_co_await/docs/), start with
  [docs/README.md](tutorial/lua_co_await/docs/README.md). Stress/regression
  scripts live in [scripts/](tutorial/lua_co_await/scripts/).

- [tutorial/lua_event_framework/](tutorial/lua_event_framework/) — a minimal
  event-driven framework built on the same primitives, demonstrating how real
  iris-based projects structure the Lua/C++ interaction:
  a Lua-driven message loop, embedded callbacks that never yield, async work
  forwarded to the worker pool, coroutine pipelines that yield, and results
  polled back in the loop — with no blocking flow and zero external
  dependencies. Run with `lua.exe scripts/Main.lua --auto`. See also its
  [docs](tutorial/lua_event_framework/docs/README.md).

## Files

| File | Description |
|------|-------------|
| [src/iris_common.h](src/iris_common.h) | Common types, macros, and utilities |
| [src/iris_dispatcher.h](src/iris_dispatcher.h) | Warp system, async worker, and DAG dispatcher |
| [src/iris_coroutine.h](src/iris_coroutine.h) | C++20 coroutine support (awaitable, switch, barrier, event, quota) |
| [src/iris_lua.h](src/iris_lua.h) | Lua binding system |
| [src/iris_system.h](src/iris_system.h) | System integration utilities |
| [src/iris_tree.h](src/iris_tree.h) | Tree data structures |
| [test/iris_dispatcher_demo.cpp](test/iris_dispatcher_demo.cpp) | Warp system and DAG dispatcher examples |
| [test/iris_coroutine_demo.cpp](test/iris_coroutine_demo.cpp) | C++ coroutine examples + concurrency stress tests (barrier/quota/pipe races, watchdog-guarded) |
| [test/iris_lua_demo.cpp](test/iris_lua_demo.cpp) | Lua binding examples: traits/bindings, cross-VM transfer, encode/decode, coroutines, proxy views (incl. queue/heap container faces), shared elements |
| [tutorial/lua_co_await/](tutorial/lua_co_await/) | Full Lua + coroutine integration tutorial: 16 runnable modules + docs (see [Tutorials](#tutorials)) |
| [tutorial/lua_co_await/docs/](tutorial/lua_co_await/docs/) | Tutorial docs: instructions, design principles, architecture, workflows, framework patterns |
| [tutorial/lua_event_framework/](tutorial/lua_event_framework/) | Event-driven framework tutorial: receive events → forward to workers → poll results in a non-blocking Lua-side event loop (zero external dependencies, with docs) |

