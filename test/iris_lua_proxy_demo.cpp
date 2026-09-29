#include "../src/iris_lua.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace iris;
using lua_t = iris_lua_t;

struct account_t {
	std::vector<int> numbers;
	std::map<std::string, int> scores;
	const std::vector<std::string> tags = { "alpha", "beta", "gamma" };

	static void lua_registar(lua_t lua, std::nullptr_t) {
		lua.set_current_proxy<&account_t::numbers>("numbers");
		lua.set_current_proxy<&account_t::scores>("scores");
		lua.set_current_proxy<&account_t::tags>("tags"); // const member: read-only proxy
	}
};

// custom sequence container: no inheritance from any std container,
// just an STL-shaped surface (see docs/design_set_current_proxy.md section 5.1)
template <typename T>
class my_array_t {
public:
	using value_type = T;

	my_array_t() = default;

	size_t size() const { return data.size(); }
	bool empty() const { return data.empty(); }
	void clear() { data.clear(); }
	void resize(size_t n) { data.resize(n); }
	T& operator [] (size_t i) { return data[i]; }
	const T& operator [] (size_t i) const { return data[i]; }
	void push_back(const T& v) { data.push_back(v); }
	void pop_back() { data.pop_back(); }
	template <typename... args_t>
	void emplace_back(args_t&&... args) { data.emplace_back(std::forward<args_t>(args)...); }
	typename std::vector<T>::iterator begin() { return data.begin(); }
	typename std::vector<T>::iterator end() { return data.end(); }
	void erase(typename std::vector<T>::iterator it) { data.erase(it); }
	void insert(typename std::vector<T>::iterator it, const T& v) { data.insert(it, v); }

private:
	std::vector<T> data;
};

// custom map-like container with key_type/mapped_type
template <typename K, typename V>
class my_dict_t {
public:
	using key_type = K;
	using mapped_type = V;

	V& operator [] (const K& k) { return slots[k]; }
	typename std::map<K, V>::iterator find(const K& k) { return slots.find(k); }
	size_t count(const K& k) const { return slots.count(k); }
	size_t erase(const K& k) { return slots.erase(k); }
	size_t size() const { return slots.size(); }
	bool empty() const { return slots.empty(); }
	void clear() { slots.clear(); }
	typename std::map<K, V>::iterator begin() { return slots.begin(); }
	typename std::map<K, V>::iterator end() { return slots.end(); }

private:
	std::map<K, V> slots;
};

struct holder_t {
	my_array_t<int> values;
	my_dict_t<std::string, int> lookup;

	static void lua_registar(lua_t lua, std::nullptr_t) {
		lua.set_current_proxy<&holder_t::values>("values");
		lua.set_current_proxy<&holder_t::lookup>("lookup");
	}
};

struct engine_t {
	int speed = 0;
	std::string name = "idle";
	int doubled_reads = 0;

	int get_speed() const noexcept { return speed; }
	void set_speed(int v) { speed = v; }

	std::string get_label() const { return name + "@" + std::to_string(speed); }

	int get_doubled() { ++doubled_reads; return speed * 2; }

	static void lua_registar(lua_t lua, std::nullptr_t) {
		// custom property: member function getter/setter pair
		lua.set_current_prop<&engine_t::get_speed, &engine_t::set_speed>("speed_rw");
		// read-only via member function (getter only)
		lua.set_current_prop<&engine_t::get_label>("label");
		// getter with side effects
		lua.set_current_prop<&engine_t::get_doubled, &engine_t::set_speed>("doubled");
		// free function getter/setter pair (first arg is the owner)
		lua.set_current_prop<&engine_t::free_get, &engine_t::free_set>("free_rw");

		// stateful functor getter/setter pair (captures scale)
		int scale = 3;
		lua.set_current_prop("scaled",
			[scale](const engine_t* e) { return e->speed * scale; },
			[scale](engine_t* e, int v) { e->set_speed(v / scale); });

		// read-only functor getter
		lua.set_current_prop("readonly_functor", [scale](engine_t* e) { return e->speed + scale; });
	}

	static std::string free_get(engine_t* e) { return e->name; }
	static void free_set(engine_t* e, std::string&& v) { e->name = std::move(v); }
};

static const char* proxy_lua_code = R"lua(
local m = getmetatable(a.numbers)

-- in-place element write: a.numbers[i] = v
a.numbers[1] = 5
a.numbers[2] = 7
assert(a.numbers[1] == 5 and a.numbers[2] == 7)
assert(#a.numbers == 3 and a.numbers[3] == 3)

-- negative index (from the end, lua convention)
assert(a.numbers[-1] == 3)
a.numbers[-1] = 33
assert(a.numbers[3] == 33)

-- methods via metatable only, data keys are never shadowed
m.push_back(a.numbers, 9)
assert(#a.numbers == 4 and a.numbers[4] == 9)
assert(a.numbers.push_back == nil) -- method not in data key space

m.insert(a.numbers, 1, 100)
assert(a.numbers[1] == 100 and #a.numbers == 5)
m.erase(a.numbers, 1)
assert(a.numbers[1] == 5 and #a.numbers == 4)
assert(m.size(a.numbers) == 4 and m.at(a.numbers, 2) == 7)
assert(m.empty(a.numbers) == false)

-- iteration without any iterator protocol
local sum = 0
for i = 1, #a.numbers do
	sum = sum + a.numbers[i]
end
assert(sum == 5 + 7 + 33 + 9)

-- clone: independent owned copy, still an operable proxy
local c = m.clone(a.numbers)
local cm = getmetatable(c)
assert(cm == m) -- same metatable / same lua face
m.push_back(c, 11)
assert(#c == 5 and #a.numbers == 4) -- owner unaffected
c[1] = 500
assert(a.numbers[1] == 5)

-- whole-container assignment: proxy payload -> member
a.numbers = c
assert(#a.numbers == 5 and a.numbers[1] == 500 and a.numbers[5] == 11)

-- whole-container assignment: lua table -> member
a.numbers = { 1, 2, 3 }
assert(#a.numbers == 3 and a.numbers[2] == 2)

-- map face
local sm = getmetatable(a.scores)
a.scores["alice"] = 10
a.scores["bob"] = 20
assert(a.scores["alice"] == 10 and #a.scores == 2)
assert(a.scores["carol"] == nil)
assert(sm.count(a.scores, "bob") == 1)

sm.insert(a.scores, "carol", 30)
assert(a.scores["carol"] == 30)

-- snapshot iteration (keys/values/items)
local keys = sm.keys(a.scores)
assert(#keys == 3 and keys[1] == "alice" and keys[3] == "carol")

local total = 0
for i, entry in ipairs(sm.items(a.scores)) do
	total = total + entry[2]
end
assert(total == 60)

sm.erase(a.scores, "bob")
assert(sm.count(a.scores, "bob") == 0 and #a.scores == 2)

-- data keys are never shadowed: store entries named like methods
a.scores["keys"] = 99
assert(a.scores["keys"] == 99) -- still data, not the method
assert(sm.count(a.scores, "keys") == 1)

-- const member: read-only proxy
local tm = getmetatable(a.tags)
assert(#a.tags == 3 and a.tags[2] == "beta")
local cloned = tm.clone(a.tags) -- clone of a const view is an owned copy
assert(#cloned == 3 and cloned[1] == "alpha")

local ok, err = pcall(function()
	a.tags[1] = "x" -- __newindex must not exist on a const proxy
end)
assert(not ok)

local ok2 = pcall(function()
	return a.numbers[100] -- out of range
end)
assert(not ok2)

-- custom containers with an STL-shaped surface are indistinguishable
-- from std containers on the lua side (full contract, no probing)
local vm = getmetatable(h.values)
h.values[1] = 10
h.values[2] = 20
vm.push_back(h.values, 30)
assert(#h.values == 4 and h.values[1] == 10 and h.values[4] == 30)
vm.insert(h.values, 1, 5)
assert(h.values[1] == 5 and #h.values == 5)
vm.erase(h.values, 1)
assert(h.values[1] == 10 and #h.values == 4)
local cc = vm.clone(h.values)
vm.push_back(cc, 99)
assert(#cc == 5 and #h.values == 4)

local dm = getmetatable(h.lookup)
h.lookup["a"] = 1
h.lookup["b"] = 2
assert(#h.lookup == 2 and h.lookup["a"] == 1)
assert(dm.count(h.lookup, "b") == 1)
local ckeys = dm.keys(h.lookup)
assert(#ckeys == 2 and ckeys[1] == "a" and ckeys[2] == "b")

-- custom properties: member getter/setter pair
assert(e.speed_rw == 0)
e.speed_rw = 7
assert(e.speed_rw == 7 and e.label == "idle@7")

-- read-only property: assignment must fail
local pok = pcall(function() e.label = "x" end)
assert(not pok)

-- getter with side effects, sharing the setter
assert(e.doubled == 14)
e.doubled = 10
assert(e.doubled == 20 and e.speed_rw == 10)

-- free function getter/setter
assert(e.free_rw == "idle")
e.free_rw = "renamed"
assert(e.free_rw == "renamed" and e.label == "renamed@10")

-- wrong value type must error, not corrupt
local pok2 = pcall(function() e.speed_rw = {} end)
assert(not pok2)

-- stateful functor getter/setter (scale = 3)
assert(e.scaled == 30)
e.scaled = 36
assert(e.speed_rw == 12 and e.scaled == 36)

-- read-only functor getter
assert(e.readonly_functor == 15)
local pok3 = pcall(function() e.readonly_functor = 1 end)
assert(not pok3)

print("proxy demo passed")
)lua";
int main(void) {
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	lua_t lua(L);

	auto account_type = lua.make_registry_type<account_t>();
	auto holder_type = lua.make_registry_type<holder_t>();
	auto engine_type = lua.make_registry_type<engine_t>();

	// lua-owned host
	lua.set_global("a", lua.make_object<account_t>(account_type, account_t()));
	{
		auto a = lua.get_global<account_t*>("a").value();
		a->numbers = { 1, 2, 3 };
		a->scores["pre"] = 1;
		a->scores.erase("pre");
	}

	// custom-container host, pre-filled with 3 elements
	lua.set_global("h", lua.make_object<holder_t>(holder_type, holder_t()));
	{
		auto h = lua.get_global<holder_t*>("h").value();
		h->values.push_back(1);
		h->values.push_back(2);
		h->values.push_back(3);
	}

	// custom-property host
	lua.set_global("e", lua.make_object<engine_t>(engine_type, engine_t()));

	auto result = lua.call<void>(lua.load(proxy_lua_code));
	if (!result) {
		fprintf(stderr, "Lua code error: %s\n", result.message.c_str());
		IRIS_ASSERT(false);
	}

	// verify C++ side sees the mutations
	{
		auto a = lua.get_global<account_t*>("a").value();
		IRIS_ASSERT(a->numbers.size() == 3);
		IRIS_ASSERT(a->numbers[0] == 1 && a->numbers[1] == 2 && a->numbers[2] == 3);
		IRIS_ASSERT(a->scores.at("alice") == 10 && a->scores.at("carol") == 30);
		IRIS_ASSERT(a->scores.at("keys") == 99);
		IRIS_ASSERT(a->scores.size() == 3);

		auto h = lua.get_global<holder_t*>("h").value();
		IRIS_ASSERT(h->values.size() == 4 && (*h->values.begin()) == 10);
		IRIS_ASSERT(h->lookup["a"] == 1 && h->lookup["b"] == 2);

		auto e = lua.get_global<engine_t*>("e").value();
		IRIS_ASSERT(e->speed == 12 && e->doubled_reads == 2 && e->name == "renamed");
	}

	// C++-side owned host through a view
	{
		account_t host;
		host.numbers = { 7 };
		lua.set_global("b", lua.make_object_view(account_type, &host));
		auto result2 = lua.call<void>(lua.load("\n\
			b.numbers[1] = 8\n\
			getmetatable(b.numbers).push_back(b.numbers, 9)\n\
			assert(#b.numbers == 2 and b.numbers[1] == 8)\n"));
		if (!result2) {
			fprintf(stderr, "Lua code error: %s\n", result2.message.c_str());
			IRIS_ASSERT(false);
		}
		IRIS_ASSERT(host.numbers.size() == 2 && host.numbers[0] == 8 && host.numbers[1] == 9);
	}

	// force a full GC cycle: proxies must not crash and must release their anchors
	lua.call<void>(lua.load("collectgarbage('collect') collectgarbage('collect')"));

	lua.deref(std::move(engine_type));
	lua.deref(std::move(holder_type));
	lua.deref(std::move(account_type));
	lua_close(L);
	printf("all passed\n");
	return 0;
}



