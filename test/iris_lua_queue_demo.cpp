#include "../src/iris_lua.h"
#include "../src/iris_common.inl" // iris_alloc_aligned/iris_free_aligned for iris_queue_list_t's allocator
#include <cstdio>
#include <queue>
#include <string>

using namespace iris;
using lua_t = iris_lua_t;

// host with the three queue-family members:
// - std::queue: queue face with front/back/push/pop
// - std::priority_queue: heap face with top/push/pop
// - iris_queue_list_t: queue face (front == top, no back), move-only
struct host_t {
	std::queue<int> fifo;
	std::priority_queue<int> heap;
	iris_queue_list_t<int> streaming;
	const std::queue<std::string> labels = std::queue<std::string>(std::deque<std::string>{ "alpha", "beta" });

	static void lua_registar(lua_t lua, std::nullptr_t) {
		lua.set_current_proxy<&host_t::fifo>("fifo");
		lua.set_current_proxy<&host_t::heap>("heap");
		lua.set_current_proxy<&host_t::streaming>("streaming");
		lua.set_current_proxy<&host_t::labels>("labels"); // const member: read-only proxy
	}
};

// minimal host holding a single move-only queue_list, for the cross-state test
struct stream_host_t {
	iris_queue_list_t<int> stream;

	static void lua_registar(lua_t lua, std::nullptr_t) {
		lua.set_current_proxy<&stream_host_t::stream>("stream");
	}
};

static const char* queue_lua_code = R"lua(
local fm = getmetatable(a.fifo)
local hm = getmetatable(a.heap)
local sm = getmetatable(a.streaming)

-- std::queue face: push/front/back/pop/size/empty
fm.push(a.fifo, 10)
fm.push(a.fifo, 20)
fm.push(a.fifo, 30)
assert(#a.fifo == 3)
assert(fm.size(a.fifo) == 3 and fm.empty(a.fifo) == false)
assert(fm.front(a.fifo) == 10 and fm.back(a.fifo) == 30)

-- __len works; __index has no data keys for queue faces
assert(a.fifo[1] == nil)
assert(a.fifo.push == nil) -- methods are metatable-only

fm.pop(a.fifo)
assert(#a.fifo == 2 and fm.front(a.fifo) == 20)

-- clone: independent owned copy, same lua face
local fc = fm.clone(a.fifo)
fm.push(fc, 99)
assert(#fc == 3 and #a.fifo == 2) -- owner unaffected

-- whole-container assignment: lua table -> std::queue member (clear + push)
a.fifo = { 1, 2, 3, 4 }
assert(#a.fifo == 4 and fm.front(a.fifo) == 1 and fm.back(a.fifo) == 4)

-- whole-container assignment: proxy payload -> std::queue member
a.fifo = fc
assert(#a.fifo == 3 and fm.back(a.fifo) == 99)

-- element assignment must error on queue faces
local ok = pcall(function() a.fifo[1] = 5 end)
assert(not ok)

-- std::priority_queue face: push/top/pop
hm.push(a.heap, 5)
hm.push(a.heap, 9)
hm.push(a.heap, 2)
assert(#a.heap == 3)
assert(hm.top(a.heap) == 9)
assert(a.heap[1] == nil)
assert(hm.back == nil) -- heap face has no back

hm.pop(a.heap)
assert(hm.top(a.heap) == 5)

-- heap table assignment keeps heap order
a.heap = { 3, 1, 2 }
assert(hm.top(a.heap) == 3 and #a.heap == 3)

-- heap clone
local hc = hm.clone(a.heap)
hm.pop(hc)
assert(#hc == 2 and #a.heap == 3)

-- clear (drain loop for std containers)
hm.clear(a.heap)
assert(hm.empty(a.heap) == true and #a.heap == 0)

-- pop/front/top on empty must error
local ok2 = pcall(function() return hm.pop(a.heap) end)
assert(not ok2)
local ok3 = pcall(function() return hm.top(a.heap) end)
assert(not ok3)

fm.clear(a.fifo)
local ok4 = pcall(function() return fm.front(a.fifo) end)
assert(not ok4)

-- iris_queue_list_t face: front (== top), push, pop, no back
sm.push(a.streaming, 100)
sm.push(a.streaming, 200)
assert(#a.streaming == 2)
assert(sm.front(a.streaming) == 100)
assert(sm.back == nil)     -- queue_list has no back
assert(sm.clone == nil)    -- move-only container: clone entry is not registered

sm.pop(a.streaming)
assert(#a.streaming == 1 and sm.front(a.streaming) == 200)

-- clear is native on queue_list
sm.clear(a.streaming)
assert(sm.empty(a.streaming) == true)

local ok5 = pcall(function() return sm.front(a.streaming) end)
assert(not ok5)

-- data keys never shadow: queue face has no data key space at all
assert(a.streaming.foo == nil)

-- move-only: whole-container assignment is not registered, so writing a table
-- must not reach C++ (silent no-op on the owner's default __newindex) -- assert
-- the C++ side is untouched rather than expecting an error
a.streaming = { 7, 8, 9 }
assert(sm.empty(a.streaming) == true and #a.streaming == 0)

-- element assignment must error on queue faces
local ok6 = pcall(function() a.streaming[1] = 5 end)
assert(not ok6)

-- multi-node: block_size/ sizeof(int) = 256 elements per node, cross a node
-- boundary to exercise the block-chaining path through the binding
local big = 256 * 2 + 17
for i = 1, big do
	sm.push(a.streaming, i)
end
assert(#a.streaming == big)
assert(sm.size(a.streaming) == big)
assert(sm.front(a.streaming) == 1) -- FIFO head
for i = 1, big do
	assert(sm.front(a.streaming) == i)
	sm.pop(a.streaming)
end
assert(sm.empty(a.streaming) == true)

-- const member: read-only queue proxy (labels = {"alpha", "beta"})
local lm = getmetatable(a.labels)
assert(#a.labels == 2)
assert(lm.front(a.labels) == "alpha")
assert(lm.back(a.labels) == "beta")
assert(lm.push == nil and lm.pop == nil) -- not registered on const views

local ok3 = pcall(function() return lm.pop(a.labels) end)
assert(not ok3 or lm.pop == nil)

-- clone of a const view is an owned mutable copy of the payload
local lc = lm.clone(a.labels)
assert(#lc == 2 and getmetatable(lc).front(lc) == "alpha")

print("queue demo passed")
)lua";

int main(void) {
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	lua_t lua(L);

	auto host_type = lua.make_registry_type<host_t>();

	lua.set_global("a", lua.make_object<host_t>(host_type, host_t()));

	auto result = lua.call<void>(lua.load(queue_lua_code));
	if (!result) {
		fprintf(stderr, "Lua code error: %s\n", result.message.c_str());
		IRIS_ASSERT(false);
	}

	// verify the C++ side sees the mutations
	{
		auto a = lua.get_global<host_t*>("a").value();
		// fifo was cleared by the empty-front test
		IRIS_ASSERT(a->fifo.empty());
		// heap was cleared
		IRIS_ASSERT(a->heap.empty());
		// streaming was cleared
		IRIS_ASSERT(a->streaming.empty());
	}

	// C++-side owned host through a view
	{
		host_t host;
		host.fifo.push(7);
		lua.set_global("b", lua.make_object_view(host_type, &host));
		auto result2 = lua.call<void>(lua.load("\n\
			local m = getmetatable(b.fifo)\n\
			m.push(b.fifo, 8)\n\
			assert(#b.fifo == 2 and m.front(b.fifo) == 7)\n\
			m.pop(b.fifo)\n\
			assert(#b.fifo == 1 and m.front(b.fifo) == 8)\n"));
		if (!result2) {
			fprintf(stderr, "Lua code error: %s\n", result2.message.c_str());
			IRIS_ASSERT(false);
		}
		IRIS_ASSERT(host.fifo.size() == 1 && host.fifo.front() == 8);
	}

	// C++-side view onto a move-only iris_queue_list_t: the proxy must address the
	// host's container in place, and front()/top() must agree (front() forwards top())
	{
		host_t host;
		lua.set_global("c", lua.make_object_view(host_type, &host));
		auto result3 = lua.call<void>(lua.load("\n\
			local m = getmetatable(c.streaming)\n\
			for i = 1, 5 do m.push(c.streaming, i * 10) end\n\
			assert(#c.streaming == 5)\n\
			assert(m.front(c.streaming) == 10)\n\
			m.pop(c.streaming)\n\
			assert(m.front(c.streaming) == 20 and #c.streaming == 4)\n"));
		if (!result3) {
			fprintf(stderr, "Lua code error: %s\n", result3.message.c_str());
			IRIS_ASSERT(false);
		}

		IRIS_ASSERT(host.streaming.size() == 4);
		// C++-side reads see exactly what Lua pushed, in FIFO order
		IRIS_ASSERT(host.streaming.front() == 20);
		IRIS_ASSERT(host.streaming.top() == 20); // front() forwards top()
		size_t expected = 20;
		for (auto&& v : host.streaming) {
			IRIS_ASSERT(v == static_cast<int>(expected));
			expected += 10;
		}
		IRIS_ASSERT(expected == 60);

		// Lua pushes again through the same view, then C++ drains it
		auto result4 = lua.call<void>(lua.load("\n\
			local m = getmetatable(c.streaming)\n\
			m.push(c.streaming, 99)\n\
			assert(m.front(c.streaming) == 20) -- FIFO head unchanged by the tail push\n\
			assert(#c.streaming == 5)\n"));
		if (!result4) {
			fprintf(stderr, "Lua code error: %s\n", result4.message.c_str());
			IRIS_ASSERT(false);
		}

		int drained[4] = { 0 };
		host.streaming.pop(drained, drained + 4);
		IRIS_ASSERT(drained[0] == 20 && drained[1] == 30 && drained[2] == 40 && drained[3] == 50);
		IRIS_ASSERT(host.streaming.front() == 99 && host.streaming.size() == 1);
	}

	// two independent lua states holding proxy views into the SAME C++ container:
	// the documented cross-warp usage shape (one side pushes, the other pops).
	// Container-level SPSC safety is covered by test/iris_dispatcher_demo.cpp;
	// this asserts the binding allows both ends to address one host container.
	{
		stream_host_t shared;

		lua_State* L2 = luaL_newstate();
		luaL_openlibs(L2);
		lua_t lua2(L2);
		auto stream_type = lua.make_registry_type<stream_host_t>();
		auto stream_type2 = lua2.make_registry_type<stream_host_t>();

		lua.set_global("p", lua.make_object_view(stream_type, &shared));
		lua2.set_global("q", lua2.make_object_view(stream_type2, &shared));

		// producer side: state 1 pushes
		auto r1 = lua.call<void>(lua.load("\n\
			local m = getmetatable(p.stream)\n\
			m.push(p.stream, 42)\n\
			m.push(p.stream, 43)"));
		IRIS_ASSERT(!!r1);

		// consumer side: state 2 observes the same container and pops
		auto r2 = lua2.call<void>(lua2.load("\n\
			local m = getmetatable(q.stream)\n\
			assert(#q.stream == 2 and m.front(q.stream) == 42)\n\
			m.pop(q.stream)\n\
			assert(#q.stream == 1 and m.front(q.stream) == 43)"));
		IRIS_ASSERT(!!r2);
		IRIS_ASSERT(shared.stream.size() == 1 && shared.stream.front() == 43);

		lua2.deref(std::move(stream_type2));
		lua.deref(std::move(stream_type));
		lua_close(L2);
	}

	// force a full GC cycle: proxies (incl. move-only queue_list views) must not crash
	lua.call<void>(lua.load("collectgarbage('collect') collectgarbage('collect')"));

	lua.deref(std::move(host_type));
	lua_close(L);
	printf("all passed\n");
	return 0;
}
