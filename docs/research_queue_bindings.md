# 调研：std::queue / std::priority_queue / iris_queue_list_t 的 Lua 绑定可行性

现有 ducktype 绑定（`iris_lua.h`）有两条独立路径，三个候选容器在两条路径上的可行性完全不同：

| 路径 | 机制 | 三个候选的现状 |
| --- | --- | --- |
| **值转换** | `push_variable` / `get_variable`（iris_lua.h:4061 / 3043）：`iris_is_iterable`（有 `std::begin/std::end`，iris_common.h:578）→ 转 Lua table / 从 table 重建（sequence 走 `emplace_back`，map 走 `operator[]`） | **全部失败**。三个容器都无 `begin/end` → `iris_is_iterable=false` → 落到默认 `iris_lua_traits_t::lua_fromstack` → 编译错误 |
| **代理视图** | `proxy_view_t<C>` + `set_current_proxy`（iris_lua.h:4429 / 1543）：结构化契约（设计文档 §5.1），sequence 面貌要求 `operator[]`/`push_back`/`begin()+n`/`resize`/拷贝构造，map 面貌要求 `mapped_type` | **全部失败**。都不满足 sequence 契约（queue/pq 无 `operator[]`、无迭代；queue_list 有 `get(i)` 但无 `operator[]`/`push_back`，且拷贝构造 deleted） |

结论：**三条都不可行于现有机制，但都可经"新增第三面貌（buffer/queue 面貌）"低侵入接入 proxy 路径**。值转换路径建议维持不支持（理由见 §4）。

## 1. std::queue — 可行，走新面貌

- 是 adaptor，底层容器 `c` 为 protected，但**不需要**碰底层：`size/empty/front/back/push/pop` 全部公开且自足。
- 新面貌契约（queue 面貌，与 sequence/map 并列）：

  ```text
  value_type, size(), empty(), front(), back(), push(v), pop()
  ```
- Lua 表面：`__len → size()`；`__index` 不注册整数数据键（无随机访问）；`__newindex` 覆盖为报错版本（同 const view 现有做法，否则 make_type 预装的默认 `__newindex` 会静默丢写）；方法 `front/back/push/pop/size/empty/clear/clone`。
- `clear()`：std::queue 没有，用 `while(!empty()) pop()` 实现（O(n)，语义成立）。
- `pop()` 空队列是 UB → Lua 方法必须先 `empty()` 检查并 `syserror`（与现有 `lua_pop_back` 一致，iris_lua.h:4670）。
- `clone` 走 `is_copy_constructible_v` 门控（现有机制，iris_lua.h:4833），std::queue 默认可拷贝 → 直接可用。
- `__set` 整体赋值的 table 分支：`get_variable<member_t>` 不可用（不 iterable），改为 `clear + for i=1,#t push(t[i])` 逐元素灌入。
- 并发：与 std::vector 同级 —— 无同步的顺序结构，遵守 AGENTS 规则 3 的单 warp 所有权协议即可，不引入新风险。

## 2. std::priority_queue — 可行，同一路径的 heap 变体

- 公开表面：`size/empty/top/push/pop`。与 std::queue 的差异只有两点：`top()` 代替 `front()/back()`；`top()` 返回 `const T&`（push_variable 按值拷贝压栈，无碍）。
- 与 queue 面貌合并还是拆分是主要设计决策：
  - **合并**（一个 buffer 面貌，core 契约 = `size/empty/push/pop` + "front 或 top"，`back` 探测式可选注册）：实现最小，但违背设计文档 §5.1 "固定契约、方法集合恒定、脚本对换容器无感" 的 fail-fast 原则——不过 FIFO 与堆序本来就是脚本可感知的语义差异，脚本不可能在两者间无感替换，此处的"无感"诉求并不成立，可以接受。
  - **拆分**（queue 面貌 / heap 面貌两个）：契约各自全量固定，更贴合现有设计哲学，代码增量也不大（heap 方法集是 queue 的子集减 back 加 top）。
  - 建议 **拆分**，与 §5.1 精神一致；`top` 在 heap 面貌下固定注册。
- 比较器是 C++ 模板参数，Lua 侧无法定制 —— 文档写明"比较逻辑在 C++ 侧定型"即可，不是障碍。
- `pop()` 空堆 UB、`clear()` 缺失：处理方式同 std::queue。

## 3. iris_queue_list_t — 可行性最高，但有三处真实缺口

自家容器（iris_common.h:2143），表面比前两者丰富：`push(v)`/`pop()`/`pop(n)`/`top()`/`size()`/`empty()`/`clear()`/`get(index)` 随机访问/`for_each`/完整 iterator（range-for 可用，`iterator+n` 也有）。行为上 `top()+pop()` 就是 FIFO 出队 → 对应 queue 面貌。缺口：

1. **命名不匹配**：无 `value_type`（是 `element_t`）、无 `front()`（是 `top()`）。自家容器可以直接加两个别名成员（`using value_type = element_t; reference front() { return top(); }`），零成本满足 queue 面貌探测 —— 不需要为它做探测分支，污染面最小。（`back()` 不提供，heap/queue 拆分方案下 queue 面貌若固定含 back，则 queue_list 需要一个 `back()` —— 但队列链尾元素在 push 侧、跨 fence 读取，建议 queue 面貌把 back 定为 heap 变体同款的可选项，或给 queue 面貌定义两个 tier。更简单：契约核心 = `size/empty/push/pop/front`，`back` 探测可选，queue_list/pq 不提供。）
2. **move-only**：拷贝构造/赋值 deleted（iris_common.h:2152-2153）。影响三处，其中一处是**现有代码的真实缺陷**（见 §5）：
   - `clone`：已被 `is_copy_constructible_v` 门控，安全跳过 ✓；
   - `__set` 整体赋值（property_proxy_set_proxy，iris_lua.h:3872-3879）：`member = *source->ptr` 需要拷贝赋值 → **无条件实例化，move-only 成员一挂 set_current_proxy 即编译失败**。需要补门控：不可拷贝赋值时不注册 `__set`（设计文档 §4.1 明说"对不可拷贝赋值的容器直接不注册"，但代码没实现）。若要保留赋值语义，可走 move 赋值 + take 语义（对齐 AGENTS 规则 5 注册类型 by-value 即 move-out 的既有约定），属设计决策点。
   - 值转换 `get_variable`：本来就建议不支持，无碍。
3. **并发语义是三者的亮点也是唯一需要认真声明的点**：`enable_in_out_fence_t`（iris_common.h:263）表明它按"push 侧单写者 + pop 侧单写者"（SPSC 可优化）设计，`push` 走 in_fence、`top/pop/get/begin` 走 out_fence。这**天然允许**"一个 warp 经 proxy push、另一个 warp 经另一个 proxy pop"——proxy 是 per-VM 瞬态视图但各自指向同一 C++ 宿主成员，不跨 VM 转移（proxy_view_t 本就不可拷贝，iris_lua.h:4443）。注意事项：
   - `size()`（无 fence）、`begin/end`（begin 走 fence）、跨段 `get(i)` 在并发下只是近似值/竞态读，Lua 面貌上建议：并发声明只覆盖 push/pop/front/empty，`get(i)`/`__len`/clear 标注为单线程所有权操作；
   - `top()` 在空容器上的行为依赖 cleanup_empty 分支，Lua 方法入口统一 `empty()` 检查报错；
   - `get(index)` 越界最终落到 `push_head->get(index)` 兜底（iris_common.h:2354），是 UB → proxy 层必须先做范围检查（size 是 O(节点数) 遍历，接受）。

另外一个加分项：该容器已在绑定层内部使用（`encode()` 默认字节流就是 `iris_queue_list_t<uint8_t>`，iris_lua.h:1936），绑定它是把既有内部结构升为一等公民，风格自洽。

## 4. 值转换路径（函数参数/返回值/encode）—— 建议不支持

- **入方向**（table → 容器）：技术上可行（clear + 逐元素 push），且非破坏性；`__set` 的 table 分支会顺手获得。
- **出方向**（容器 → table）：queue/pq **无迭代器**，唯一遍历方式是破坏性 pop —— 要么拷贝一份再 drain（O(n) 拷贝 + 元素要求可拷贝），要么语义上就错误。queue_list 有迭代器可正常遍历，但 `size()` 无 fence 下并发遍历语义弱。
- 与现有"iterable 才转换"的判定一致地排除三者，行为可预测；proxy 面貌已覆盖全部交互诉求（读 front/top、push、pop、size、clear）。建议文档明确"queue 族仅经 proxy 暴露，不做值语义"。

## 5. 调研中发现的现有缺陷（与本次目标直接相关）

`set_current_proxy`（iris_lua.h:1562-1573）只按 const 成员分派 `__set` 注册，未按 `is_copy_assignable_v<member_t>` 门控。对 `iris_queue_list_t` 这类 move-only 成员，`property_proxy_set_proxy` 中 `member = *source->ptr`（3873 行）会在实例化时编译失败。设计文档 §4.1 声称的"不可拷贝赋值直接不注册"并未落地。**接入 queue_list 前必须先修**，属于前置工作而非新增功能。

## 6. 建议的实施顺序

1. 修 `set_current_proxy` 的 `__set` 拷贝赋值门控（前置缺陷，独立提交）；
2. proxy_view_t 新增 queue 面貌与 heap 面貌（固定契约：queue = `front/back/push/pop/size/empty/clear(循环)/clone(门控)`；heap = `top/push/pop/size/empty/clear(循环)/clone(门控)`），`__len` 转发 size，`__index`/`__newindex` 不注册数据键、写路径报错，pop/front/top 空检查报 `error.exec`；
3. `iris_queue_list_t` 加 `value_type`/`front()` 别名后按 queue 面貌接入（back 不提供，文档标注），可选：用 `get(i)` 额外暴露只读整数索引（需自行做范围检查 + 并发标注）；
4. demo：扩展 `test/iris_lua_demo.cpp`（`test_queue_faces` 子测试）覆盖三容器 × {push/pop/front|top/empty/clear/clone/整体赋值}，沿用三环境（5.5 / 5.1.5 / LuaJIT）回归矩阵；
5. 文档：`design_set_current_proxy.md` 增补 queue/heap 面貌契约与 queue_list 的并发声明（push/pop 跨 warp 合法，size/get/clear 单线程）。

方法命名注意沿用现有约定：避开 `lua_insert` 等 Lua 头文件宏（设计文档实现备注），`push`/`pop`/`front`/`top` 均安全。
