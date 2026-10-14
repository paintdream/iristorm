# 设计：`set_current_proxy` — 成员容器的引用视图（proxy view）

## 1. 问题

`set_current<&T::member>("name")` 对成员对象的绑定走 `property_get_proxy` / `property_set_proxy`
（iris_lua.h:3419-3456）：

- `__get`：`push_variable(L, object->*prop)` —— 把成员**拷贝**一份压栈；
- `__set`：`object->*prop = get_variable<...>(L, 3)` —— 整体赋值。

于是 Lua 侧对 `std::vector` 成员只能"读出整个 vector → 改 → 写回"。目标是支持原位访问：

```lua
a.vec[1] = 5
a.vec:push_back(7)
print(#a.vec)
```

## 2. 核心思路

`a.vec` 不再返回成员的拷贝，而是返回一个 **proxy userdata**，其中按值存放一个轻量 view：

```cpp
template <typename C>
struct proxy_view_t {
    C* ptr;   // 指向宿主对象内嵌的那个容器对象本身（不是元素）
};
```

关键点：`ptr` 指向**嵌入在宿主里的容器对象**。vector 扩容搬动的是元素，容器对象本身的地址不变，所以 `push_back` 导致的重分配不会使 proxy 失效 —— 这正是"包一层指向容器而非指向元素"的原因。

proxy 自身注册为一个普通 lua 类型（有自己的 metatable / `lua_registar`），`__index`/`__newindex`/`__len` 全部转发给 `*ptr`。

## 3. 与现有基础设施的对接（都已存在，无需新机制）

| 需求 | 现成设施 |
| --- | --- |
| 引用型 userdata | `native_push_object_view`（1245 行）的 view 模式：userdata 存 `type_t*`，`size_mask_view` 标记 |
| userdata 内嵌额外数据 | `has_lua_view_payload` / `lua_view_initialize`（1253-1267 行） |
| uservalue 槽位 | `has_lua_uservalue_count`（1156 行）—— **仅 5.4+ 有效**（≤5.3 时 `lua_newuserdatauv` 退化为 `lua_newuserdata`，uservalue 参数被丢弃），因此锚定机制**不依赖 uservalue**，见 §4.4 |
| 类型注册时机 | `make_type` 在装好 `__index`/`__newindex` 之后才调用自定义 `lua_registar`（1078 行）→ proxy 的 registar 可以**覆盖** `__index`/`__newindex`，也可以退回委托给已装好的 `__get`/`__set` 分发 |

实现上比 view 机制更简单：proxy_view_t 是平凡可析构的（就一个指针），直接把
`proxy_view_t<C>` **按值放进 userdata**（类似 `push_functor_internal` 2460 行的做法），
不需要 `__gc`，不需要 `size_mask_view`。

## 4. 新增 API

### 4.1 `iris_lua_t::set_current_proxy`

```cpp
// 只接受成员对象指针；与 set_current 的 member-object 分支同构
template <auto ptr, typename type_t = void, typename key_t, typename... envs_t>
reflection_t set_current_proxy(key_t&& key, envs_t&&... envs);
```

行为（对照 set_current 1489-1510 行的 member-object 分支）：

- `__get["name"]` ← `property_proxy_get_proxy<ptr, type_t>`：
  1. `type_t* object = get_variable<type_t*>(L, 1);`
  2. `C* c = &(object->*ptr);`
  3. new userdata（对齐、5.4+ 声明 1 个 uservalue，低版本 0 个），原地构造 `proxy_view_t<C>{c}`；
  4. 挂上 `proxy_view_t<C>` 的 registry 类型 metatable（≤5.3 时内含 `__gc` 释放宿主锚，见 §4.4）；
  5. 锚住宿主（§4.4：5.4+ 走 uservalue，≤5.3 走 registry ref）。
- `__set["name"]`：保留整体赋值语义（`object->*ptr = ...`），与非 proxy 绑定兼容；对不可拷贝赋值的容器直接不注册。
- `const` 成员：只注册 `__get`，proxy 持有 `const C*`，只暴露只读操作。

### 4.2 proxy 类型的 `lua_registar`

```cpp
template <typename C>
struct proxy_view_t {
    using value_t = typename C::value_type;

    C* ptr = nullptr;

    // ---- 转发的元方法（直接 rawset 进类型表，类型表即 metatable）----
    static int lua_index(lua_State* L);    // 仅数据键：整数 -> (*ptr)[i-1]；map -> find(k)。不查方法表
    static int lua_newindex(lua_State* L); // 仅数据键：整数 -> (*ptr)[i-1] = v；map -> (*ptr)[k] = v
    static int lua_len(lua_State* L);      // -> ptr->size()

    // ---- 方法（set_current 进类型表 = metatable，经 getmetatable(p).xxx(p, ...) 调用，见 §4.2.1）----
    void push_back(value_t v)   { ptr->push_back(std::move(v)); }
    void pop_back()             { ptr->pop_back(); }
    size_t size() const         { return ptr->size(); }
    bool empty() const          { return ptr->empty(); }
    void clear()                { ptr->clear(); }
    void resize(size_t n)       { ptr->resize(n); }
    // insert/erase/at(越界走 result_error 风格)/clone(§4.2.2)/keys/values/items(§4.2.3) 按需加

    static void lua_registar(iris_lua_t lua, iris_lua_traits_t<proxy_view_t>) {
        lua.set_current(&proxy_view_t::push_back, "push_back");
        // ...
        lua.set_current("__len", &proxy_view_t::lua_len);
        lua.set_current("__index", &proxy_view_t::lua_index);       // 覆盖 make_type 预装的
        lua.set_current("__newindex", &proxy_view_t::lua_newindex);
    }
};
```

`lua_index` 只做数据键查找（覆盖预装 `index_proxy` 是因为键是运行时动态的，
`__get`/`__set` 表按注册期名字建键，且不能让方法名遮蔽用户数据键，见 §4.2.1）：

1. 键是 integer → 越界检查后返回 `(*ptr)[i-1]`（负数按 Lua 惯例解释为 `size+i`）；
   map 面貌则按键类型走 `find`；
2. 未命中返回 nil —— **不回落方法表**。

### 4.2.1 方法命名空间：不遮蔽，方法只经 metatable 可达

proxy 的 `__index`/`__newindex` **只做数据键查找**（vector 整数键 / map 任意键），不回落
到方法表 —— 数据键空间 100% 归用户，库方法一个都不占用。理由：

- 库无法控制用户行为：map 的数据键可能是任意字符串，任何"方法优先"分派都是在把库的
  命名空间强加给用户数据；
- 库自身演化（如新增 `clone`）不应有遮蔽已有用户数据键、改变既有代码行为的风险。

方法直接放在 metatable（即类型表）里，**不带下划线** —— 因为不与动态键共享命名空间，
无需 `__keys` 式的前缀规避。访问形式：

```lua
local m = getmetatable(p)     -- 或在获取 proxy 时一并返回方法表
m.push_back(p, 7)
m.keys(p)                     -- map
m.clone(p)                    -- 见下
```

（`__len`/`__index`/`__newindex` 等 VM 元方法照常在 metatable 里，不构成冲突：它们只在
VM 层被咨询，`p["__len"]` 作为数据键仍返回 map 中的对应值。）

### 4.2.2 `clone`：拥有型 proxy

`clone` 返回容器的独立拷贝，且拷贝仍是一个 proxy（Lua 面貌与 view 型完全一致）：

```cpp
template <typename C>
struct proxy_view_t {
    C* ptr = nullptr;
    bool owned = false;   // true: userdata 按值持有 C，__gc 析构；false: 指向宿主内嵌成员
#if LUA_VERSION_NUM < 504
    int owner_ref = LUA_NOREF;   // 仅 view 型使用
#endif
};
```

- userdata payload 尺寸取 `max(sizeof(proxy_view_t), sizeof(C) 对齐后)`：owned 型在
  payload 里原地 copy-construct 一份 `C`，`ptr` 指向自身 payload；view 型照旧；
- owned 型的 `__gc` 先析构 `C` 再（低版本）unref（owned 型无需锚，`owner_ref` 为 NOREF）；
- 两种形态共用同一个 metatable / 方法表，`m.push_back(c, v)` 对 clone 结果同样原位生效；
- 好处：**不需要全局注册 `std::vector<E>` 为 lua 类型**（§9 否决过的路径），
  clone 结果仍是可原位操作的 proxy 而非退化成只读快照。

### 4.2.3 迭代：快照语义，不用 `__pairs`

不提供 `__pairs`（有状态迭代器的失效检测不可靠：`std::map` 无 epoch，vector 的失效
信号也不可信，且 5.1/LuaJIT 根本没有 `__pairs`）。替代：

- **vector 零成本**：`for i = 1, #p do p[i] end` 走已有 `__len` + `__index`，无需任何
  新机制，天然无失效问题；
- **map 快照方法**：`m.keys(p)` / `m.values(p)` / `m.items(p)` 返回取值瞬间的 fresh
  Lua table。遍历的是快照，遍历中修改容器不影响本次遍历，失效问题被快照语义直接消解；
  O(n) 拷贝一次，循环外取一次即可接受。

### 4.3 使用方式

```cpp
struct test_t {
    std::vector<int> vec;
    static void lua_registar(iris_lua_t lua, iris_lua_traits_t<test_t>) {
        lua.set_current_proxy<&test_t::vec>("vec");
    }
};
```

```lua
a.vec[1] = 5                          -- __newindex -> (*vec)[0] = 5，原位写
print(#a.vec)                         -- __len -> size()
for i = 1, #a.vec do print(a.vec[i]) end

local m = getmetatable(a.vec)         -- 方法表（§4.2.1）
m.push_back(a.vec, 7)
local c = m.clone(a.vec)              -- 独立拷贝，仍是 proxy（§4.2.2）
m.push_back(c, 8)
for i, k in ipairs(m.keys(a.map)) do  -- map 快照迭代（§4.2.3）
    print(k, a.map[k])
end
```

proxy 的注册类型 `proxy_view_t<std::vector<int>>` 在宿主类型 `make_registry_type` 时
一并 `make_registry_type`（每元素类型一次，挂在同一个 lua_State 的 registry 上）。

### 4.4 宿主锚定：条件编译（5.4+ uservalue / ≤5.3 registry ref）

不能用单一 uservalue 方案：`LUA_VERSION_NUM <= 503` 时 `lua_newuserdatauv` 退化为
`lua_newuserdata`（iris_lua.h:60-62），uservalue 被静默丢弃，5.1/LuaJIT 上锚定会无声失效
导致宿主被 GC。因此按版本条件编译，各自取最优：

```cpp
template <typename C>
struct proxy_view_t {
    C* ptr = nullptr;
#if LUA_VERSION_NUM < 504
    int owner_ref = LUA_NOREF;   // 低版本：luaL_ref 到宿主 Lua 对象
#endif
};
```

- **5.4+**：创建时 `lua_pushvalue(L, 1); lua_setiuservalue(L, -2, 1);` 把宿主对象锚在
  proxy 的 uservalue（`get_lua_uservalue_count<proxy_view_t>` = 1）上。proxy 平凡可析构，
  无 `__gc`、无 unref，零额外开销。
- **≤5.3（含 5.1/LuaJIT）**：创建时 `lua_pushvalue(L, 1); luaL_ref(L, LUA_REGISTRYINDEX);`
  存入 `owner_ref`；proxy metatable 挂 `__gc` 做 `luaL_unref`（复用库内现有 userdata
  析构 stub 机制）。

两条路径全版本覆盖、无全局状态（uservalue 与 registry ref 均为 per-state、惰性创建，
符合 AGENTS 规则 4）。

## 5. 索引与错误语义

- Lua 1-based：`a.vec[i]` ↔ `(*ptr)[i-1]`；`i` 为负按 Lua 惯例 `size + i`；越界报参数错误
  （与 `property_get_proxy` 现有 `syserror(L, "error.parameter", ...)` 风格一致；
  这类调用点错误不属于"协程方法失败走 result_error_t"的范畴）。
- `a.vec[i]` 读：元素**一律按值**（现有 `push_variable` 规则压栈 —— 算术/字符串是值拷贝，
  嵌套容器深拷贝为 Lua table）。这是刻意决策而非实现局限：与 Lua 原生 table 的值语义一致，
  嵌套 view 的否决理由见 §8 第 5 条。

### 5.1 容器契约：结构化（鸭子类型），非 STL 耦合

`proxy_view_t<C>` 对 `C` 没有任何类型/继承要求，面貌与操作全部由**编译期结构探测**决定，
自定义容器只要长这个形状即可无侵入接入（已实测：`test/iris_lua_custom_container_demo.cpp`，
非 STL 类 `my_array_t`/`my_dict_t` 直接绑定通过）：

- 面貌判定：有 `mapped_type` → map 面貌；否则满足 queue/heap 契约（见 §5.2）→ queue/heap 面貌；否则 sequence；
- sequence 契约：`value_type`、`size()`、`empty()`、`clear()`、`resize(n)`、`operator[]`、
  `push_back`、`pop_back`、`emplace_back`（`__set` 表赋值路径经 `get_variable` 容器转换）、
  `begin() + n`（随机访问迭代器）、`erase(it)`、`insert(it, v)`、拷贝构造（clone）；
- map 契约：`key_type`、`mapped_type`、`size()`、`empty()`、`clear()`、`find(k)`/`end()`
  （迭代器可比较）、`count(k)`、`erase(k)`、`operator[]`、range-for 元素有 `.first/.second`、
  拷贝构造（clone）；
- 设计约束：`lua_registar` **无条件注册全部方法，新容器必须满足全量契约**（缺一项即编译失败，
  fail-fast）。这是刻意设计而非限制：保证 Lua 脚本对"换一个满足契约的容器"完全无感——
  方法集合恒定，不存在"这个容器少了某个方法"的探测分支；对脚本而言 sequence/map 面貌
  是稳定接口，容器实现是自由变量。

### 5.2 queue/heap 面貌（v2 新增）

无随机访问、无迭代的顺序结构。探测在 sequence 之前（与 sequence 契约不相交：queue 族没有
`operator[]`）。探测 traits：`iris_is_queue` / `iris_is_heap`（iris_lua.h，位于 iris_lua_t
之前，property_proxy_set_proxy 亦引用）。

- **queue 面貌**（`std::queue`、`iris_queue_list_t`）：
  `value_type`、`size()`、`empty()`、`front()`、`push(v)`、`pop()`；
  可选探测 `back()`（`std::queue` 有、`iris_queue_list_t` 无 —— `iris_is_backable`，有则注册）；
- **heap 面貌**（`std::priority_queue`）：
  `value_type`、`size()`、`empty()`、`top()`、`push(v)`、`pop()`。

行为差异（相对 sequence/map 面貌）：

- `__len → size()` 照常；`__index` 对任何键返回 nil（无数据键空间）、`__newindex` 报错
  （无元素赋值路径，`push()`/`pop()` 是唯一修改入口）；
- `clear`：无 `clear()` 的容器（std 两个 adaptor）以 drain 循环实现；
- `clone`：`is_copy_constructible_v` 门控照旧 —— `iris_queue_list_t` 为 move-only，
  不注册 clone（`lua_clone` 函数体同样门控，防止 move-only 容器误实例化）；
- `__set` 整体赋值：table 分支不走 `get_variable`（queue 族不 iterable），改为 clear +
  逐元素 `push`；proxy 拷贝赋值分支要求容器可拷贝赋值 —— move-only 成员（queue_list）在
  `set_current_proxy` 侧即不注册 `__set`（`is_copy_assignable_v` 门控；v2 修复了 v1 缺失
  此门控导致 move-only 成员编译失败的缺陷）；
- 空容器上的 `pop/front/top` 报 `error.exec`（std 容器空操作是 UB，绑定层必须拦截）。

`iris_queue_list_t` 的接入适配（iris_common.h）：补 `using value_type = element_t;` 与
`front()`（转发 `top()`，即 pop 侧端点）。并发声明（AGENTS 规则 3）：queue_list 的 `push`
走 in_fence、`pop/front/top` 走 out_fence，天然支持"一个 warp push、另一个 warp pop"；
但 `size()`（无 fence）、`clear` 为单线程所有权操作。一期未暴露 `get(i)` 随机读，如有需求
按 §5.1 fail-fast 原则单独评估。

值转换路径（`push_variable`/`get_variable` 的 table 互转）**不支持** queue 族：出方向无迭代
器、唯一遍历是破坏性 pop，语义不成立；队列交互全部经 proxy 完成，`__set` 的 table 赋值
分支（clear + push）是唯一"从 table 重建"入口。

测试覆盖（`test/iris_lua_demo.cpp` 的 `test_queue_faces`，2026-10 与 proxy/shared-element 子测试合并进单一 lua demo 工程）：

- `std::queue` / `std::priority_queue`：push/pop/front/back/top、`#`、空容器 `pop/front/top`
  报错、clone、table 与 proxy 两种整体赋值、元素赋值报错、const 成员只读视图；
- `iris_queue_list_t` 专项：`front()` 与容器 `top()` 一致（`front()` 转发 `top()`）、
  无 `back`/无 `clone`（move-only 门控生效）、`clear` 走原生实现、**跨块多节点**
  push/pop（`element_count = block_size/sizeof(int) = 256`，测试推 256*2+17 个元素并逐个
  校验 FIFO 出队顺序）、C++ 侧 view 宿主上 Lua push 后 C++ 按序读回并与 Lua 交替操作；
- move-only 的 `__set` 语义：`set_current_proxy` 不注册 `__set`，因此 `a.q = {...}`
  **不会**触及 C++（落到宿主默认 `__newindex`，成为静默 no-op）——测试断言 C++ 侧容器
  未被修改，而非期望报错；这是 `is_copy_assignable_v` 门控的直接后果，已固化为断言；
- 两个独立 `lua_State` 各自持有指向**同一** C++ 宿主的 proxy view，一端 push、
  另一端 front/pop（绑定层允许双端寻址同一宿主容器；容器自身的 SPSC 安全性由
  `test/iris_dispatcher_demo.cpp` 覆盖，本节不重复验证）。
实现备注（v2）：proxy 方法命名继续规避 `lua.h` 宏 —— 队列 pop 的绑定函数命名为
`lua_queue_pop`（`lua_pop` 是宏，恰好印证 §8 备注的必要性）。

## 6. 生命周期分析（重点）

| 场景 | 结论 |
| --- | --- |
| Lua GC 回收宿主 | proxy 持有宿主锚（§4.4：5.4+ uservalue / ≤5.3 registry ref，全版本有效）→ 宿主不会被回收。✅ |
| vector 内部重分配 | `ptr` 指向容器对象本身；扩容只是换掉 begin/end/cap（那是 vector 的成员，不是 vector 自身的地址），元素搬动不影响。✅ |
| 宿主 userdata 存储 | userdata 地址固定，Lua 拥有的宿主对象不会移动。✅ |
| 宿主被 move（move-construct 新宿主） | 旧宿主内嵌的 vector 变为 moved-from（空），但对象地址不变 → proxy **不悬垂**；且 fresh fetch `a.vec` 与 proxy 看到的是同一个对象，语义完全一致。✅ |
| 宿主存储被 C++ 侧迁移/销毁（如宿主本身存在 `std::vector<test_t>` 中扩容搬走） | proxy 悬垂。⚠️ 但这与现有 `make_object_view` 挂在 `&hosts[0]` 上的 view 是同一个 hazard、同一份保证 —— **proxy 不引入任何新的生命周期风险**。 |

结论：proxy 的生命周期保证严格等于"现有 view 语义 + 版本分派的 GC 锚（§4.4）"。C++ 侧拥有且会重定位的宿主，
本来就要遵守框架对 view 的既有约定（不重定位 / 重定位前失效所有 Lua 引用），proxy 无需额外规则，
也无需额外的 Debug 校验（宿主 move 前后 `&owner->*ptr` 并不变化，此类校验抓不住任何真实场景）。

补充：宿主为 **C++ 侧 view** 时，锚定的对象是 Lua 里那个 view userdata 而非 C++ 对象本身 ——
锚定保证的是"Lua 引用不消失"，C++ 对象自身的存活仍由 view 的既有约定管辖，这与 §6 最后一行一致。

## 7. 并发

- 每个转发操作照常走 `write_fence`（现有 set_current/property proxy 同款），单次
  `a.vec[i] = 5` 与 `push_back` 原子性等同现有属性写。
- 但 proxy 让 Lua 侧多出了细粒度变更路径，按 AGENTS 规则 3：凡是把带 proxy 成员的类型
  发布给多 warp 的，必须声明所有权 + 同步协议（proxy 不改变协议，只是扩大了可达面）。

## 8. 实施拆分（已全部落地，v1 完成）

宿主两种拥有方式（Lua 拥有 / C++ 侧 view）一期的锚定与生命周期分析均已覆盖（§6），
实现上无分叉：`property_proxy_get_proxy` 对 index 1 的宿主 userdata 一视同仁。

1. ✅ `proxy_view_t<C>` + 其 `lua_registar`（`__len`/`__index`/`__newindex`/基础方法，
   const 视图的 `__newindex` 也必须覆盖为报错版本 —— `make_type` 预装的默认 `__newindex`
   会让 const 写静默失效）；
2. ✅ `set_current_proxy` + `property_proxy_get_proxy`/`property_proxy_set_proxy`
   （版本分派宿主锚定、registry 类型惰性注册、`a.vector = proxy_or_table` 整体赋值）；
3. ✅ map 与 vector 双面貌（sequence/map 分派、方法不遮蔽数据键、`clone` 拥有型 proxy、
   `keys/values/items` 快照迭代、负索引、越界与类型错误）；
4. ✅ `test/iris_lua_demo.cpp` 的 `test_proxy_views`（MSVC 19.16 / C++17 与 C++20+协程模式编译通过，
   **Lua 5.5、Lua 5.1.5、LuaJIT 2.1（OpenResty rolling）三环境全断言通过**：
   5.1/LuaJIT 实测了 ≤5.3 的 registry ref 锚定路径；原有 `iris_lua_demo` 回归通过）。
   CMake target `iris_lua_demo`（`test_lua`）已加。
   注：iris_lua.h 补了一个 vanilla 5.1 缺口 shim：`luaL_loadbufferx` → `luaL_loadbuffer`
   （5.2+ API，库的 `load()` 无条件调用；门控 `!defined(LUAJIT_VERSION)` —— LuaJIT 虽是
   `LUA_VERSION_NUM 501` 但自带 `luaL_loadbufferx`，shim 必须避开它以免丢 mode 参数）；
5. ❌ ~~二期：嵌套容器链式 proxy~~ —— **已否决**。理由：
   - 元素地址不稳定是原理性障碍：`vector<vector<int>>` 的行活在外层堆缓冲区里，
     外层扩容/erase 时整块搬移，元素 view 无法靠任何锚定机制保证有效（GC 锚不住 C++ 重定位）；
     map 值虽是节点存储地址稳定，但"能否 view"随容器类型而变，语义组合爆炸；
   - 按值取元素与 Lua 原生 table 的值语义一致，无意外；`a.vector[i] = 5` 的标量诉求已闭环；
   - 写路径维持框架既有语义：注册类型按值赋入即 move-out（`get_variable` 的 take 语义），一致优先；
   - 嵌套原位修改的正确建模：外层容器装注册类型、成员挂 proxy（成员地址稳定），
     如 `map<string, row_t>` + `row_t::values`。将来若真有刚需，加显式 opt-in 的
     `row_view(i)`（文档写明失效规则），不让 `[]` 隐式返回 view。
6. ✅ v2：queue/heap 面貌（§5.2）—— std::queue / std::priority_queue / iris_queue_list_t
   三容器经 `set_current_proxy` 接入；`test/iris_lua_demo.cpp` 的 `test_queue_faces`
   （CMake target `iris_lua_demo` / `test_lua`），MSVC 2026 / Lua 5.5 编译运行通过；
   proxy / shared-element 子测试在同一工程内回归通过。附带修复 v1 缺陷：
   `set_current_proxy` 未按 `is_copy_assignable_v` 门控 `__set` 注册（move-only 成员
   编译失败），与 §4.1 声明不符。

实现备注：
- proxy 方法名避免 `lua_insert` 等与 Lua 5.4+ `lua.h` 宏冲突（宏在编译期会吃掉同名标识符）；
- `proxy_view_t` 不可拷贝/移动 —— 防止跨 VM 搬运/复制 machinery 经 `__copy`/`__move` 复制
  header 导致 owned payload 双重析构与悬垂，proxy 是 per-VM 瞬态视图；
- 锚定实现：`push_proxy_view_variable` 在 5.4+ 用 `lua_setiuservalue`，≤5.3 用 `luaL_ref`
  存入 `owner_ref`，释放统一走 `lua_finalize` 钩子（`make_type` 自动注册的 `__gc`
  `delete_object` 会调用它，owned 形态同时负责析构 payload 容器）。

## 9. 备选方案（为何不选）

- **直接注册 `std::vector<E>` 本身为 lua 类型**，`a.vec` 走 `native_push_object_view`：
  能复用最多代码，但把"vector 的 Lua 面貌"全局固定成一套，且对 `const`/只读场景、
  按宿主定制暴露哪些操作都不灵活。作为 proxy_view_t 的一个薄特化实现倒是可以。
- **getter 返回轻量 table 快照 + setter 深写回**：就是现状的语法糖，仍有 O(n) 往返和丢引用问题，不解决诉求。
