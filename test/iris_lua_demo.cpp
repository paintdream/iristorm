#include "../src/iris_lua.h"
#include "../src/iris_common.inl"
#include <map>
#include <chrono>
#include <cstdio>
#include <queue>
#include <string>
#include <vector>

#if USE_LUA_COROUTINE
#include "../src/iris_coroutine.h"
#endif
using namespace iris;

#if USE_LUA_COROUTINE
using worker_t = iris_async_worker_t<std::thread, std::function<void()>>;
using warp_t = iris_warp_t<worker_t>;

static warp_t* warpptr = nullptr;
static warp_t* warpptr2 = nullptr;
static worker_t* workerptr = nullptr;
#else
using warp_t = void;
#endif

using lua_t = iris_lua_t;

// ---- merged sub-tests (see main) ----
static void test_overload_cast(lua_t lua);
static void test_registry_types(lua_t lua);
static void test_functions(lua_t lua);
static void test_cross_vm(lua_t lua);
static void test_encode(lua_t lua);
static void test_bindings(lua_t lua);
static void test_coroutines(lua_t lua);
static void test_proxy_views(lua_t lua);
static void test_queue_faces(lua_t lua);
static void test_shared_elements(lua_t lua);

struct vector3 {
	float x, y, z;
};

template <>
struct iris::iris_lua_traits_t<vector3> : std::true_type {
	using type = iris::iris_lua_traits_t<vector3>;

	static int lua_tostack(lua_State* L, vector3&& v) noexcept {
		lua_newtable(L);
		lua_pushnumber(L, v.z);
		lua_pushnumber(L, v.y);
		lua_pushnumber(L, v.x);
		lua_rawseti(L, -4, 1);
		lua_rawseti(L, -3, 2);
		lua_rawseti(L, -2, 3);
		return 1;
	}

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

struct number_t {
	virtual ~number_t() {}
};

struct complex_t : number_t {
	double real = 0;
	double image = 0;
};

template <>
struct iris::iris_lua_traits_t<complex_t> : lua_t::traits_trivial_t<complex_t> {};

template <typename type_t>
struct crtp_t {
	void crtp_foo() {
		printf("CRTP foo()!\n");
	}

	int crtp_member = 5;
};

struct example_base_t : crtp_t<example_base_t> {
	template <typename traits_t>
	static void lua_registar(lua_t lua, traits_t) {
		lua.set_current<&example_base_t::crtp_foo, example_base_t>("crtp_foo");
		lua.set_current<&example_base_t::crtp_member, example_base_t>("crtp_member");
		lua.set_current<&example_base_t::base_value>("base_value");
		lua.set_current<&example_base_t::base_func>("base_func");
		lua.set_current<&example_base_t::base_bind_static>("base_bind_static", 1.0);
		lua.set_current<&example_base_t::base_bind>("base_bind", 1.0);
	}

	void base_bind(double value) {
		printf("base bind\n");
	}

	static void base_bind_static(double value) {
		printf("base bind static\n");
	}

	void base_func() {
		printf("base func\n");
	}

	static constexpr const char* lua_typename() noexcept {
		return "example_base_t";
	}

	static void lua_view_initialize(lua_t lua, int index, void* t) {
		example_base_t** p = (example_base_t**)t;
		p[1] = p[0];
		p[0] = (example_base_t*)(size_t)0x1234;
		printf("view initialize\n");
	}

	static void lua_view_finalize(lua_t lua, int index, void* p) {
		printf("view join\n");
	}

	static size_t lua_view_payload(lua_t lua, void* p) {
		printf("view payload\n");
		return sizeof(size_t);
	}

	static example_base_t* lua_view_extract(lua_t lua, int index, void* t) {
		example_base_t** p = (example_base_t**)t;
		printf("view extractor\n");
		return ((example_base_t**)p)[1];
	}

	int base_value = 2222;
};

static std::atomic<int> ref_count = 0;
struct example_t : example_base_t {
	example_t(float f, int v) noexcept {
		printf("custom constructor %f %d\n", f, v);
	}

	static void lua_registar(lua_t lua, std::nullptr_t) {
		lua.set_current_new<&lua_t::place_new_object<example_t, float, int>>("new", 1.4f);
		//lua.set_current_new<&lua_t::trivial_object_creator>("new_org", 1.4f);
		lua.set_current("lambda", [lua](int v) mutable {
			IRIS_ASSERT(v == 4);
			return 4;
		});
		lua.set_current<&example_t::value>("value");
		lua.set_current<iris_add_member_const(&example_t::value_as_const)> ("value_as_const");
		lua.set_current<&example_t::const_value>("const_value");
		lua.set_current<&example_t::accum_value>("accum_value");
		lua.set_current<&example_t::join_value>("join_value");
		lua.set_current<&example_t::join_value_by_value>("join_value_by_value");
		lua.set_current<&example_t::join_value_by_const_value>("join_value_by_const_value");
		lua.set_current<&example_t::join_value_by_const_ref>("join_value_by_const_ref");
		lua.set_current<&example_t::join_value_by_rvalue_ref>("join_value_by_rvalue_ref");
		lua.set_current<&example_t::join_value_required>("join_value_required");
		lua.set_current<&example_t::join_value_refptr>("join_value_refptr");
		lua.set_current<&example_t::join_value_required_refptr>("join_value_required_refptr");
		lua.set_current<&example_t::var>("var");
		lua.set_current<&example_t::get_value>("get_value");
		lua.set_current<&example_t::get_value_raw>("get_value_raw");
		lua.set_current<&example_t::get_value_raw_lambda>("get_value_raw_lambda");
		lua.set_current<&example_t::call>("call");
		lua.set_current<&example_t::forward_pair>("forward_pair");
		lua.set_current<&example_t::forward_tuple>("forward_tuple");
		lua.set_current<&example_t::forward_tuple_raw>("forward_tuple_raw");
		lua.set_current<&example_t::forward_map>("forward_map");
		lua.set_current<&example_t::forward_vector>("forward_vector");
		lua.set_current<&example_t::prime>("prime");
		lua.set_current<&example_t::get_vector3>("get_vector3");
		lua.set_current<&example_t::native_call>("native_call");
		lua.set_current<&example_t::native_call_noexcept>("native_call_noexcept");
		lua.set_current_overload<iris_overload_cast<int>(&example_t::overload_func)>("overload_func");
		lua.set_current_overload<iris_overload_cast<int, int>(&example_t::overload_func)>("overload_func");
		lua.set_current_prop<&example_t::get_prop_value, &example_t::set_prop_value>("prop_value");
#if USE_LUA_COROUTINE
		lua.set_current_overload<iris_overload_cast<iris::iris_coroutine_t<int>, const std::string&>(&example_t::coro_get_int)>("coro_get_int");
		lua.set_current_overload<iris_overload_cast<iris::iris_coroutine_t<int>, const std::string&, int>(&example_t::coro_get_int)>("coro_get_int");
		lua.set_current<&example_t::coro_get_none>("coro_get_none");
		lua.set_current<&example_t::mem_coro_get_int>("mem_coro_get_int");
		lua.set_current<&example_t::mem_coro_get_int_raw>("mem_coro_get_int_raw");
#endif
	}

	static void lua_method_begin(lua_t lua, example_t* self, bool is_coroutine) {
		printf("lua_method_begin %d\n", is_coroutine);
	}

	static void lua_method_end(lua_t lua, example_t* self, bool is_coroutine) {
		printf("lua_coroutine_end %d\n", is_coroutine);
	}

	example_t() noexcept {}
	example_t(const example_t& rhs) noexcept {
		printf("copy construct!!\n");
		value = rhs.value;
	}
	example_t(example_t&& rhs) noexcept {
		printf("move construct!!\n");
		value = rhs.value;
	}

	static constexpr const char* lua_typename() noexcept {
		return "example_t";
	}

	template <typename init_t>
	static size_t lua_sizeof(init_t&& i) noexcept {
		return sizeof(example_t) + sizeof(void*);
	}

	int call(lua_t lua, lua_t::ref_t&& r, int value) {
		printf("call type %d\n", r.get_type(lua));
		auto result = lua.call<int>(r, value);
		lua.deref(std::move(r));

		return result.value_or(0);
	}

	static int native_call(lua_State* L) {
		printf("native call!\n");
		return 0;
	}

	static int native_call_noexcept(lua_State* L) noexcept {
		printf("native call noexcept!\n");
		return 0;
	}

	static void lua_initialize(lua_t lua, int index, example_t* p) {
		ref_count.fetch_add(1, std::memory_order_acquire);
		printf("initialize!\n");
	}

	static void lua_finalize(lua_State* L, int index, example_t* p) noexcept {
		printf("join!\n");
		ref_count.fetch_sub(1, std::memory_order_release);
	}

	int get_value() noexcept {
		return value;
	}

	int overload_func() {
		printf("overload 1\n");
		return 1;
	}

	int overload_func(int) {
		printf("overload 2\n");
		return 2;
	}

	static int get_value_raw(lua_State* L) {
		return lua_t::forward(L, &example_t::get_value);
	}

	static int get_value_raw_lambda(lua_State* L) {
		return lua_t::forward(L, [L](lua_t lua, example_t* t) {
			bool same = L == lua.get_state();
			IRIS_ASSERT(same);
			return t->get_value();
		});
	}

	void join_value(const example_t* rhs) noexcept {
		if (rhs != nullptr) {
			value += rhs->value;
		}
	}

	// verify cast_arg_type_t handles registered types uniformly across every
	// parameter form: by value, const by value, const reference, and rvalue ref.
	void join_value_by_value(example_t rhs) noexcept {
		value += rhs.value;
	}

	void join_value_by_const_value(const example_t rhs) noexcept {
		value += rhs.value;
	}

	void join_value_by_const_ref(const example_t& rhs) noexcept {
		value += rhs.value;
	}

	void join_value_by_rvalue_ref(example_t&& rhs) noexcept {
		value += rhs.value;
	}

	static lua_t::native_variadic_t var(lua_t lua, lua_t::native_variadic_t&& params) {
		printf("Variadic param count: %d, index: %d\n", params.get_count(), params.get_index());
		lua.native_push_variable(params.get_count());
		lua.native_push_variable(params.get_index());
		return lua_t::native_variadic_t(0);
	}

	void join_value_required(lua_t::required_t<example_t*>&& rhs) noexcept {
		printf("Required!\n");
		value += rhs.get()->value;
	}

	void join_value_required_refptr(lua_t&& lua, lua_t::required_t<lua_t::refptr_t<example_t>>&& rhs) noexcept {
		printf("Required ptr! \n");
		value += rhs.get().get()->value;
		lua.deref(std::move(rhs.get()));
	}

	void join_value_refptr(lua_t&& lua, lua_t::refptr_t<example_t>&& rhs) noexcept {
		auto guard = lua.ref_guard(rhs);
		if (rhs) {
			value += rhs->value;
		}
	}

	vector3 get_vector3(const vector3& input) noexcept {
		return input;
	}

	int accum_value(int init) noexcept {
		return value += init;
	}

	static std::tuple<int, std::string> forward_tuple(std::tuple<int, std::string>&& v) {
		std::get<0>(v) = std::get<0>(v) + 1;
		return v;
	}

	static int forward_tuple_raw(lua_State* L) {
		return lua_t::forward(L, &example_t::forward_tuple);
	}

	static std::pair<int, std::string> forward_pair(std::pair<int, std::string>&& v) {
		v.first += 1;
		return v;
	}

	std::map<std::string, int> forward_map(std::map<std::string, int>&& v) {
		v["abc"] = 123;
		return v;
	}

	std::vector<std::string> forward_vector(std::vector<std::string>&& v) {
		v.emplace_back("str");
		return std::move(v);
	}

	lua_t::ref_t prime(lua_t lua) {
		return lua.make_table([](lua_t lua) noexcept {
			lua.set_current("name", "prime");
			lua.set_current(1, 2);
			lua.set_current(2, 3);
			lua.set_current(3, 5);
		});
	}

#if USE_LUA_COROUTINE
	static iris::iris_coroutine_t<int> coro_get_int(const std::string& s) noexcept {
		co_return 1;
	}

	static iris::iris_coroutine_t<int> coro_get_int(const std::string& s, int) noexcept {
		co_return 2;
	}

	static iris::iris_coroutine_t<> coro_get_none() noexcept {
		workerptr->terminate();
		co_return;
	}

	iris::iris_coroutine_t<lua_t::optional_result_t<int>> mem_coro_get_int(iris_lua_t&& t, std::string&& s) noexcept {
		// co_return lua_t::result_error_t("test error 1");
		iris_lua_t tt = t;
		co_await iris::iris_switch(warpptr2);
		co_await iris::iris_switch(warpptr);
		tt.native_push_variable(1);
		// co_return lua_t::result_error_t("test error 2");
		co_return 2;
	}

	static int mem_coro_get_int_raw(lua_State* L) {
		return lua_t::forward(L, &example_t::mem_coro_get_int);
	}
#endif

	std::string_view get_prop_value() const {
		return prop_value;
	}

	void set_prop_value(std::string&& value) {
		prop_value = std::move(value);
	}

	int value = 10;
	const int const_value = 0;
	int value_as_const = 2;
	std::string prop_value = "base prop value";
};

struct shared_data_t : std::enable_shared_from_this<shared_data_t> {
	static constexpr const char* lua_typename() noexcept {
		return "shared_data_t";
	}

	std::string data = "shared data";
};

template <typename type_t>
struct iris::iris_lua_traits_t<type_t, std::enable_if_t<std::is_base_of_v<std::enable_shared_from_this<type_t>, type_t>>> {
	using type = iris::iris_lua_traits_t<type_t>;
	static constexpr int lua_uservalue_count = 0;
	static_assert(alignof(std::weak_ptr<type_t>) <= alignof(type_t*), "Unexpected alignment.");

	static void lua_view_initialize(lua_t lua, int index, void* t) {
		type_t** p = reinterpret_cast<type_t**>(t);
		new (p + 1) std::weak_ptr<type_t>((*p)->weak_from_this());
	}

	static void lua_view_finalize(lua_t lua, int index, void* t) {
		type_t** p = reinterpret_cast<type_t**>(t);
		reinterpret_cast<std::weak_ptr<type_t>*>(p + 1)->~weak_ptr();
	}

	static size_t lua_view_payload(lua_t lua, void* p) {
		return sizeof(std::weak_ptr<type_t>);
	}

	static type_t* lua_view_extract(lua_t lua, int index, void* t) {
		type_t** p = reinterpret_cast<type_t**>(t);
		auto* ptr = reinterpret_cast<std::weak_ptr<type_t>*>(p + 1);
		return ptr->expired() ? nullptr : *p;
	}
};

static int error_handler(lua_State* L) {
	return lua_t::forward(L, [](std::string_view message) {
		printf("ERROR_HANDLER %s\n", message.data());
	});
}

static void use_expired(shared_data_t* t, shared_data_t* s) {
	printf("expired object: %p\nhold object: %p\n", t, s);
}

static void env_test(std::string title, std::string hi) {
	printf("env_test %s - %s\n", title.c_str(), hi.c_str());
}

struct shared_object_example_t : lua_t::shared_local_object_t<shared_object_example_t> {
	static void lua_registar(lua_t lua, std::nullptr_t) {
		lua.set_current<&shared_object_example_t::foo, shared_object_example_t>("foo");
		lua.set_current<&shared_object_example_t::other, shared_object_example_t>("other");
		lua.set_current_new<&lua_t::shared_new_object<shared_object_example_t, int>>("make", 2);
	}

	shared_object_example_t() : index(0) {}
	shared_object_example_t(int i) : index(i) {}

	~shared_object_example_t() {
		printf("shared_object_example_t destruct %d %p\n", index, this);
	}

	shared_object_example_t* foo(lua_t::shared_ref_t<shared_object_example_t> ptr, lua_t::required_t<lua_t::shared_ref_t<shared_object_example_t>> req, shared_object_example_t* ptr2) {
		printf("foo: %p\n", other.get());
		return this;
	}

	static constexpr const char* lua_typename() noexcept {
		return "shared_object_example_t";
	}

	lua_t::shared_ref_t<shared_object_example_t> other;
	int index;
};

struct shared_object_example_sub_t : shared_object_example_t {
	static constexpr const char* lua_typename() noexcept {
		return "shared_object_example_sub_t";
	}
};

struct test_overload_t {
	void foo() const {}
	void foo(int) const {}
	static void bar() noexcept {}
	static void bar(int) noexcept {}
};

static void test_overload_cast(lua_t lua) {
	constexpr auto castfoo = iris_overload_cast<void, int>(&test_overload_t::foo);
	constexpr auto castfoo2 = iris_overload_cast<void>(&test_overload_t::foo);
	//constexpr auto q = iris_overload_cast<void>(&test_overload_t::foo);
	static_assert(!std::is_same_v<decltype(castfoo), decltype(castfoo2)>);

	constexpr auto castbar = iris_overload_cast<void, int>(&test_overload_t::bar);
	constexpr auto castbar2 = iris_overload_cast<void>(&test_overload_t::bar);
	//constexpr auto q = iris_overload_cast<void>(&test_overload_t::bar);
	static_assert(!std::is_same_v<decltype(castbar), decltype(castbar2)>);
}

struct test_refl {
	int operator () (int a, double b) noexcept { return 1; }
};

static void test_registry_types(lua_t lua) {
	complex_t cpx;
	cpx.image = 1.0;
	cpx.real = 2.0;
	auto complexType = lua.make_registry_type<complex_t>();
	auto getType = lua.get_registry_type<complex_t>();
	lua.deref(std::move(getType));
	lua.deref(std::move(complexType));
	lua.set_global("test_complex", cpx);
	auto cpxget = lua.get_global<complex_t>("test_complex");
	lua.set_global("test_complex_view", lua.make_registry_object_view((number_t*)&cpx));

	test_refl functor;
	auto r1 = lua.set_global("test_reflection", functor);
	auto r2 = lua.set_global<&example_t::accum_value>("test_reflection2");

	auto printTable = lua.load("for i, v in pairs(...) do print(v) end");
	lua.call<void>(printTable, lua.call<lua_t::ref_t>(r1));
	lua.call<void>(printTable, lua.call<lua_t::ref_t>(r2));
	lua.deref(std::move(*printTable));

	auto shared_object_example_type = lua.make_registry_type<shared_object_example_t>();
	auto shared_ptr_instance = lua_t::shared_ref_t<shared_object_example_t>::make(1);
	lua.set_global("shared_ptrinstance", std::move(shared_ptr_instance));
	lua.call<void>(lua.load("function make_instance(type) return type.make() end"));
	auto from_make = lua.call<lua_t::shared_ref_t<shared_object_example_t>>(lua.get_global<lua_t::ref_t>("make_instance"), shared_object_example_type);
	IRIS_ASSERT(from_make.value());
	{
		auto instance_copy1 = lua.get_global<lua_t::shared_ref_t<shared_object_example_t>>("shared_ptrinstance").value();
		shared_object_example_t& ref_instance = *instance_copy1;
		printf("ref_instance %p\n", &ref_instance);

		auto casted = instance_copy1.cast_static<shared_object_example_sub_t>();
		auto instance_copy2 = instance_copy1;
		lua.set_global("shared_deref_auto", std::move(instance_copy1));

		auto instance_construct = lua_t::shared_ref_t<shared_object_example_t>::make(2);
		instance_construct->foo(nullptr, nullptr, nullptr);

		auto instance_offline = lua_t::shared_ref_t<shared_object_example_t>::make(3);
		lua.set_global("registry_instance", std::move(instance_offline));
		IRIS_ASSERT(lua.get_global<shared_object_example_t*>("registry_instance").value() != nullptr);
	
		{
			auto instance_offline2 = lua_t::shared_ref_t<shared_object_example_t>::make(4);
			lua.set_global("success_instance", lua.make_object_view(shared_object_example_type, instance_offline2.get()));
			IRIS_ASSERT(lua.get_global<shared_object_example_t*>("success_instance").value() != nullptr);
		}
	}

	lua.deref(std::move(shared_object_example_type));
	lua.set_global<&env_test>("env_test", "lua_env");
	lua.call<void>(lua.get_global<lua_t::ref_t>("env_test"), "extra");
	lua.set_global<&use_expired>("use_expired");
	std::shared_ptr<shared_data_t> expired_object = std::make_shared<shared_data_t>();
	std::shared_ptr<shared_data_t> hold_object = std::make_shared<shared_data_t>();
	auto shared_data_type = lua.make_type<shared_data_t>();
	lua.set_global("expired_instance", lua.make_object_view(shared_data_type, expired_object.get()));
	lua.set_global("hold_instance", lua.make_object_view(shared_data_type, hold_object.get()));
	lua.deref(std::move(shared_data_type));
	expired_object = nullptr;
	lua.call<void>(lua.load("use_expired(expired_instance, hold_instance)"));

	lua.deref(iris_lua_t::ref_t().once(lua, [](iris_lua_t lua) -> iris_lua_t::ref_t {
		return lua.make_table([](iris_lua_t lua) { lua.set_current("once", 1); });
	}).with(lua, [](iris_lua_t lua) {
		printf("once value = %d\n", lua.get_current<int>("once"));
	}));

	auto example_base_type = lua.make_type<example_base_t>();
	auto example_type = lua.make_registry_type<example_t>(std::move(example_base_type));
	// lua.cast_type(std::move(example_base_type), example_type);
	lua.set_global("example_t", std::move(example_type));
	lua.set_global("refstr", "shared_string_object");
	auto p = lua.get_global<lua_t::refview_t<std::string_view>>("refstr").value();
	std::map<int, lua_t::refview_t<std::string_view>> vv;
	vv[1234] = std::move(p);
	for (auto&& v : vv) {
		lua.deref(std::move(v.second));
	}
}

static void test_functions(lua_t lua) {
	struct lambda {
		lambda() {
			printf("lambda constructor!\n");
		}

		lambda(lambda&& l) noexcept {
			printf("lambda move constructor!\n");
		}
		
		lambda(const lambda&) = delete;

		~lambda() {
			printf("lambda destructor!\n");
		}

		int operator () () {
			return 3;
		}
	};

	auto result = lua.call<void>(lua.load("local function error_entry() error('must be error') end; local function error_entry2() error_entry() end; error_entry2()"));
	IRIS_ASSERT(!result);
	printf("Error with stack: %s\n", result.message.data());

	lua.set_global("fmt_string", lua.make_string("hello %s", "world!"));
	IRIS_ASSERT(lua.get_global<std::string>("fmt_string") == "hello world!");

	lua.set_global("fmt_string_lambda", lua.make_string([](auto&& buff) {
		buff << "hello world!";
	}));

	IRIS_ASSERT(lua.get_global<std::string>("fmt_string_lambda") == "hello world!");

	int capture = 2;
	lua.set_global("functor", [capture]() mutable noexcept {
		return capture;
	});
	int retcapture = lua.call<int>(lua.get_global<iris_lua_t::ref_t>("functor")).value();
	IRIS_ASSERT(retcapture == capture);

	lua.set_global("functor2", lambda());
	int retcapture2 = lua.call<int>(lua.get_global<iris_lua_t::ref_t>("functor2")).value();
	IRIS_ASSERT(retcapture2 == 3);
}

static void test_cross_vm(lua_t lua) {
	lua_State* T = luaL_newstate();
	luaL_openlibs(T);
	lua_t target(T);

	lua.native_push_variable([T](const char* text) mutable noexcept {
		printf("No metatable %s\n", text);
		return "no metatable";
	});

	lua.native_cross_transfer_variable<true>(target, -1);
	auto f = target.native_get_variable<lua_t::ref_t>(-1);
	target.native_pop_variable(1);
	target.call<void>(std::move(f), "transferred!");

	lua.native_push_variable(lua.load("print('transfer function')"));
	lua.native_cross_transfer_variable<true>(target, -1);
	auto ft = target.native_get_variable<lua_t::ref_t>(-1);
	target.native_pop_variable(1);
	target.call<void>(std::move(ft));

	auto callResult = target.call<void>(target.load("\n\
function test(a, b, c) \n\
	print('cross value ' .. b.value) \n\
	b:base_func() \n\
	b:base_bind() \n\
	b.base_bind_static() \n\
	b.prop_value = 'new prop value' \n\
	print('prop value: ' .. b.prop_value) \n\
	assert(b.prop_value == 'new prop value') \n\
	b:crtp_foo() \n\
	print('equal value ======== ' .. tostring(b == c)) \n\
	print('base value ======== ' .. tostring(b.base_value)) \n\
	print('cross ' .. tostring(a)) \n\
	print('lambda value ' .. b.lambda(4)) \n\
	print('cross value ' .. c.value) \n\
	c.value = 3333 \n\
	print('cross value ' .. c.value) \n\
	return a \n\
end\n\
function test2() \n\
	local a = { b = {}, text = 'text' } \n\
	a.b.self = a\n\
	return a \n\
end\n\
").value());
	if (!callResult) {
		fprintf(stderr, "Lua code error: %s\n", callResult.message.c_str());
		IRIS_ASSERT(false);
	}

	lua_t::ref_t test = target.get_global<lua_t::ref_t>("test").value();
	example_t existing_object;
	existing_object.value = 2222;
	auto temp_type = target.make_registry_type<example_t>();
	{
		temp_type.make_cast(target, target.make_type<example_base_t>());
	}

	auto callResultProperty = target.call<void>(test, "existing", target.make_registry_object_view<example_t>(&existing_object), target.make_object_view<example_t>(temp_type, &existing_object));
	if (!callResultProperty) {
		fprintf(stderr, "Lua code error: %s\n", callResultProperty.message.c_str());
		IRIS_ASSERT(false);
	}

	target.deref(std::move(temp_type));
	IRIS_ASSERT(existing_object.value == 3333);
	existing_object.value = 2222;

	lua.native_push_variable(1234);
	lua.native_push_variable(lua.make_registry_object<example_t>());
	lua.native_push_variable(lua.make_object_view<example_t>(lua.make_type<example_t>(), &existing_object));
	lua.native_cross_transfer_variable<true>(target, -3);
	lua.native_cross_transfer_variable<true>(target, -2);
	lua.native_cross_transfer_variable<false>(target, -1);
	auto* g = target.native_get_variable<example_t*>(-1);
	lua.native_pop_variable(3);
	target.clear_registry_type<example_t>();

	int result = target.native_call(std::move(test), 3).value();
	IRIS_ASSERT(existing_object.value == 3333);
	int ret_val = target.native_get_variable<int>(-1);
	IRIS_ASSERT(ret_val == 1234);
	target.native_pop_variable(1);

	auto test2 = target.get_global<lua_t::ref_t>("test2");
	int result2 = target.native_call(std::move(test2), 0).value();
	target.native_cross_transfer_variable<true>(lua, -1);
	target.native_pop_variable(1);
	lua_close(T);

	auto callResult2 = lua.call<void>(lua.load("\n\
	function print2(a) \n\
		print(a.text) \n\
		assert(a.b.self == a) \n\
	end\n").value());

	if (!callResult2) {
		fprintf(stderr, "Lua code error: %s\n", callResult2.message.c_str());
		IRIS_ASSERT(false);
	}
}

static void test_encode(lua_t lua) {
	auto encode_text = lua.encode<std::string>("haha");
	auto decode_text = lua.decode<iris_lua_t::ref_t>(std::move(encode_text)).value();
	auto decode_textview = decode_text.as<std::string_view>(lua);
	IRIS_ASSERT(decode_textview == "haha");
	lua.deref(std::move(decode_text));

	auto complex = lua.make_table([](lua_t lua) {
		lua.set_current("key", "value");
		lua.set_current("recursion", "recursion");
		lua.set_current(1, true);
		lua.set_current(2, 2);
		lua.set_current("tab", lua.make_table([](lua_t lua) {
			lua.set_current("key2", lua.make_table([](lua_t) {}));
			lua.set_current(1, 6.0);
			lua.set_current(2, &error_handler);
			lua.set_current(3, lua.call<iris_lua_t::ref_t>(lua.load("local b = 1 return function () return tostring(b + 1) end")));
		}));
	});

	auto recursive_tab = complex.get<lua_t::ref_t>(lua, "tab");
	recursive_tab->set(lua, "recursive", recursive_tab);
	lua.deref(std::move(recursive_tab.value()));

	auto encode_complex = lua.encode<std::string>(std::move(complex), [](iris_lua_t lua, iris_queue_list_t<uint8_t>& bytes, int index, int type) {
		if (type == LUA_TFUNCTION) {
			if (lua_iscfunction(lua.get_state(), index)) {
				void* p = (void*)&error_handler;
				bytes.push(0);
				bytes.push(reinterpret_cast<const char*>(&p), reinterpret_cast<const char*>(&p) + sizeof(p));
				return true;
			} else {
				bytes.push(1);
				return false;
			}
		}

		return false;
	}, iris_queue_list_t<uint8_t>());

	auto decode_complex = std::move(lua.decode<iris_lua_t::ref_t>(std::move(encode_complex), [](iris_lua_t lua, const char*& from, const char* to, int type) {
		if (type == LUA_TFUNCTION) {
			if (*from++ == 0) {
				void* p = nullptr;
				memcpy(&p, from, sizeof(p));
				lua_pushcclosure(lua.get_state(), (lua_CFunction)p, 0);
				from += sizeof(p);
				return true;
			} else {
				return false;
			}
		} else {
			return false;
		}
	}).value());

	IRIS_ASSERT(decode_complex.get<std::string_view>(lua, "key") == "value");
	IRIS_ASSERT(decode_complex.get<bool>(lua, 1));
	IRIS_ASSERT(decode_complex.get<int>(lua, 2) == 2);
	auto decode_tab = decode_complex.get<lua_t::ref_t>(lua, "tab");
	IRIS_ASSERT(decode_tab->get<double>(lua, 1) == 6.0);
	auto decode_tab2 = decode_tab->get<lua_t::ref_t>(lua, "key2");
	IRIS_ASSERT(decode_tab2->get_type(lua) == LUA_TTABLE);

	lua.native_push_variable(decode_tab->get<lua_t::ref_t>(lua, 2));
	void* pfunc = (void*)lua_tocfunction(lua.get_state(), -1);
	IRIS_ASSERT(pfunc == error_handler);
	lua.native_pop_variable(1);

	IRIS_ASSERT(lua.call<std::string>(decode_tab->get<lua_t::ref_t>(lua, 3)).value() == "2");
	recursive_tab = decode_tab->get<lua_t::ref_t>(lua, "recursive");
	IRIS_ASSERT(recursive_tab->get_type(lua) == LUA_TTABLE);
	lua.native_push_variable(recursive_tab->get<lua_t::ref_t>(lua, "recursive"));
	lua.native_push_variable(std::move(recursive_tab.value()));
	IRIS_ASSERT(lua_rawequal(lua.get_state(), -1, -2));
	lua.native_pop_variable(2);

	lua.deref(std::move(decode_tab2.value()));
	lua.deref(std::move(decode_tab.value()));
	lua.deref(std::move(decode_complex));

	auto print2 = lua.get_global<lua_t::ref_t>("print2");
	lua.native_call(std::move(print2), 1);

	lua.native_push_variable(1234);
	int v = lua.native_get_variable<int>(-1);
	lua.native_pop_variable(1);
	IRIS_ASSERT(v == 1234);
}

static void test_bindings(lua_t lua) {
	lua_t::refptr_t<example_t> example = lua.make_object<example_t>(lua.get_global<lua_t::ref_t>("example_t"), example_t());
	example->value = 5;
	lua.deref(std::move(example));
	auto temp_tab = lua.make_table([](lua_t lua) {
		lua.set_current("first", 1);
	});

	lua.with(temp_tab, [](lua_t lua) {
		lua.set_current("second", 2);
	});

	lua.set_global("test_tab", std::move(temp_tab));

	auto callResult3 = lua.call<void>(lua.load("\
		print(_VERSION)\n\
		example_t.native_call() \n\
		example_t.native_call_noexcept() \n\
		local a = example_t.new(2)\n\
		local b = example_t.new(3)\n\
		b:overload_func() \n\
		b:overload_func(1) \n\
		b:base_func() \n\
		print('base value ' .. tostring(b.base_value)) \n\
		b:join_value_required(a)\n\
		--b:join_value_required()\n\
		b:join_value_required_refptr(a)\n\
		--b:join_value_required_refptr()\n\
		print(a.const_value)\n\
		local count, base = a.var(1, 2, 3)\n\
		print('lua base ' .. base .. ', count ' .. count)\n\
		b.value = 1\n\
		assert(b.value == 1)\n\
		assert(b:get_value_raw() == 1)\n\
		assert(b:get_value_raw_lambda() == 1)\n\
		local base2 = b.value\n\
		print(base2)\n\
		b:accum_value(1000)\n\
		local sum = 0\n\
		for i = 1, 10 do\n\
			sum = sum + a:accum_value(i)\n\
		end\n\
		b:join_value(a)\n\
		b:join_value_refptr(a)\n\
		b:join_value_by_value(a)\n\
		b:join_value_by_const_value(a)\n\
		b:join_value_by_const_ref(a)\n\
		b:join_value_by_rvalue_ref(a)\n\
		local p = a:call(function (v) return v + 1 end, 1)\n\
		print(p) \n\
		print(a:get_value())\n\
		print(b:get_value())\n\
		print(sum)\n\
		print(a.forward_tuple({1, 'tuple'})[1])\n\
		print(a.forward_tuple_raw({1, 'tuple'})[1])\n\
		print(a.forward_pair({2, 'pair'})[1])\n\
		print(a:forward_map({ type = 'map'})['abc'])\n\
		print(a:forward_vector({ '3', 4, '5' })[4])\n\
		local prime = a:prime()\n\
		print(prime.name)\n\
		for i = 1, #prime do\n\
			print(prime[i])\n\
		end\n\
		local t = a:get_vector3({11, 22, 33 })\n\
		for i = 1, #t do\n\
			print(t[i])\n\
		end\n").value());
	if (!callResult3) {
		fprintf(stderr, "Lua code error: %s\n", callResult3.message.c_str());
		IRIS_ASSERT(false);
	}

	auto tab = lua.make_table([](lua_t&& lua) {
		lua.set_current("key", "value");
		lua.set_current(1, "number");
		lua.set_current(2, 2);
	});

	tab.set(lua, "set", "newvalue");
	IRIS_ASSERT(*tab.get<int>(lua, 2) == 2);
	auto r = tab.get<lua_t::ref_t>(lua, 2);
	IRIS_ASSERT(r);
	IRIS_ASSERT((*r).as<int>(lua) == 2);
	IRIS_ASSERT(tab.size(lua) == 2);
	lua.deref(std::move(*r));

	tab.for_each<std::string_view, std::string_view>(lua, [](auto key, auto value) {
		printf("key = %s, value = %s\n", key.data(), value.data());
		return false;
	});
	lua.deref(std::move(tab));

	lua.set_global<&lua_error>("other_error");
	lua.set_registry<&lua_error>("other_error");
	// lua.native_push_variable<&lua_error>();
	// lua.load("a").set<&lua_error>(lua, "test");
	IRIS_ASSERT(!lua.load("err"));
	printf("Error message: %s\n", lua.load("err").message.c_str());
}

static void test_coroutines(lua_t lua) {
#if USE_LUA_COROUTINE
	worker_t worker(1);
	warp_t warp(worker);
	warp_t warp2(worker);

	workerptr = &worker;
	warpptr = &warp;
	warpptr2 = &warp2;
	worker.start();
	warp_t::preempt_guard_t preempt_guard(warp, 0);

#if LUA_VERSION_NUM <= 501
	// lua 5.1 do not accept yield from pcall
	auto callResult4 = lua.call<void>(lua.load("\n\
		local a = example_t.new(4)\n\
		local coro = coroutine.create(function() \n\
			print('coro get ' .. a.coro_get_int('hello')) \n\
			print('coro get ' .. a.coro_get_int('hello', 1)) \n\
			local x1, x2 = a:mem_coro_get_int('world') \n\
			print('memcoro get ' .. x1 .. ', ' .. x2) \n\
			print('memcoro get second ' .. a:mem_coro_get_int('world')) \n\
			print('memcoro get second ' .. a:mem_coro_get_int_raw('world')) \n\
			a.coro_get_none()\n\
			print('coro finished')\n\
		end)\n\
		coroutine.resume(coro)\n").value());
#else
	auto callResult4 = lua.call<void>(lua.load("\n\
		local a = example_t.new(5)\n\
		local coro = coroutine.create(function() \n\
			local status, message = pcall(function() \n\
			print('coro get ' .. a.coro_get_int('hello')) \n\
			print('coro get ' .. a.coro_get_int('hello', 1)) \n\
			local x1, x2 = a:mem_coro_get_int('world') \n\
			print('memcoro get ' .. x1 .. ', ' .. x2) \n\
			print('memcoro get second ' .. a:mem_coro_get_int('world')) \n\
			print('memcoro get second ' .. a:mem_coro_get_int_raw('world')) \n\
			end) \n\
			print(status, message) \n\
			a.coro_get_none()\n\
			print('coro finished')\n\
		end)\n\
		coroutine.resume(coro)\n").value());
#endif

	if (!callResult4) {
		fprintf(stderr, "Lua code error: %s\n", callResult4.message.c_str());
		IRIS_ASSERT(false);
	}

	warp.yield();
	worker.join();

	auto waiter = []() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		return false;
	};

	while (warp_t::poll({ std::ref(warp), std::ref(warp2) })) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	preempt_guard.cleanup();
#endif
}

int main(void) {
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	lua_t lua(L);

	test_overload_cast(lua);
	test_registry_types(lua);
	test_functions(lua);
	test_cross_vm(lua);
	test_encode(lua);
	test_bindings(lua);
	test_coroutines(lua);
	test_proxy_views(lua);
	test_queue_faces(lua);
	test_shared_elements(lua);

	lua_close(L);
	IRIS_ASSERT(ref_count.load(std::memory_order_acquire) == 0);
	return 0;
}

// ============================================================================
// proxy views: in-place member container binding (was iris_lua_proxy_demo.cpp)
// ============================================================================
namespace proxy_test {

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

}

static void test_proxy_views(lua_t lua) {
	using namespace proxy_test;

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
}

// ============================================================================
// queue/heap container faces (was iris_lua_queue_demo.cpp)
// ============================================================================
namespace queue_test {

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

local ok7 = pcall(function() return lm.pop(a.labels) end)
assert(not ok7 or lm.pop == nil)

-- clone of a const view is an owned mutable copy of the payload
local lc = lm.clone(a.labels)
assert(#lc == 2 and getmetatable(lc).front(lc) == "alpha")

print("queue demo passed")
)lua";

}

static void test_queue_faces(lua_t lua) {
	using namespace queue_test;

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
}

// ============================================================================
// shared elements (was iris_lua_shared_element_demo.cpp)
// ============================================================================
namespace shared_element_test {

	struct node_t : lua_t::shared_local_object_t<node_t> {
		static void lua_registar(lua_t lua, std::nullptr_t) {
			lua.set_current<&node_t::value>("value");
			lua.set_current<&node_t::bump>("bump");
		}

		void bump() { ++value; }

		int value = 0;
	};

	struct graph_t {
		std::vector<lua_t::shared_ref_t<node_t>> nodes;

		static void lua_registar(lua_t lua, std::nullptr_t) {
			lua.set_current_proxy<&graph_t::nodes>("nodes");
		}
	};

}

// verify vector<shared_ref_t<T>> elements nest correctly through a proxy:
// element reads yield shared-object views (refcount-anchored), writes go
// through to the heap objects, GC releases cleanly
static void test_shared_elements(lua_t lua) {
	using namespace shared_element_test;

	auto node_type = lua.make_registry_type<node_t>();
	auto graph_type = lua.make_registry_type<graph_t>();
	lua.set_global("g", lua.make_object<graph_t>(graph_type, graph_t()));
	{
		auto g = lua.get_global<graph_t*>("g").value();
		g->nodes.push_back(lua_t::shared_ref_t<node_t>::make());
		g->nodes[0]->value = 10;
	}

	auto result = lua.call<void>(lua.load(R"lua(
local m = getmetatable(g.nodes)
local n = g.nodes[1] -- shared view of the heap object
assert(n.value == 10)
n.value = 20 -- writes through, no copy
assert(g.nodes[1].value == 20)
n:bump()
assert(g.nodes[1].value == 21)

-- the view survives element copies and vector growth
m.push_back(g.nodes, n)
assert(#g.nodes == 2 and g.nodes[2].value == 21)
m.push_back(g.nodes, g.nodes[1])
assert(#g.nodes == 3 and g.nodes[3].value == 21)

-- whole-object assignment through the proxy
g.nodes[2] = g.nodes[1]
print("shared element demo passed")
)lua"));
	if (!result) {
		fprintf(stderr, "Lua code error: %s\n", result.message.c_str());
		IRIS_ASSERT(false);
	}

	{
		auto g = lua.get_global<graph_t*>("g").value();
		IRIS_ASSERT(g->nodes.size() == 3);
		IRIS_ASSERT(g->nodes[0]->value == 21 && g->nodes[2]->value == 21);
		IRIS_ASSERT(g->nodes[0].get() != nullptr);
	}

	// views collected: refcounts must return to the vector-held references
	lua.call<void>(lua.load("collectgarbage('collect') collectgarbage('collect')"));

	lua.deref(std::move(graph_type));
	lua.deref(std::move(node_type));
}
