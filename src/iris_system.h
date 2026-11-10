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

namespace iris {
	namespace {
		// find type index for given type
		// https://stackoverflow.com/questions/26169198/how-to-get-the-index-of-a-type-in-a-variadic-type-pack
		template <typename target_t, typename... types_t>
		struct fetch_index_impl : std::integral_constant<size_t, 0> {};

		template <typename target_t, typename... types_t>
		struct fetch_index_impl<target_t, target_t, types_t...> : std::integral_constant<size_t, 0> {};

		template <typename target_t, typename next_t, typename... types_t>
		struct fetch_index_impl<target_t, next_t, types_t...> : std::integral_constant<size_t, 1 + fetch_index_impl<target_t, types_t...>::value> {};

		template <size_t n, typename... args_t>
		using locate_type = typename std::tuple_element<n, std::tuple<args_t...>>::type;

		template <typename type_t>
		constexpr bool check_duplicated_components_one() { return true; }

		template <typename type_t, typename first_t, typename... check_types_t>
		constexpr bool check_duplicated_components_one() {
			return !std::is_same<type_t, first_t>::value && check_duplicated_components_one<type_t, check_types_t...>();
		}

		template <typename first_t>
		constexpr bool check_duplicated_components() { return true; }

		template <typename first_t, typename second_t, typename... check_types_t>
		constexpr bool check_duplicated_components() {
			return check_duplicated_components_one<first_t, second_t, check_types_t...>() && check_duplicated_components<second_t, check_types_t...>();
		}

		template <typename... type_t>
		struct placeholder {};

		template <typename... iterators_t>
		void dummy(iterators_t&&...) {}

		template <typename iterators_t, size_t... i>
		void step_iterators(iterators_t& iterators, size_t count) {
			dummy(std::get<i>(iterators) += static_cast<ptrdiff_t>(count)...);
		}

		template <typename iterators_t, typename operation_t, size_t... i>
		static void do_operation(iterators_t& iterators, operation_t& op, iris_sequence<i...>) {
			op(*std::get<i>(iterators)++...);
		}

		template <typename iterators_t, typename operation_t, size_t... i>
		static void do_operation_batch(iterators_t& iterators, operation_t& op, iris_sequence<i...>, size_t count) {
			op(count, std::get<i>(iterators)...);
		}
	}

	// components_t is not allowed to contain repeated types
	template <typename entity_t, template <typename...> class allocator_t, template <typename...> class vector_allocator_t = std::allocator, typename... components_t>
	struct iris_system_t : protected enable_read_write_fence_t<> {
		template <typename target_t>
		struct fetch_index : fetch_index_impl<target_t, components_t...> {};
		static constexpr size_t block_size = allocator_t<entity_t>::block_size;
		using index_t = entity_t; // just for alignment
		using components_tuple_t = std::tuple<entity_t, components_t...>;

		iris_system_t() {
			// check if there are duplicated types
			static_assert(check_duplicated_components<entity_t, components_t...>(), "duplicated entity/component detected!");
		}

		iris_system_t(const allocator_t<entity_t>& allocator, const vector_allocator_t<entity_t>& vector_allocator)
			: components(allocator_t<components_t>(allocator)...), entity_components(vector_allocator), entities(allocator) {
			// check if there are duplicated types
			static_assert(check_duplicated_components<entity_t, components_t...>(), "duplicated entity/component detected!");
		}

		// entity-based component insertion
		bool valid(entity_t entity) const noexcept {
			auto guard = read_fence();
			// A tombstone entry (second == ~0, left in place by remove()) does
			// NOT mean the entity exists: its component was deleted and the
			// slot may be recycled. Filtering tombstones keeps Valid()/get()
			// consistent with filter() (which already skips them) - otherwise
			// a deleted entity whose numeric id was REUSED by a later
			// Create() resolves to the stale tombstone and get() reads index
			// ~0, an out-of-range slot (the RenderCamera::Tick "std::vector
			// []" Debug assert after streaming evictions).
			auto it = iris_binary_find(entity_components.begin(), entity_components.end(), entity);
			return it != entity_components.end() && it->second != ~(index_t)0;
		}

		// returns true if the existing entity was replaced, or false if new entity was created
		template <typename... elements_t>
		bool insert(entity_t entity, elements_t&&... t) {
			auto guard = write_fence();

			auto iterator = iris_binary_find(entity_components.begin(), entity_components.end(), entity);
			if (iterator != entity_components.end() && iterator->second != ~(index_t)0) {
				replace_components<sizeof...(components_t)>(iterator->second, std::forward<elements_t>(t)...);
				return true;
			}

			// Tombstone entry (a previous Delete left second == ~0 in place,
			// keyed by this entity). Whatever the branch below, the entry must
			// be (re)used, never duplicated: iris_binary_find on an entity
			// with two entries can resolve to the stale tombstone, so
			// Valid()/get() would read index ~0 and fetch an out-of-range
			// slot (the RenderCamera::Tick Debug assert after an entity-id
			// reuse). Capture whether the entry exists BEFORE the reserve
			// below can reallocate the vector and invalidate `iterator`.
			const bool reuse_tombstone = iterator != entity_components.end();

			if (entity_components.capacity() <= entity_components.size() + 1) {
				entity_components.reserve(entity_components.size() * 3 / 2);
			}

			emplace_components<sizeof...(components_t)>(std::forward<elements_t>(t)...);

			// The new component lives at the just-pushed slot
			// (entities.end_index() before entities.push()).
			const index_t new_index = iris_verify_cast<index_t>(entities.end_index());
			if (reuse_tombstone) {
				// Re-locate the entry after the reserve (the old iterator may
				// be dangling) and point it at the new slot.
				auto it = iris_binary_find(entity_components.begin(), entity_components.end(), entity);
				IRIS_ASSERT(it != entity_components.end());
				it->second = new_index;
			} else {
				iris_binary_insert(entity_components, iris_make_key_value(entity, new_index));
			}

			entities.push(entity);
			return false;
		}

		void compress() noexcept {
			auto guard = write_fence();

			size_t j = 0;
			for (size_t i = 0; i < entity_components.size(); i++) {
				auto& item = entity_components[i];
				if (item.second != ~(index_t)0) {
					entity_components[j++] = item;
				}
			}

			entity_components.resize(j);
		}

		size_t size() const noexcept {
			auto guard = read_fence();
			return entities.size();
		}

		template <typename to_component_t, typename from_component_t>
		to_component_t& locate(const from_component_t& from_component) noexcept {
			auto guard = read_fence();
			size_t index = component<from_component_t>().get_index(from_component);
			IRIS_ASSERT(index != ~(size_t)0);
			return component<to_component_t>().get(index);
		}

		// get specified component of given entity
		template <typename component_t>
		component_t& get(entity_t entity) noexcept {
			auto guard = read_fence();
			IRIS_ASSERT(valid(entity));
			return component<component_t>().get(iris_binary_find(entity_components.begin(), entity_components.end(), entity)->second);
		}

		template <typename... for_components_t, typename operation_t>
		bool filter(entity_t entity, operation_t&& op) {
			entity_t arr[1] = { entity };
			return filter<for_components_t...>(&arr[0], &arr[0] + 1, std::forward<operation_t>(op)) == (&arr[0] + 1);
		}

		template <typename... for_components_t, typename iterator_t, typename operation_t>
		iterator_t filter(iterator_t begin, iterator_t end, operation_t&& op) {
			auto guard = read_fence();

			auto ip = entity_components.begin();
			while (begin < end) {
				ip = iris_binary_find(ip, entity_components.end(), *begin);
				if (ip == entity_components.end() || ip->second == ~(index_t)0) {
					return begin;
				}

				op(component<for_components_t>().get(ip->second)...);
				++begin;
			}

			return begin;
		}

		template <typename component_t>
		const component_t& get(entity_t entity) const noexcept {
			auto guard = read_fence();

			IRIS_ASSERT(valid(entity));
			return component<component_t>().get(iris_binary_find(entity_components.begin(), entity_components.end(), entity)->second);
		}

		// entity-based component removal
		bool remove(entity_t entity) {
			entity_t arr[1] = { entity };
			return remove(&arr[0], &arr[0] + 1) == (&arr[0] + 1);
		}

		template <typename iterator_t>
		iterator_t remove(iterator_t begin, iterator_t end) {
			auto guard = write_fence();

			auto ip = entity_components.begin();
			while (begin < end && !entities.empty()) {
				entity_t top_entity = entities.top();
				entity_t entity = *begin;

				if (entity != top_entity) {
					// swap the top element (component_t, entity_t) with removed one
					ip = iris_binary_find(ip, entity_components.end(), entity);
					if (ip == entity_components.end() || ip->second == ~(index_t)0) {
						return begin; // not found
					}

					// move!!
					// Locate the TOP entity's entry relative to the removed
					// entity's entry (both keyed by id in entity_components):
					// if top < entity its entry is left of ip -> search
					// [begin, ip); if top > entity it is right of ip -> search
					// [ip, end). WARNING: the original expression chose the
					// end bound as `entity > top ? end : ip`, which collapses
					// the entity < top case to the EMPTY range [ip, ip) - the
					// top entry was never found, swap bookkeeping drifted from
					// the pools, and a later get()/filter() hit a popped slot
					// (out-of-bounds under streaming churn / id reuse).
					auto it = iris_binary_find(entity < top_entity ? ip : entity_components.begin(), entity > top_entity ? ip : entity_components.end(), top_entity);
					IRIS_ASSERT(it != entity_components.end() && it->second != ~(index_t)0);

					index_t index = ip->second;
					it->second = index; // reuse space!
					ip->second = ~(index_t)0;
					move_components(index, placeholder<components_t...>());

					entities.get(index) = top_entity; // update recorded entity list
				} else {
					ip = iris_binary_find(ip, entity_components.end(), entity);
					if (ip == entity_components.end() || ip->second == ~(index_t)0) {
						return begin; // not found
					}

					ip->second = ~(index_t)0;
				}

				pop_components(placeholder<components_t...>());
				entities.pop();
				++begin;
			}

			return begin;
		}

		void clear() {
			auto guard = write_fence();
			clear_components(placeholder<components_t...>());
			entities.clear();
			entity_components.clear();
		}

		// iterate components
		template <typename... for_components_t, typename operation_t>
		void iterate(operation_t&& op) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			auto guard = read_fence();
			auto iterators_begin = std::make_tuple(component<for_components_t>().begin()...);
			const auto iterators_end = std::make_tuple(component<for_components_t>().end()...);

			while (std::get<0>(iterators_begin) != std::get<0>(iterators_end)) {
				do_operation(iterators_begin, op, iris_make_sequence<std::tuple_size<decltype(iterators_begin)>::value>());
				step_iterators(iterators_begin, 1);
			}
		}

		template <typename... for_components_t, typename operation_t>
		void iterate_batch(size_t batch_count, operation_t&& op) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);

			auto guard = read_fence();
			auto iterators_begin = std::make_tuple(component<for_components_t>().begin()...);
			const auto iterators_end = std::make_tuple(component<for_components_t>().end()...);

			size_t counter = 0;
			auto iterators_current = iterators_begin;
			size_t total = iris_verify_cast<size_t>(std::get<0>(iterators_end) - std::get<0>(iterators_begin));
			for (size_t i = 0; i < total; i += batch_count) {
				size_t count = std::min(i + batch_count, total) - i;
				do_operation_batch(iterators_begin, op, iris_make_sequence<std::tuple_size<decltype(iterators_begin)>::value>(), count);
				step_iterators(iterators_begin, count);
			}
		}

		template <typename component_t>
		typename std::enable_if<!std::is_same<component_t, entity_t>::value, iris_queue_quick_list_t<component_t, allocator_t>&>::type component() noexcept {
			auto guard = read_fence();
			return std::get<fetch_index<component_t>::value>(components);
		}

		template <typename component_t>
		typename std::enable_if<std::is_same<component_t, entity_t>::value, iris_queue_quick_list_t<component_t, allocator_t>&>::type component() noexcept {
			auto guard = read_fence();
			return entities;
		}

		template <typename component_t>
		typename std::enable_if<!std::is_same<component_t, entity_t>::value, const iris_queue_quick_list_t<component_t, allocator_t>&>::type component() const noexcept {
			auto guard = read_fence();
			return std::get<fetch_index<component_t>::value>(components);
		}

		template <typename component_t>
		typename std::enable_if<std::is_same<component_t, entity_t>::value, const iris_queue_quick_list_t<component_t, allocator_t>&>::type component() const noexcept {
			auto guard = read_fence();
			return entities;
		}

		template <typename component_t>
		static constexpr bool has() noexcept {
			return fetch_index<component_t>::value < sizeof...(components_t);
		}

		using entity_components_t = std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>;
		entity_components_t& get_entity_components() noexcept {
			return entity_components;
		}

	protected:
		template <size_t i>
		void emplace_components() {}

		template <size_t i, typename first_t, typename... elements_t>
		void emplace_components(first_t&& first, elements_t&&... remaining) {
			std::get<sizeof...(components_t) - i>(components).push(std::forward<first_t>(first));
			emplace_components<i - 1>(std::forward<elements_t>(remaining)...);
		}

		template <size_t i>
		void replace_components(index_t id) {}

		template <size_t i, typename first_t, typename... elements_t>
		void replace_components(index_t id, first_t&& first, elements_t&&... remaining) {
			std::get<sizeof...(components_t) - i>(components).get(id) = std::forward<first_t>(first);
			replace_components<i - 1>(id, std::forward<elements_t>(remaining)...);
		}

		template <typename first_t, typename... elements_t>
		void move_components(index_t index, placeholder<first_t, elements_t...>) noexcept {
			auto& comp = std::get<sizeof...(elements_t)>(components);
			auto& top = comp.top();
			comp.get(index) = std::move(top);

			move_components(index, placeholder<elements_t...>());
		}

		void move_components(index_t& index, placeholder<>) noexcept {}

		template <typename first_t, typename... elements_t>
		void pop_components(placeholder<first_t, elements_t...>) noexcept {
			std::get<sizeof...(elements_t)>(components).pop();
			pop_components(placeholder<elements_t...>());
		}

		void pop_components(placeholder<>) noexcept {}

		template <typename first_t, typename... elements_t>
		void clear_components(placeholder<first_t, elements_t...>) noexcept {
			std::get<sizeof...(elements_t)>(components).clear();
			clear_components(placeholder<elements_t...>());
		}

		void clear_components(placeholder<>) noexcept {}

	protected:
		std::tuple<iris_queue_quick_list_t<components_t, allocator_t>...> components;
		entity_components_t entity_components;
		iris_queue_quick_list_t<entity_t, allocator_t> entities;
	};

	template <typename entity_t, template <typename...> class allocator_t = iris_default_block_allocator_t>
	struct iris_entity_allocator_t : protected enable_in_out_fence_t<> {
		entity_t allocate() noexcept(noexcept(std::declval<iris_entity_allocator_t>().free_entities.pop())) {
			auto guard = in_fence();
			if (!free_entities.empty()) {
				entity_t entity = free_entities.top();
				free_entities.pop();
				return entity;
			} else {
				return max_allocated_entity++;
			}
		}

		void free(entity_t entity) noexcept(noexcept(std::declval<iris_entity_allocator_t>().free_entities.push(entity_t()))) {
			auto guard = out_fence();
			free_entities.push(entity);
		}

		void reset() noexcept(noexcept(std::declval<iris_entity_allocator_t>().free_entities.reset(0))) {
			auto in_guard = in_fence();
			auto out_guard = out_fence();

			free_entities.reset(0);
			max_allocated_entity = 0;
		}

	protected:
		iris_queue_quick_list_t<entity_t, allocator_t> free_entities;
		entity_t max_allocated_entity = 0;
	};

	template <typename entity_t, template <typename...> class allocator_t, template <typename...> class vector_allocator_t = std::allocator>
	struct iris_systems_t : protected enable_read_write_fence_t<> {
		template <typename type_t>
		struct component_info_t {};
		using index_t = entity_t;

		template <typename type_t>
		static size_t get_type_hash() noexcept {
			return iris_static_instance_t<component_info_t<type_t>>::get_unique_hash();
		}

		iris_systems_t() {}
		iris_systems_t(const allocator_t<entity_t>& allocator, const vector_allocator_t<entity_t>& vector_allocator) : system_infos(vector_allocator) {}

		struct system_info_t {
			void* address = nullptr;
			bool (*remove)(void*, entity_t) = nullptr;
			void (*compress)(void*) = nullptr;
			void (*clear)(void*) = nullptr;
			std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>* entity_components = nullptr;
			std::vector<iris_key_value_t<size_t, void*>> components;
		};

		template <typename system_t>
		void attach(system_t& sys) {
			auto guard = write_fence();
			system_info_t info;
			info.address = &sys;
			info.remove = &remove_handler<system_t>;
			info.clear = &clear_handler<system_t>;
			info.compress = &compress_handler<system_t>;
			info.entity_components = &sys.get_entity_components();
			info.components = generate_info(info, sys, iris_make_sequence<std::tuple_size<typename system_t::components_tuple_t>::value>());
			std::sort(info.components.begin(), info.components.end());

			system_infos.emplace_back(std::move(info));
		}

		template <typename system_t>
		void detach(system_t& sys) {
			auto guard = write_fence();
			for (size_t i = 0; i < system_infos.size(); i++) {
				if (system_infos[i].address == &sys) {
					system_infos.erase(system_infos.begin() + i);
					break;
				}
			}
		}

		void compress() {
			auto guard = write_fence();
			for (size_t i = 0; i < system_infos.size(); i++) {
				auto& system_info = system_infos[i];
				system_info.compress(system_info.address);
			}
		}

		size_t remove(entity_t entity) {
			auto guard = write_fence();
			size_t count = 0;
			for (size_t i = 0; i < system_infos.size(); i++) {
				auto& system_info = system_infos[i];
				count += system_info.remove(system_info.address, entity) ? 1u : 0u;
			}

			return count;
		}

		void reset() {
			auto guard = write_fence();
			system_infos.clear();
		}

		void clear() {
			auto guard = write_fence();
			for (size_t i = 0; i < system_infos.size(); i++) {
				auto& system_info = system_infos[i];
				system_info.clear(system_info.address);
			}
		}

		template <typename... for_components_t, typename operation_t>
		void iterate(operation_t&& op) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);
			auto guard = read_fence();

			for (size_t i = 0; i < system_infos.size(); i++) {
				auto& system_info = system_infos[i];
				auto iterators_begin = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);
				auto iterators_end = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);

				if (match_iterators<decltype(iterators_begin), 0, for_components_t...>(iterators_begin, iterators_end, system_info)) {
					while (std::get<0>(iterators_begin) != std::get<0>(iterators_end)) {
						do_operation(iterators_begin, op, iris_make_sequence<std::tuple_size<decltype(iterators_begin)>::value>());
						step_iterators(iterators_begin, 1);
					}
				}
			}
		}

		template <typename... for_components_t, typename operation_t>
		void iterate_batch(size_t batch_count, operation_t&& op) {
			IRIS_PROFILE_SCOPE(__FUNCTION__);
			auto guard = read_fence();
			for (size_t i = 0; i < system_infos.size(); i++) {
				auto& system_info = system_infos[i];
				auto iterators_begin = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);
				auto iterators_end = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);

				if (match_iterators<decltype(iterators_begin), 0, for_components_t...>(iterators_begin, iterators_end, system_info)) {
					size_t total = iris_verify_cast<size_t>(std::get<0>(iterators_end) - std::get<0>(iterators_begin));
					for (size_t i = 0; i < total; i += batch_count) {
						size_t count = std::min(i + batch_count, total) - i;
						do_operation_batch(iterators_begin, op, iris_make_sequence<std::tuple_size<decltype(iterators_begin)>::value>(), count);
						step_iterators(iterators_begin, count);
					}
				}
			}
		}

		template <typename... for_components_t, typename operation_t>
		void filter(entity_t entity, operation_t&& op) {
			entity_t arr[1] = { entity };
			filter<for_components_t...>(&arr[0], &arr[0] + 1, std::forward<operation_t>(op));
		}

		template <typename... for_components_t, typename iterator_t, typename operation_t>
		void filter(iterator_t begin, iterator_t end, operation_t&& op) {
			auto guard = read_fence();

			for (size_t i = 0; i < system_infos.size(); i++) {
				auto& system_info = system_infos[i];
				auto iterators_begin = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);
				auto iterators_end = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);

				if (match_iterators<decltype(iterators_begin), 0, for_components_t...>(iterators_begin, iterators_end, system_info)) {
					auto& entity_components = *system_info.entity_components;
					auto ip = entity_components.begin();
					iterator_t p = begin;
					auto it = iterators_begin;

					size_t last_index = 0;
					while (p < end) {
						ip = iris_binary_find(ip, entity_components.end(), *p);
						if (ip == entity_components.end() || ip->second == ~index_t(0)) {
							break;
						}

						if (last_index < ip->second) {
							it = iterators_begin;
							last_index = 0;
						}

						step_iterators(it, ip->second - last_index);
						do_operation(it, op, iris_make_sequence<std::tuple_size<decltype(iterators_begin)>::value>());
						++p;
					}
				}
			}
		}

		template <size_t i, typename entity_iterator_t, typename iterators_t>
		bool locate_iterators_joined(entity_iterator_t p, iterators_t& it, iterators_t& iterators_begin, size_t last_indices[], std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>* entity_components[], typename std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>::iterator* ips) {
			return true;
		}

		template <size_t i, typename first_component_t, typename... components_t, typename entity_iterator_t, typename iterators_t>
		bool locate_iterators_joined(entity_iterator_t p, iterators_t& it, iterators_t& iterators_begin, size_t last_indices[], std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>* entity_components[], typename std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>::iterator* ips) {
			auto& entity_components_slice = *(entity_components[i]);
			auto ip = ips[i];

			size_t& last_index = last_indices[i];
			ip = iris_binary_find(ip, entity_components_slice.end(), *p);
			if (ip == entity_components_slice.end() || ip->second == ~index_t(0)) {
				return false;
			}

			ips[i] = ip;
			if (last_index < ip->second) {
				std::get<i>(it) = std::get<i>(iterators_begin);
				last_index = 0;
			}

			step_iterators(it, ip->second - last_index);
			return locate_iterators_joined<i - 1, components_t...>(p, it, iterators_begin, last_indices, entity_components, ips);
		}

		template <typename... for_components_t, typename iterator_t, typename operation_t>
		void filter_joined(iterator_t begin, iterator_t end, operation_t&& op) {
			auto guard = read_fence();
			auto iterators_begin = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);
			auto iterators_end = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);

			std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>* entity_components[sizeof...(for_components_t)] = { nullptr };
			typename std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>::iterator ips[sizeof...(for_components_t)] = {};
			for (size_t i = 0; i < system_infos.size(); i++) {
				auto& system_info = system_infos[i];
				size_t total_count = match_iterators_joined<decltype(iterators_begin), 0, for_components_t...>(iterators_begin, iterators_end, system_info, entity_components);
				if (total_count == sizeof...(for_components_t))
					break;
			}

			for (size_t j = 0; j < sizeof...(for_components_t); j++) {
				ips[j] = entity_components[j]->begin();
			}

			size_t last_indices[sizeof...(for_components_t)] = { 0 };
			auto it = std::make_tuple(typename iris_queue_quick_list_t<for_components_t, allocator_t>::iterator()...);

			for (iterator_t p = begin; p != end; ++p) {
				if (locate_iterators_joined<sizeof...(for_components_t) - 1, for_components_t...>(p, it, iterators_begin, last_indices, entity_components, ips)) {
					do_operation(it, op, iris_make_sequence<std::tuple_size<decltype(iterators_begin)>::value>());
				}
			}
		}

		template <typename... for_components_t, typename operation_t>
		void filter_joined(entity_t e, operation_t&& op) {
			return filter_joined<for_components_t...>(&e, &e + 1, std::forward<operation_t>(op));
		}

		template <typename iterators_t, size_t i>
		size_t match_iterators_joined(iterators_t& iterators_begin, iterators_t& iterators_end, const system_info_t& system_info, std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>* entity_components[]) noexcept {
			return 0;
		}

		template <typename iterators_t, size_t i, typename first_component_t, typename... components_t>
		size_t match_iterators_joined(iterators_t& iterators_begin, iterators_t& iterators_end, const system_info_t& system_info, std::vector<iris_key_value_t<entity_t, index_t>, vector_allocator_t<iris_key_value_t<entity_t, index_t>>>* entity_components[]) noexcept {
			if (entity_components[i] == nullptr) {
				size_t hash = get_type_hash<first_component_t>();
				auto it = iris_binary_find(system_info.components.begin(), system_info.components.end(), hash);
				if (it != system_info.components.end()) {
					auto* list = reinterpret_cast<iris_queue_quick_list_t<first_component_t, allocator_t>*>(it->second);
					std::get<i>(iterators_begin) = list->begin();
					std::get<i>(iterators_end) = list->end();
					entity_components[i] = system_info.entity_components;
				}
			}

			return (size_t)(entity_components[i] != nullptr) + match_iterators_joined<iterators_t, i + 1, components_t...>(iterators_begin, iterators_end, system_info, entity_components);
		}

	protected:
		template <typename system_t>
		static bool remove_handler(void* address, entity_t entity) {
			return reinterpret_cast<system_t*>(address)->remove(entity);
		}

		template <typename system_t>
		static void clear_handler(void* address) {
			reinterpret_cast<system_t*>(address)->clear();
		}

		template <typename system_t>
		static void compress_handler(void* address) {
			reinterpret_cast<system_t*>(address)->compress();
		}

		template <typename system_t, size_t... i>
		std::vector<iris_key_value_t<size_t, void*>> generate_info(system_info_t& info, system_t& sys, iris_sequence<i...>) {
			return std::vector<iris_key_value_t<size_t, void*>>{ iris_make_key_value(get_type_hash<typename std::tuple_element<i, typename system_t::components_tuple_t>::type>(), reinterpret_cast<void*>(&sys.template component<typename std::tuple_element<i, typename system_t::components_tuple_t>::type>()))... };
		}

		template <typename iterators_t, size_t i>
		bool match_iterators(iterators_t& iterators_begin, iterators_t& iterators_end, const system_info_t& system_info) noexcept {
			return true;
		}

		template <typename iterators_t, size_t i, typename first_component_t, typename... components_t>
		bool match_iterators(iterators_t& iterators_begin, iterators_t& iterators_end, const system_info_t& system_info) noexcept {
			size_t hash = get_type_hash<first_component_t>();
			auto it = iris_binary_find(system_info.components.begin(), system_info.components.end(), hash);
			if (it != system_info.components.end()) {
				auto* list = reinterpret_cast<iris_queue_quick_list_t<first_component_t, allocator_t>*>(it->second);
				std::get<i>(iterators_begin) = list->begin();
				std::get<i>(iterators_end) = list->end();

				return match_iterators<iterators_t, i + 1, components_t...>(iterators_begin, iterators_end, system_info);
			} else {
				return false;
			}
		}

	protected:
		std::vector<system_info_t, vector_allocator_t<system_info_t>> system_infos;
	};
}
