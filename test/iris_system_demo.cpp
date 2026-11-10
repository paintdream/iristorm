#include "../src/iris_system.h"
#include "../src/iris_common.inl"
#include <map>
using namespace iris;

using entity_t = uint32_t;
using entity_allocator = iris_entity_allocator_t<entity_t>;

struct iris_component_matrix_t {
	float values[4][4];
};

template <typename element_t>
using block_allocator_t = iris_block_allocator_t<element_t>;

struct int_interface : iris_pool_t<int_interface, iris_queue_quick_list_t<int*>> {
	template <typename element_t>
	element_t acquire_element();

	template <typename element_t>
	void release_element(element_t&& element);
};

template <>
inline int* int_interface::acquire_element<int*>() {
	return new int();
}

template <>
inline void int_interface::release_element<int*>(int*&& object) {
	delete object;
}

int main(void) {
	iris::iris_queue_quick_list_t<int> ql;
	std::vector<int> vv;
	for (int i = 0; i < 0x716; i++) {
		vv.push_back(i);
	}

	ql.push(vv.begin(), vv.end());
	auto s = ql.copy(vv.begin(), vv.end(), 100);
	IRIS_ASSERT(vv.end() - s == 100);

	/*
	for (auto v : ql) {
		printf("%d\n", v);
	}*/

	auto t = ql.copy(vv.begin() + 100, vv.end(), 300);
	IRIS_ASSERT(vv.end() - t == 200);

	iris::iris_quota_t<int, 2> quota({ 5, 3 });
	bool u1 = quota.acquire({ 1,2 });
	IRIS_ASSERT(u1);
	bool u2 = quota.acquire({ 2,3 });
	IRIS_ASSERT(!u2);
	{
		auto v = quota.guard({ 4, 4 });
		IRIS_ASSERT(!v);
		auto w = quota.guard({ 1, 1 });
		IRIS_ASSERT(w);
	}

	quota.release({ 1,2 });

	using queue = iris_queue_quick_list_t<int, iris_default_block_allocator_t>;
	iris_default_block_allocator_t<int> alloc;
	queue q(alloc);
	q.push(1);
	q.pop();

	int_interface pool;
	std::vector<int*> allocated;
	for (size_t i = 0; i < 0x1234; i++) {
		allocated.emplace_back(pool.acquire());
	}

	for (size_t i = 0; i < 0x1234; i++) {
		pool.release(std::move(allocated[i]));
	}

	pool.clear();

	iris_system_t<entity_t, block_allocator_t, std::allocator, iris_component_matrix_t, uint8_t> matrix_system(alloc, std::allocator<entity_t>());
	std::vector<entity_t> entities;
	iris_entity_allocator_t<entity_t> allocator;

	for (size_t k = 0; k < 128; k++) {
		entity_t entity = allocator.allocate();
		matrix_system.insert(entity, iris_component_matrix_t(), (uint8_t)k);
		entities.emplace_back(entity);
	}

	for (size_t m = 0; m < 32; m++) {
		entity_t entity = entities[m] * 4;
		matrix_system.remove(entity);
		allocator.free(entity);
	}

	for (size_t m = 0; m < 64; m++) {
		matrix_system.insert(allocator.allocate(), iris_component_matrix_t(), (uint8_t)m);
	}

	float sum = 0;
	for (auto&& item : matrix_system.component<iris_component_matrix_t>()) {
		sum += item.values[0][0];
	}

	allocator.reset();
	printf("Sum should be zero: %f\n", sum);
	
	matrix_system.iterate<iris_component_matrix_t, uint8_t>([](iris_component_matrix_t& matrix, uint8_t& v) {
		// initialize with identity matrix
		matrix.values[0][0] = 1; matrix.values[0][1] = 0; matrix.values[0][2] = 0; matrix.values[0][3] = 0;
		matrix.values[1][0] = 0; matrix.values[1][1] = 1; matrix.values[1][2] = 0; matrix.values[1][3] = 0;
		matrix.values[2][0] = 0; matrix.values[2][1] = 0; matrix.values[2][2] = 1; matrix.values[2][3] = 0;
		matrix.values[3][0] = 0; matrix.values[3][1] = 0; matrix.values[3][2] = 0; matrix.values[3][3] = 1;
	});

	matrix_system.iterate_batch<iris_component_matrix_t>(4, [](size_t count, iris_queue_quick_list_t<iris_component_matrix_t, block_allocator_t>::iterator it) {
		while (count-- != 0) {
			it->values[3][3] = 2;
			it++;
		}
	});

	matrix_system.iterate<entity_t, iris_component_matrix_t>([&](entity_t entity, iris_component_matrix_t& matrix) {
		// initialize with identity matrix
		IRIS_ASSERT(&matrix == &matrix_system.locate<iris_component_matrix_t>(matrix_system.locate<uint8_t>(matrix)));
		IRIS_ASSERT(matrix.values[0][0] == 1);
	});

	matrix_system.filter<iris_component_matrix_t>(0, [&](iris_component_matrix_t& matrix) {
		IRIS_ASSERT(&matrix == &matrix_system.locate<iris_component_matrix_t>(matrix_system.locate<uint8_t>(matrix)));
		matrix.values[1][1] = 2;
	});

	int arr[2] = { 4, 6 };
	matrix_system.filter<iris_component_matrix_t>(&arr[0], &arr[0] + 2, [](iris_component_matrix_t& matrix) {
		matrix.values[1][1] = 2;
	});

	iris_system_t<entity_t, block_allocator_t, std::allocator, float, uint8_t> other_system;
	for (size_t k = 0; k < 5; k++) {
		other_system.insert(iris::iris_verify_cast<entity_t>(k), 0.1f, (uint8_t)k);
	}

	iris_systems_t<entity_t, block_allocator_t, std::allocator> systems(alloc, std::allocator<entity_t>());
	systems.attach(other_system);
	systems.attach(matrix_system);
	int counter = 0;
	systems.iterate<uint8_t>([&counter](uint8_t& i) {
		i = counter++;
	});

	systems.filter_joined<float, iris_component_matrix_t>(1u, [](float& f, iris_component_matrix_t& mat) {
		printf("jointed filter: %f\n", f);
	});

	systems.filter_joined<float>(1u, [](float& f) {
		printf("jointed filter2: %f\n", f);
	});

	systems.filter_joined<float, iris_component_matrix_t>(8u, [](float& f, iris_component_matrix_t& mat) {
		IRIS_ASSERT(false);
	});

	systems.filter<uint8_t>(1, [](uint8_t& v) {
		printf("Entity 1 = %u\n", v);
	});

	entity_t qe[2] = { 1, 2 };
	systems.filter<uint8_t>(qe, qe + 2, [](uint8_t& v) {
		printf("Entity arr = %u\n", v);
	});

	systems.iterate_batch<iris_component_matrix_t>(4, [](size_t count, iris_queue_quick_list_t<iris_component_matrix_t, block_allocator_t>::iterator it) {
		while (count-- != 0) {
			IRIS_ASSERT(it->values[3][3] == 2);
			it++;
		}
	});

	systems.detach(other_system);
	systems.iterate<entity_t, uint8_t>([&counter](entity_t e, uint8_t& i) {
		counter--;
	});
	IRIS_ASSERT(counter == 5);
	systems.attach(other_system);

	systems.iterate<float>([&counter](float& i) {
		counter--;
	});

	IRIS_ASSERT(counter == 0);
	
	iris_system_t<entity_t, block_allocator_t, std::allocator, uint8_t> re_system;
	systems.attach(re_system);
	re_system.insert(0, 1u);
	systems.remove(0);
	systems.compress();
	re_system.iterate<entity_t>([](entity_t entity) {
		IRIS_ASSERT(false); // already removed
	});

	re_system.clear();
	systems.clear();
	systems.detach(re_system);

	matrix_system.remove(&arr[0], &arr[0] + 2);
	matrix_system.filter<iris_component_matrix_t>(&arr[0], &arr[0] + 2, [](iris_component_matrix_t& matrix) {
		assert(false);
	});

	int union_set[10];
	iris_union_set_init(union_set, (int)0u, (int)(sizeof(union_set) / sizeof(union_set[0])));
	iris_union_set_join(union_set, 3u, 6u);
	iris_union_set_join(union_set, 6u, 9u);
	iris_union_set_join(union_set, 2u, 4u);
	iris_union_set_join(union_set, 8u, 4u);
	iris_union_set_join(union_set, 7u, 5u);
	iris_union_set_join(union_set, 1u, 5u);

	IRIS_ASSERT(iris_union_set_find(union_set, 1u) == iris_union_set_find(union_set, 7u));
	IRIS_ASSERT(iris_union_set_find(union_set, 4u) != iris_union_set_find(union_set, 6u));
	IRIS_ASSERT(iris_union_set_find(union_set, 2u) == iris_union_set_find(union_set, 8u));
	IRIS_ASSERT(iris_union_set_find(union_set, 5u) != iris_union_set_find(union_set, 9u));
	IRIS_ASSERT(iris_union_set_find(union_set, 0u) != iris_union_set_find(union_set, 3u));

	iris_cache_t<uint8_t> cache;
	iris_cache_allocator_t<double, uint8_t> cache_allocator(&cache);

	// todo: more tests
	std::vector<double, iris_cache_allocator_t<double, uint8_t>> vec(cache_allocator);
	vec.push_back(1234.0f);
	vec.resize(777);

	std::vector<double> dbl_vec;
	iris::iris_binary_insert(dbl_vec, 1234.0f);
	auto it = iris::iris_binary_find(dbl_vec.begin(), dbl_vec.end(), 1234.0f);
	IRIS_ASSERT(it != dbl_vec.end());
	iris::iris_binary_erase(dbl_vec, 1234.0f);

	std::vector<iris::iris_key_value_t<int, const char*>> str_vec;
	iris::iris_binary_insert(str_vec, iris::iris_make_key_value(1234, "asdf"));
	iris::iris_binary_insert(str_vec, iris::iris_make_key_value(2345, "defa"));
	auto it2 = iris::iris_binary_find(str_vec.begin(), str_vec.end(), 1234);
	IRIS_ASSERT(it2 != str_vec.end());
	IRIS_ASSERT(iris::iris_binary_find(str_vec.begin(), str_vec.end(), 1236) == str_vec.end());
	iris::iris_binary_erase(str_vec, 1234);
	iris::iris_binary_erase(str_vec, iris::iris_make_key_value(1234, ""));

	// Tombstone / entity-id-reuse regression (mirrors PaintsNowNext 913a27f
	// and this repo's eda478b29): remove() leaves a tombstone entry
	// (second == ~0) in entity_components for the deleted entity. When the
	// allocator reuses that id and insert() is called again, the entry must
	// be REUSED (not duplicated), and valid()/get()/filter() must resolve to
	// the new component - never to the stale tombstone (getting index ~0 was
	// an out-of-range slot / Debug assert in the renderer's per-entity
	// culling after streaming evictions).
	{
		iris_system_t<entity_t, block_allocator_t, std::allocator, uint8_t> reuse_system;
		iris_entity_allocator_t<entity_t> reuse_alloc;

		entity_t kept = reuse_alloc.allocate();   // 0
		entity_t doomed = reuse_alloc.allocate(); // 1
		reuse_system.insert(kept, 11u);
		reuse_system.insert(doomed, 22u);
		IRIS_ASSERT(reuse_system.valid(kept) && reuse_system.valid(doomed));
		IRIS_ASSERT(reuse_system.get<uint8_t>(doomed) == 22u);
		IRIS_ASSERT(reuse_system.size() == 2);

		// Delete the doomed entity: its entry becomes a tombstone, its
		// component is removed and its id is returned to the allocator.
		reuse_system.remove(doomed);
		reuse_alloc.free(doomed);
		IRIS_ASSERT(!reuse_system.valid(doomed)); // tombstone is NOT alive
		IRIS_ASSERT(reuse_system.filter<uint8_t>(doomed, [](uint8_t&) { IRIS_ASSERT(false); }) == false); // filter skips tombstones
		IRIS_ASSERT(reuse_system.size() == 1);

		// The allocator hands the SAME id back (reuse); insert must reuse the
		// tombstone entry - not add a second key for the same entity.
		entity_t reused = reuse_alloc.allocate();
		IRIS_ASSERT(reused == doomed);
		reuse_system.insert(reused, 33u);
		IRIS_ASSERT(reuse_system.valid(reused));
		IRIS_ASSERT(reuse_system.get<uint8_t>(reused) == 33u); // must NOT resolve to the stale tombstone (~0 slot)
		{
			bool hit = false;
			IRIS_ASSERT(reuse_system.filter<uint8_t>(reused, [&](uint8_t& v) { hit = (v == 33u); }));
			IRIS_ASSERT(hit);
		}
		IRIS_ASSERT(reuse_system.size() == 2);

		// Exactly ONE entry per entity in entity_components (live or
		// tombstone) - a second entry would let binary_find hit the stale
		// tombstone on the next lookup.
		{
			size_t kept_keys = 0, reused_keys = 0, kept_alive = 0, reused_alive = 0;
			for (auto& kv : reuse_system.get_entity_components()) {
				const bool alive = kv.second != static_cast<size_t>(~(entity_t)0);
				if (kv.first == kept) { kept_keys++; if (alive) kept_alive++; }
				if (kv.first == reused) { reused_keys++; if (alive) reused_alive++; }
			}
			IRIS_ASSERT(kept_keys == 1 && reused_keys == 1);
			IRIS_ASSERT(kept_alive == 1 && reused_alive == 1);
		}

		// Delete once more and re-verify the tombstone is not alive.
		reuse_system.remove(reused);
		reuse_alloc.free(reused);
		IRIS_ASSERT(!reuse_system.valid(reused));
		IRIS_ASSERT(reuse_system.size() == 1);
		reuse_alloc.reset();
	}

	// Standalone queue sanity: swap-and-pop move + pop + re-push + indexed
	// get must resolve to the re-pushed value (regression guard for the
	// tombstone-reuse bookkeeping above).
	{
		iris_queue_quick_list_t<uint8_t, iris_default_block_allocator_t> tq;
		tq.push(11u);
		tq.push(22u);
		tq.get(1) = std::move(tq.top()); // swap-and-pop move step
		tq.pop();
		tq.push(33u);
		IRIS_ASSERT(tq.get(1) == 11u);
		IRIS_ASSERT(tq.get(2) == 33u);
		IRIS_ASSERT(tq.begin_index() == 1 && tq.end_index() == 3);
		tq.clear();
	}

	// Multi-block churn / indexed access stress: a WIDE component makes the
	// queue element_count small, so inserts span multiple blocks; heavy
	// remove (pop fronts whole blocks) + entity-id reuse must leave every
	// live entry pointing at a slot inside the active pool (regression for
	// the TransformComponent out-of-bounds under streaming churn - a remove()
	// swap-and-pop range bug left the top entity's entry pointing at a popped
	// slot).
	{
		struct WideComp { double m[64]; }; // 512 B -> element_count = block_size/512 (multi-block)
		iris_system_t<entity_t, block_allocator_t, std::allocator, WideComp> wide;
		iris_entity_allocator_t<entity_t> walloc;

		std::vector<entity_t> alive;
		// wave 1: insert 800
		for (size_t i = 0; i < 800; i++) {
			entity_t e = walloc.allocate();
			wide.insert(e, WideComp());
			alive.emplace_back(e);
		}
		// delete ~every 3rd
		for (size_t i = 0; i < alive.size(); i += 3) {
			wide.remove(alive[i]);
			walloc.free(alive[i]);
			alive[i] = ~0u;
		}
		// verify all surviving entities resolve (get()/filter() must not throw out-of-bounds)
		for (entity_t e : alive) {
			if (e == ~0u) continue;
			IRIS_ASSERT(wide.valid(e));
			WideComp* w = nullptr;
			IRIS_ASSERT(wide.filter<WideComp>(e, [&](WideComp& c) { w = &c; }));
			IRIS_ASSERT(w != nullptr);
			(void)w;
		}
		// re-insert on the freed ids (id reuse), then full re-verify
		for (size_t i = 0; i < alive.size(); i++) {
			if (alive[i] == ~0u) {
				alive[i] = walloc.allocate();
				wide.insert(alive[i], WideComp());
			}
		}
		// invariant audit: single live slot per entity, each inside the pool
		{
			const size_t comp_begin = wide.component<WideComp>().begin_index();
			const size_t comp_end = wide.component<WideComp>().end_index();
			std::map<size_t, size_t> slot_use;
			for (auto& kv : wide.get_entity_components()) {
				if (kv.second == static_cast<size_t>(~(entity_t)0)) continue;
				const size_t s = static_cast<size_t>(kv.second);
				IRIS_ASSERT(s >= comp_begin && s < comp_end); // live slot in range
				IRIS_ASSERT(slot_use[s]++ == 0); // no duplicate slots
			}
		}
		for (entity_t e : alive) {
			IRIS_ASSERT(wide.valid(e));
			WideComp* w = nullptr;
			IRIS_ASSERT(wide.filter<WideComp>(e, [&](WideComp& c) { w = &c; }));
			IRIS_ASSERT(w != nullptr);
		}
		// remove everything; after each removal no live entry may point at a
		// popped slot (the exact failure mode of the old swap-and-pop range bug).
		for (entity_t e : alive) {
			wide.remove(e);
			walloc.free(e);
			const size_t cb = wide.component<WideComp>().begin_index(), ce = wide.component<WideComp>().end_index();
			for (auto& kv : wide.get_entity_components()) {
				if (kv.second == static_cast<size_t>(~(entity_t)0)) continue;
				const size_t s = static_cast<size_t>(kv.second);
				IRIS_ASSERT(s >= cb && s < ce);
			}
		}
		walloc.reset();
		wide.clear();
	}

	return 0;
}

