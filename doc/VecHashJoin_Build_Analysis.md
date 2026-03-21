# VecHashJoin Build 阶段全流程分析

本文档对 openGauss 向量化哈希连接（VecHashJoin）的 **Build（构建）阶段** 进行全面深入的源码级分析，涵盖从算子初始化、内表数据读取、哈希 cell 构建、内存管理与溢出处理、哈希表构建、布隆过滤器下推，到最终转入 Probe 阶段的完整流程。

> **源码位置**
> - 实现文件：`src/gausskernel/runtime/vecexecutor/vecnode/vechashjoin.cpp`
> - 头文件：`src/include/vecexecutor/vechashjoin.h`
> - 哈希表与基类定义：`src/include/vecexecutor/vechashtable.h`
> - 节点状态：`src/include/vecexecutor/vecnodes.h`

---

## 目录

1. [整体架构概览](#1-整体架构概览)
2. [顶层入口：ExecVecHashJoin 的 HASH_BUILD 分支](#2-顶层入口execvechashjoin-的-hash_build-分支)
3. [算子初始化：HashJoinTbl 构造函数](#3-算子初始化hashjointbl-构造函数)
4. [内存控制初始化：initMemoryControl](#4-内存控制初始化initmemorycontrol)
5. [Join 键分析与分类](#5-join-键分析与分类)
6. [哈希函数设置](#6-哈希函数设置)
7. [批次与描述符初始化](#7-批次与描述符初始化)
8. [Join 类型映射：SetJoinType](#8-join-类型映射setjointype)
9. [函数指针绑定：bindingFp](#9-函数指针绑定bindingfp)
10. [Build 主循环：Build 方法](#10-build-主循环build-方法)
11. [内存存储：SaveToMemory](#11-内存存储savetomemory)
12. [内存充足性检查：HasEnoughMem](#12-内存充足性检查hasenoughmem)
13. [溢出到磁盘：策略切换流程](#13-溢出到磁盘策略切换流程)
14. [磁盘存储：SaveToDisk](#14-磁盘存储savetodisk)
15. [缓存刷盘：flushToDisk](#15-缓存刷盘flushtodisk)
16. [分区文件初始化：initFile](#16-分区文件初始化initfile)
17. [溢出文件数计算：calcSpillFile](#17-溢出文件数计算calcspillfile)
18. [哈希值计算机制](#18-哈希值计算机制)
19. [布隆过滤器下推：PushDownFilterIfNeed](#19-布隆过滤器下推pushdownfilterifneed)
20. [探测准备：PrepareProbe](#20-探测准备prepareprobe)
21. [哈希表构建：buildHashTable](#21-哈希表构建buildhashtable)
22. [关键数据结构](#22-关键数据结构)
23. [LLVM 优化路径](#23-llvm-优化路径)
24. [完整调用关系图](#24-完整调用关系图)

---

## 1. 整体架构概览

VecHashJoin 的 Build 阶段负责从内表（inner plan）读取全部数据并构建哈希表，为后续 Probe 阶段做准备。其整体架构如下：

```
ExecVecHashJoin(HASH_BUILD)          ← 顶层执行入口
  └─ new HashJoinTbl(node)           ← 算子初始化（仅首次调用）
       ├─ initMemoryControl()        ← 内存控制参数初始化
       ├─ 分析 Join 键               ← 确定 m_complicateJoinKey
       ├─ 设置哈希函数               ← m_innerHashFuncs / m_outerHashFuncs
       ├─ 初始化批次结构             ← m_innerBatch / m_outerBatch / m_result
       ├─ SetJoinType()              ← 映射 Join 类型
       └─ bindingFp<>()              ← 绑定 Build/Probe/Join 函数指针
  
  └─ HashJoinTbl::Build()            ← Build 主循环
       ├─ 循环: VectorEngine(inner)  ← 逐批获取内表数据
       │    └─ m_funBuild[m_strategy](batch)
       │         ├─ SaveToMemory()   ← [MEMORY_HASH] 内存存储
       │         │    ├─ HasEnoughMem()  ← 内存检查
       │         │    ├─ 分配 hashCell 数组
       │         │    ├─ 填充列值到 cell
       │         │    └─ 加入 m_cache 链表
       │         │
       │         └─ SaveToDisk()     ← [GRACE_HASH] 磁盘存储
       │              ├─ 计算哈希值
       │              └─ 按哈希分区写入临时文件
       │
       ├─ PushDownFilterIfNeed()     ← 构建布隆过滤器
       │
       └─ PrepareProbe()             ← 探测准备
            ├─ [MEMORY_HASH]
            │    ├─ buildHashTable()  ← 从 m_cache 构建哈希表
            │    └─ 创建 m_probOpSource
            └─ [GRACE_HASH]
                 └─ 设置初始分区状态
```

**核心设计特点：**
- **自适应内存管理**：初始以 MEMORY_HASH 策略在内存中积累数据，当内存不足时自动切换到 GRACE_HASH 策略溢出到磁盘
- **延迟构建哈希表**：先将数据以 hashCell 数组形式缓存在 m_cache 链表中，所有内表数据读取完毕后才在 `PrepareProbe()` 中真正构建哈希表
- **向量化批处理**：以 VectorBatch 为单位处理数据，每批最多 1000 行（BatchMaxSize）
- **模板特化消除分支**：通过 `complicate_join_key` 和 `simple` 模板参数在编译期消除运行时分支开销
- **函数指针分发**：通过 `m_funBuild[m_strategy]` 实现 MEMORY_HASH 和 GRACE_HASH 策略的零开销切换

---

## 2. 顶层入口：ExecVecHashJoin 的 HASH_BUILD 分支

**位置**：`vechashjoin.cpp:235-315`

`ExecVecHashJoin` 是向量化执行器调用 VecHashJoin 算子的入口函数，使用 `node->joinState` 状态机控制执行流程：

### HASH_BUILD 分支流程

```
1. 首次调用时创建算子对象：
   if (node->hashTbl == NULL):
     node->hashTbl = new HashJoinTbl(node)   ← 完整初始化

2. 执行 Build：
   ((HashJoinTbl*)node->hashTbl)->Build()
   rows = ((HashJoinTbl*)node->hashTbl)->getRows()

3. 提前释放内表子节点资源：
   ExecEarlyFree(innerPlanState(node))

4. 短路优化（哈希表为空时）：
   if (jointype 是 INNER/SEMI/RIGHT_SEMI/RIGHT/RIGHT_ANTI 且 rows == 0):
     ExecEarlyDeinitConsumer(node)
     return NULL    ← 不需要读取外表，直接结束

5. 继续进入 HASH_PROBE 状态（Build 结束后 joinState 已被设为 HASH_PROBE）
```

**说明**：
- 支持 Sonic Hash Join（`IS_SONIC_HASH`）和普通 Hash Join 两种实现
- 短路优化对于 LEFT/ANTI/LEFT_ANTI_FULL 不适用，因为这些 Join 类型即使内表为空也可能需要输出外表行

---

## 3. 算子初始化：HashJoinTbl 构造函数

**位置**：`vechashjoin.cpp:345-595`

构造函数完成 HashJoinTbl 的全部初始化工作，主要包括以下步骤：

### 3.1 成员变量初始化

```cpp
m_strategy = MEMORY_HASH;   // 初始策略为内存哈希
m_cache = NULL;              // 内存缓存链表
m_probeStatus = 0;
m_fill_table_rows = 0;
m_probeIdx = 0;
m_build_time = 0.0;
m_probe_time = 0.0;
```

### 3.2 内存控制初始化

调用 `initMemoryControl()` 计算可用内存预算，然后创建两个关键内存上下文：
- **m_hashContext**：哈希表数据的存储上下文，带有总内存限制（`m_totalMem`），支持 Rack 内存借用
- **m_tmpContext**：临时计算使用的上下文（如哈希值计算中的中间结果）

### 3.3 列数确定

```
if 是 RIGHT/RIGHT_ANTI_FULL/RIGHT_ANTI/RIGHT_SEMI:
  m_cols = inner_targetlist_length + 1    ← 额外一列用于匹配标记
else:
  m_cols = inner_targetlist_length
```

Right 类型的 Join 需要在 cell 的最后一列（`m_val[m_cols-1]`）存储匹配标记，用于 `endJoin()` 中识别未匹配的内表行。

### 3.4 Join 键分析

详见第5节。

### 3.5 哈希函数设置

详见第6节。

### 3.6 批次初始化

详见第7节。

### 3.7 Cell 大小计算

```cpp
if (m_complicateJoinKey)
    m_cellSize = offsetof(hashCell, m_val) + (m_cols + 1) * sizeof(hashVal);
else
    m_cellSize = offsetof(hashCell, m_val) + m_cols * sizeof(hashVal);
```

复杂键需要在 cell 末尾额外存储一个 hashVal 来保存预计算的哈希值。

### 3.8 Join 类型与函数指针绑定

```cpp
SetJoinType();           // 映射 Join 类型
bindingFp<true/false>(); // 绑定所有函数指针
ReplaceEqfunc();         // 替换特殊类型的等值函数
```

### 3.9 内存自动扩展上限

```cpp
if (node->operatorMaxMem > 0)
    m_maxMem = SET_NODEMEM(node->operatorMaxMem, node->dop) * 1024L;
```

`m_maxMem` 是内存自动扩展的上限，由优化器根据查询计划设置。

---

## 4. 内存控制初始化：initMemoryControl

**位置**：`vechashjoin.cpp:597-609`

```
m_totalMem = SET_NODEMEM(operatorMemKB[0], dop) * 1024L    ← 算子分配的工作内存
           + GetAvailRackMemory(dop) * 1024L                ← 可借用的 Rack 内存
m_availmems = m_totalMem
```

- `operatorMemKB[0]` 由优化器根据查询计划中的内存估算设定
- `SET_NODEMEM` 宏根据并行度（dop）调整单线程可用内存
- Rack 内存是集群中可共享的额外内存资源

---

## 5. Join 键分析与分类

**位置**：构造函数内，`vechashjoin.cpp:420-480`

构造函数遍历 `hj_InnerHashKeys` 和 `hj_OuterHashKeys`，将 Join 键分为两类：

### 5.1 简单键（m_complicateJoinKey = false）

当所有键都是简单的列引用（`Var`）或简单的类型转换（`RelabelType(Var)`）时，视为简单键：

```
对每个内表键:
  if 是 Var: 
    m_keyIdx[i] = varattno - 1     ← 列在内表中的位置
    m_simpletype[i] = simpletype(vartype)  ← 是否是简单比较类型
  if 是 RelabelType(Var):
    提取 Var，同上处理
  否则:
    m_complicateJoinKey = true  ← 标记为复杂键
    break

对每个外表键:
  m_outKeyIdx[i] = varattno - 1   ← 列在外表中的位置
  m_simpletype[i] &= simpletype(vartype)  ← 两侧都简单才算简单
```

### 5.2 复杂键（m_complicateJoinKey = true）

当任意键包含表达式（如函数调用、类型转换链等）时，标记为复杂键。复杂键需要：
- 使用 `CalcComplicateHashVal()` 通过表达式引擎计算哈希值
- 使用 `matchComplicateKey()` 通过表达式引擎进行键匹配
- Cell 中额外存储一列用于保存预计算的哈希值

### 5.3 simpletype 判断

`simpletype()` 函数判断类型是否支持直接按位比较（避免调用比较函数的开销）：
- 返回 true 的类型包括：INT1OID, INT2OID, INT4OID, INT8OID 等整数类型
- 返回 false 的类型需要通过 `m_eqfunctions` 注册的比较函数进行比较

---

## 6. 哈希函数设置

**位置**：构造函数内，`vechashjoin.cpp:486-502`

为每个 Join 键设置内表和外表的哈希函数：

```
遍历 hj_HashOperators:
  获取等值操作符 OID（如 int4eq 对应的 hashop）
  get_op_hash_functions(hashop, &left_hashfn, &right_hashfn)
  m_outerHashFuncs[i] = left_hashfn    ← 外表（probe 侧）使用
  m_innerHashFuncs[i] = right_hashfn   ← 内表（build 侧）使用
```

**说明**：
- 同一个等值操作符的左右哈希函数可能不同（当两侧类型不同时）
- 如果启用了 `enable_fast_numeric`，会将 numeric 类型的哈希函数替换为 biginteger 版本

---

## 7. 批次与描述符初始化

**位置**：构造函数内，`vechashjoin.cpp:510-580`

### 7.1 批次结构

```
m_innerBatch   ← 内表结果批次（用于存放探测匹配的内表数据）
m_outerBatch   ← 外表结果批次（用于存放探测匹配的外表数据）
m_inQualBatch  ← 内表 joinqual 检查批次（仅在有 joinqual 时创建）
m_outQualBatch ← 外表 joinqual 检查批次（仅在有 joinqual 时创建）
m_complicate_innerBatch ← 复杂键内表批次（仅在复杂键时创建）
m_complicate_outerBatch ← 复杂键外表批次（仅在复杂键时创建）
m_cjVector     ← 复杂键表达式计算的临时向量
m_result       ← 最终结果批次（按 ResultTupleSlot 描述符创建）
```

### 7.2 键描述符

```
m_keyDesc[i]   ← 每个 Join 键列的类型描述
m_keySimple    ← 1 表示所有键都不是 encoded 类型，0 表示存在 encoded 类型
m_colDesc[i]   ← 每个内表列的类型描述
m_innerSimple  ← 内表是否所有列都不含 encoded 类型
m_outSimple    ← 外表是否所有列都不含 encoded 类型
```

`encoded` 属性表示该列是否是变长类型（如 VARCHAR、TEXT），encoded 列在存储到 hashCell 时需要额外的内存拷贝（`addVariable()`）。

---

## 8. Join 类型映射：SetJoinType

**位置**：`vechashjoin.cpp:636-678`

将优化器的 Join 类型映射到向量化哈希 Join 的内部枚举：

| 优化器类型 | 内部枚举 | 说明 |
|---|---|---|
| JOIN_INNER | HASH_JOIN_INNER | 内连接 |
| JOIN_LEFT | HASH_JOIN_LEFT | 左外连接 |
| JOIN_RIGHT | HASH_JOIN_RIGHT | 右外连接 |
| JOIN_SEMI | HASH_JOIN_SEMI | 左半连接 |
| JOIN_ANTI | HASH_JOIN_ANTI | 左反连接 |
| JOIN_RIGHT_SEMI | HASH_JOIN_RIGHT_SEMI | 右半连接 |
| JOIN_RIGHT_ANTI | HASH_JOIN_RIGHT_ANTI | 右反连接 |
| JOIN_LEFT_ANTI_FULL | HASH_JOIN_LEFT_ANTI_FULL | 左反全连接 |
| JOIN_RIGHT_ANTI_FULL | HASH_JOIN_RIGHT_ANTI_FULL | 右反全连接 |

Right 类型的 Join（RIGHT、RIGHT_SEMI、RIGHT_ANTI）会在构造函数中为 `cellPoint` 分配额外的辅助数组，用于探测阶段的链表操作。

---

## 9. 函数指针绑定：bindingFp

**位置**：`vechashjoin.cpp:1294-1331`

`bindingFp<complicate_join_key>()` 是模板函数，在构造函数末尾调用，负责绑定 Build/Probe/Join 阶段的所有函数指针：

### 9.1 Build 函数指针 (m_funBuild)

```cpp
m_funBuild[0] = &HashJoinTbl::SaveToMemory<complicate_join_key, simple>;
m_funBuild[1] = &HashJoinTbl::SaveToDisk<complicate_join_key, true>;
```

| 索引 | 策略 | 函数 | 说明 |
|---|---|---|---|
| 0 | MEMORY_HASH | `SaveToMemory<CK, simple>` | 内存存储，`simple` 由 `m_innerSimple` 决定 |
| 1 | GRACE_HASH | `SaveToDisk<CK, true>` | 磁盘存储（build 侧） |

### 9.2 Probe 函数指针 (m_probeFun)

```cpp
m_probeFun[0] = &HashJoinTbl::probeMemory;
m_probeFun[1] = &HashJoinTbl::probeGrace;
```

### 9.3 Join 函数指针 (m_joinFun)

通过 `InitJoinTemplate` 宏将所有 Join 类型的模板实例填入 `m_joinFunArray[]`，然后根据 `m_joinType` 和是否有 `joinqual` 选择具体函数：

```cpp
array_idx = 有 joinqual ? (2 * m_joinType) + 1 : (2 * m_joinType)
m_joinFun = m_joinFunArray[base_idx + idx_array[array_idx]]
```

### 9.4 键匹配函数指针 (m_matchKeyFunction)

对于简单键，根据内表键的类型分发到对应的类型特化版本：
```cpp
DispatchKeyInnerFunction(i)
  → DispatchKeyOuterFunction<innerType>(i)
      → matchKey<innerType, outerType, true, nulleqnull>
```

对于非简单类型，统一使用通用比较函数版本。

---

## 10. Build 主循环：Build 方法

**位置**：`vechashjoin.cpp:876-924`

`Build()` 是 Build 阶段的核心入口，执行以下流程：

```
1. 内表数据读取循环:
   loop:
     batch = VectorEngine(innerPlanState)   ← 从内表子节点获取一批数据
     if batch 为空: break
     
     记录时间戳
     RuntimeBinding(m_funBuild, m_strategy)(batch)  ← 分发到 SaveToMemory 或 SaveToDisk
     累加 m_build_time

2. 后处理:
   PushDownFilterIfNeed()    ← 尝试构建布隆过滤器
   PrepareProbe()            ← 准备探测阶段

3. 性能统计记录:
   if 有 instrument:
     记录 hashbuild_time
     if MEMORY_HASH:
       记录 spaceUsed（m_hashContext 的 totalSpace）
       hash_writefile = false
     if GRACE_HASH:
       记录 hash_FileNum
       hash_writefile = true
     记录 spreadNum（内存自动扩展次数）

4. 释放 build 侧文件处理器缓冲区:
   m_buildFileSource->ReleaseAllFileHandlerBuffer()
```

**关键点**：
- `RuntimeBinding(m_funBuild, m_strategy)` 是宏展开为 `(this->*m_funBuild[m_strategy])`，实现零开销的策略分发
- Build 开始时 `m_strategy = MEMORY_HASH`（即 0），如果中途内存不足，`SaveToMemory` 会将 `m_strategy` 切换为 `GRACE_HASH`（即 1），后续批次自动走 `SaveToDisk` 路径
- 时间统计通过 `INSTR_TIME_SET_CURRENT` 和 `elapsed_time` 精确测量每批的处理耗时

---

## 11. 内存存储：SaveToMemory

**位置**：`vechashjoin.cpp:1085-1176`

**模板签名**：`template <bool complicate_join_key, bool simple> void SaveToMemory(VectorBatch* batch)`

当策略为 MEMORY_HASH 时，每个内表批次通过 `SaveToMemory` 存储到内存中：

### 11.1 内存检查

```
m_rows += batch->m_rows

if HasEnoughMem(rows) == false:
  → 触发溢出流程（详见第13节）
  return
```

### 11.2 分配 hashCell 数组

```
切换到 m_hashContext 内存上下文
cell_arr = palloc0(rows * m_cellSize)   ← 一次性分配整个批次的 cell 数组
m_colWidth += rows * m_cols * sizeof(hashVal)
```

cell 数组是一块连续内存，每个 cell 占 `m_cellSize` 字节，通过 `GET_NTH_CELL(cell_arr, i)` 宏按偏移访问第 i 个 cell。

### 11.3 计算复杂键哈希值

```
if complicate_join_key:
  CalcComplicateHashVal(batch, hj_InnerHashKeys, true)
  → 结果存入 m_cacheLoc[]
```

### 11.4 填充列值到 cell

逐列遍历批次数据，将每列的值复制到对应 cell 中：

```
对每列 j:
  if simple 或该列不是 encoded:
    直接拷贝值和 flag（定长类型，值在 ScalarValue 中）
  else:
    对每行:
      if 非 NULL:
        cell->m_val[j].val = addVariable(m_hashContext, value)
        → 在 m_hashContext 中分配空间并拷贝变长数据
        累加 m_colWidth
      拷贝 flag
```

**addVariable 函数**（`vechashtable.cpp:99-109`）：
```cpp
ScalarValue addVariable(MemoryContext context, ScalarValue val) {
    int key_size = VARSIZE_ANY(val);
    char* addr = palloc(key_size);       // 在 context 中分配
    memcpy_s(addr, key_size, DatumGetPointer(val), key_size);
    return PointerGetDatum(addr);
}
```

### 11.5 存储复杂键哈希值

```
if complicate_join_key:
  对每行 i:
    cell->m_val[m_cols].val = m_cacheLoc[i]   ← 哈希值存在最后一个位置
    cell->m_val[m_cols].flag = 0
```

### 11.6 加入缓存链表

```
m_cache = lcons(cell_arr, m_cache)   ← 将 cell 数组头插入链表
cell_arr->flag.m_rows = rows         ← 利用 flag 联合体的 m_rows 记录行数
m_tupleCount += rows
```

**关键设计**：`hashCell` 的 `flag` 是一个联合体（union），在缓存阶段借用 `m_rows` 字段存储该 cell 数组的行数，在哈希表阶段使用 `m_next` 字段形成桶链表。

---

## 12. 内存充足性检查：HasEnoughMem

**位置**：`vechashjoin.cpp:1007-1079`

`HasEnoughMem` 在每个批次存储前被调用，执行多层内存检查：

### 12.1 内存使用量计算

```
used_size = m_hashContext->totalSpace + rows * m_cellSize
```

### 12.2 系统内存压力检查

```
sys_busy = gs_sysmemory_busy(used_size * dop)      ← 系统级内存是否紧张
rackBusy = RackMemoryBusy(used_size * dop)          ← Rack 级内存是否紧张
rackAvail = GetAvailRackMemory(dop)                  ← 可用的 Rack 内存
```

### 12.3 决策逻辑

```
if used_size > m_totalMem 或 sys_busy 或 (本地内存耗尽 且 rackBusy):
  ① 如果是系统忙导致的提前溢出：
     记录警告，设置 m_totalMem = used_size
     
  ② 如果有自动扩展空间（m_maxMem > m_totalMem）：
     尝试获取额外内存: spreadMem = min(动态内存, m_totalMem, m_maxMem - m_totalMem)
     if spreadMem > m_totalMem * MEM_AUTO_SPREAD_MIN_RATIO:
       m_totalMem += spreadMem    ← 扩展成功
       m_spreadNum++
       return true
     else:
       扩展失败，记录日志
       
  ③ 无法扩展或扩展不足：
     记录溢出日志
     return false  ← 触发策略切换

else:
  m_availmems = m_totalMem - used_size
  return true  ← 内存充足
```

**关键特性**：
- **三级内存检查**：算子级（m_totalMem）、系统级（gs_sysmemory_busy）、Rack级（RackMemoryBusy）
- **内存自动扩展**：在 m_maxMem 限制内，可动态申请额外内存避免溢出
- **提前溢出（early spill）**：系统内存紧张时即使算子级未超限也会触发溢出

---

## 13. 溢出到磁盘：策略切换流程

当 `HasEnoughMem` 返回 `false` 时，`SaveToMemory` 内部触发策略切换：

```
1. 计算分区文件数:
   file_num = calcSpillFile()

2. 切换策略:
   m_strategy = GRACE_HASH    ← 后续批次将直接走 SaveToDisk

3. 初始化分区文件:
   initFile(true, batch, file_num)

4. 初始化分区级别:
   m_pLevel[i] = 1     ← 初始分区级别为 1
   m_isValid[i] = true

5. 记录峰值内存（性能统计）

6. 将已缓存数据刷到磁盘:
   flushToDisk<complicate_join_key>()     ← 将 m_cache 中的所有 cell 写入分区文件

7. 将当前批次写入磁盘:
   SaveToDisk<complicate_join_key, true>(batch)

8. 清理:
   pgstat_increase_session_spill()        ← 记录溢出事件
   MemoryContextReset(m_hashContext)      ← 释放哈希上下文中的所有内存
```

**关键时序**：策略切换是不可逆的——一旦切换到 GRACE_HASH，`m_strategy` 变为 1，后续所有 `Build()` 循环中的 `RuntimeBinding(m_funBuild, m_strategy)(batch)` 都会分发到 `SaveToDisk`。

---

## 14. 磁盘存储：SaveToDisk

**位置**：`vechashjoin.cpp:1209-1240`

**模板签名**：`template <bool complicate_join_key, bool build_side> void SaveToDisk(VectorBatch* batch)`

当策略为 GRACE_HASH 时，每个批次通过 `SaveToDisk` 写入分区临时文件：

```
1. 计算哈希值:
   if complicate_join_key:
     CalcComplicateHashVal(batch, hj_InnerHashKeys, true)
   else:
     hashBatch(batch, m_keyIdx, m_cacheLoc, m_innerHashFuncs, true)
   → 结果存入 m_cacheLoc[]

2. 按行写入分区文件:
   对每行 i:
     if complicate_join_key:
       file_source->writeBatchWithHashval(batch, i, hash_value)
       → 同时写入行数据和哈希值
     else:
       file_source->writeBatch(batch, i, hash_value)
       → 只写入行数据
```

**分区策略**：
```
file_idx = hash_value & (file_num - 1)
```
哈希值的低位直接决定写入哪个分区文件（文件数始终是 2 的幂）。

**writeBatch vs writeBatchWithHashval**：
- `writeBatch`：只写入行数据，分区文件读取时需要重新计算哈希值
- `writeBatchWithHashval`：同时写入行数据和哈希值，用于复杂键场景（避免重复执行表达式计算）

---

## 15. 缓存刷盘：flushToDisk

**位置**：`vechashjoin.cpp:1179-1206`

**模板签名**：`template <bool complicate_join_key> void flushToDisk()`

当从 MEMORY_HASH 切换到 GRACE_HASH 时，需要将之前缓存在 `m_cache` 链表中的所有 cell 数组刷到分区文件：

```
遍历 m_cache 链表:
  对每个 cell_arr:
    rows = cell_arr->flag.m_rows     ← 获取该数组的行数
    cell_arr->flag.m_next = NULL     ← 重置 flag（之前借用的 m_rows 字段）

    if !complicate_join_key:
      hashCellArray(cell_arr, rows, m_keyIdx, m_cacheLoc, m_innerHashFuncs, true)
      → 批量计算 cell 数组中所有行的哈希值（用于分区）

    对每行 i:
      cell = GET_NTH_CELL(cell_arr, i)
      
      if complicate_join_key:
        m_buildFileSource->writeCell(cell, cell->m_val[m_cols].val)
        → 使用 cell 中预存的哈希值写入
      else:
        m_buildFileSource->writeCell(cell, m_cacheLoc[i])
        → 使用刚计算的哈希值写入
```

**关键点**：
- 对于简单键，由于 SaveToMemory 阶段没有计算和存储哈希值，刷盘时需要重新计算
- 对于复杂键，SaveToMemory 阶段已经将哈希值存储在 `cell->m_val[m_cols].val` 中，直接使用
- `hashCellArray` 与 `hashBatch` 类似，但操作对象是 hashCell 数组而非 VectorBatch

---

## 16. 分区文件初始化：initFile

**位置**：`vechashjoin.cpp:732-795`

`initFile(bool build_side, VectorBatch* template_batch, int file_num)` 初始化 Build 侧或 Probe 侧的分区文件源：

### Build 侧初始化

```
1. 分配 cell 缓冲数组:
   cell_array = palloc0(BatchMaxSize * m_cellSize)   ← 用于从文件读取 cell

2. 创建文件源:
   if m_buildFileSource == NULL:
     创建 STACK_CONTEXT 内存上下文（带 m_totalMem 限制）
     m_buildFileSource = new hashFileSource(
       template_batch,     ← 模板批次（决定列数和类型）
       stack_context,
       m_cellSize,         ← cell 大小
       cell_array,         ← cell 缓冲
       m_complicateJoinKey,
       m_cols,
       file_num,           ← 分区文件数
       inner_desc           ← 元组描述符
     )
   else:
     m_buildFileSource->initFileSource(file_num)  ← rescan 时重新初始化

3. 关联溢出大小统计:
   m_buildFileSource->m_spill_size = &instrument->sorthashinfo.spill_size
```

### Probe 侧初始化

结构与 Build 侧类似，但：
- `m_cellSize` 传 0（probe 侧不需要读取 cell）
- `cell_array` 传 NULL
- 使用 outerPlanState 的描述符

---

## 17. 溢出文件数计算：calcSpillFile

**位置**：`vechashjoin.cpp:611-631`

```
rows = inner_plan->plan_rows                ← 优化器估算的内表行数
estimated_size = rows * (2 * sizeof(hashCell*) + Max(m_cellSize, plan_width))
file_num = getPower2NextNum(estimated_size / m_totalMem)
file_num = Max(32, file_num)                ← 最少 32 个文件
file_num = Min(file_num, 1024)              ← 最多 1024 个文件
```

**设计说明**：
- `2 * sizeof(hashCell*)` 是每行在哈希表中占用的桶指针空间的估算
- 文件数取 2 的幂（`getPower2NextNum`），便于后续按位与操作进行分区
- 下限 32 确保足够的并行度，上限 1024 避免文件句柄过多

---

## 18. 哈希值计算机制

### 18.1 简单键哈希：hashBatch

**位置**：`vechashtable.h:646-668`

对于简单列引用的 Join 键，使用 `hashBatch()` 批量计算整个批次的哈希值：

```
1. 对第一个键列: hashColT<false>(vector, hashFmgr, nrows, hashRes)
   → 对每行: hashRes[i] = hash_function(value[i])
   → NULL 值: hashRes[i] = 0

2. 对后续键列: hashColT<true>(vector, hashFmgr, nrows, hashRes)
   → 对每行:
     hashV = hashRes[i]
     hashV = (hashV << 1) | ((hashV & 0x80000000) ? 1 : 0)   ← 左旋 1 位
     hashV ^= hash_function(value[i])                          ← 异或新键的哈希值
     hashRes[i] = hashV
   → NULL 值: 保持不变

3. 二次哈希:
   if needSpill:
     hashRes[i] = hash_new_uint32(hashRes[i])   ← 溢出场景使用不同的二次哈希
   else:
     hashRes[i] = hash_uint32(hashRes[i])        ← 正常场景的二次哈希
```

**二次哈希的目的**：避免哈希值与数据分布键使用相同的哈希函数导致的系统性冲突。溢出场景使用 `hash_new_uint32` 以区分重分区层级。

### 18.2 简单键 cell 哈希：hashCellArray

**位置**：`vechashtable.h:670-690`

与 `hashBatch` 类似，但数据源是 hashCell 数组而非 VectorBatch：

```
hashCellT<false>(cell, keyIdx[0], hashFmgr, nrows, hashRes)  ← 第一个键
hashCellT<true>(cell, keyIdx[i], hashFmgr, nrows, hashRes)   ← 后续键
二次哈希
```

### 18.3 复杂键哈希：CalcComplicateHashVal

**位置**：`vechashjoin.cpp:3668-3731`

对于表达式类型的 Join 键，使用 `CalcComplicateHashVal()` 通过表达式引擎计算哈希值：

```
1. 设置表达式上下文:
   if inner: initEcontextBatch(NULL, NULL, batch, NULL)
   else:     initEcontextBatch(NULL, batch, NULL, NULL)

2. 遍历哈希键表达式列表:
   对每个表达式:
     results = VectorExprEngine(clause, econtext, ...)  ← 向量化表达式求值
     
     if 第一个键:
       m_cacheLoc[i] = FunctionCall1(&hash_functions[j], key)
       NULL 值: m_cacheLoc[i] = 0
     else:
       hash_val = m_cacheLoc[i]
       hash_val = (hash_val << 1) | ((hash_val & 0x80000000) ? 1 : 0)
       hash_val ^= FunctionCall1(&hash_functions[j], key)
       m_cacheLoc[i] = hash_val

3. 二次哈希:
   m_cacheLoc[i] = hash_uint32(m_cacheLoc[i])
```

---

## 19. 布隆过滤器下推：PushDownFilterIfNeed

**位置**：`vechashjoin.cpp:930-999`

在所有内表数据读取完毕后、构建哈希表前，尝试创建布隆过滤器用于优化探测侧的扫描：

### 前置条件

```
enable_bloom_filter 为 true
且 策略为 MEMORY_HASH（溢出场景不支持）
且 非复杂键
且 m_cache 非空
且 m_rows <= DEFAULT_ORC_BLOOM_FILTER_ENTRIES * 5（行数不超过阈值）
```

### 构建流程

```
遍历 bf_var_list（布隆过滤器变量列表）:
  找到变量对应的 Join 键索引 idx
  
  if 数据类型不满足布隆过滤器要求: 跳过
  
  创建布隆过滤器:
    filter = createBloomFilter(data_type, typmod, collid,
                               HASHJOIN_BLOOM_FILTER, entries, true)
  
  设置 LLVM 优化（如果可用且类型为 INT2/INT4/INT8）:
    filter->jitted_bf_addLong = m_runtime->jitted_hashjoin_bfaddLong
    filter->jitted_bf_incLong = m_runtime->jitted_hashjoin_bfincLong
  
  填充布隆过滤器:
    遍历 m_cache 链表:
      对每个 cell_arr:
        对每行 cell:
          if cell->m_val[idx] 非 NULL:
            filter->addDatum(cell->m_val[idx].val)
  
  注册到运行时:
    bf_array[pos] = filter
```

**关键点**：
- 布隆过滤器在 Build 结束后创建，利用已经在内存中的 cell 数据
- 只支持 MEMORY_HASH 策略（数据必须全部在内存中）
- 填充的是内表 Join 键值，用于在探测侧提前过滤不可能匹配的行
- 支持 INT2/INT4/INT8 类型的 LLVM 优化

---

## 20. 探测准备：PrepareProbe

**位置**：`vechashjoin.cpp:683-730`

`PrepareProbe()` 在 Build 完成后调用，根据策略进行最终准备并将状态切换到 HASH_PROBE：

### 20.1 MEMORY_HASH 策略

```
1. 设置探测状态:
   m_probeStatus = PROBE_FETCH
   m_probOpSource = new hashOpSource(outerPlanState)  ← 外表数据源

2. 创建内存数据源:
   source = new hashMemSource(m_cache)   ← 封装 m_cache 链表为 hashSource

3. 构建哈希表:
   切换到 m_hashContext 上下文
   if complicate_join_key:
     buildHashTable<true, false>(source, m_rows)
   else:
     buildHashTable<false, false>(source, m_rows)

4. 哈希表质量分析（可选）:
   if 启用了 ANLS_HASH_CONFLICT 或 资源跟踪:
     m_hashTbl->Profile(stats)
     记录冲突统计
   if 存在严重冲突:
     pgstat_add_warning_hash_conflict()
```

### 20.2 GRACE_HASH 策略

```
m_probeStatus = PROBE_PARTITION_FILE   ← 探测侧也需要先分区
m_probeIdx = 0                          ← 从第一个分区开始
```

### 20.3 状态转换

```
m_runtime->joinState = HASH_PROBE   ← 切换到探测阶段
```

---

## 21. 哈希表构建：buildHashTable

**位置**：`vechashjoin.cpp:798-874`

**模板签名**：`template <bool complicate_join_key, bool need_copy> void buildHashTable(hashSource* source, int64 rownum)`

在 `PrepareProbe` 中被调用，从数据源构建最终的哈希表：

### 21.1 哈希表创建

```
hash_size = Max(MIN_HASH_TABLE_SIZE, getPower2LessNum(Min(rownum, MAX_BUCKET_NUM)))
m_hashTbl = new vechashtable(hash_size)
  → 分配 hash_size 个桶指针，初始化为 NULL
mask = hash_size - 1
```

哈希表大小取 2 的幂（`getPower2LessNum`），范围在 `MIN_HASH_TABLE_SIZE` 到 `MAX_BUCKET_NUM` 之间。

### 21.2 数据插入循环

```
循环从数据源获取 cell 数组:
  cell_head = source->getCell()
  while cell_head != NULL:
    
    ① LLVM 快速路径（need_copy=false 且 jitted_buildHashTable 可用）:
       调用 LLVM 编译版本
       → 批量计算哈希值并插入桶
    
    ② LLVM 快速路径（need_copy=true 且 jitted_buildHashTable_NeedCopy 可用）:
       调用 LLVM 编译版本（带拷贝）
    
    ③ 普通路径:
       rows = cell_head->flag.m_rows
       
       if need_copy:
         copy_cell_array = palloc0(rows * m_cellSize)  ← 分配拷贝空间
       
       if !complicate_join_key:
         hashCellArray(cell_head, rows, m_keyIdx, m_cacheLoc, m_innerHashFuncs)
         → 批量计算哈希值存入 m_cacheLoc[]
       
       对每行 i:
         cell = GET_NTH_CELL(cell_head, i)
         
         if complicate_join_key:
           location = cell->m_val[m_cols].val & mask   ← 使用预存哈希值
         else:
           location = m_cacheLoc[i] & mask              ← 使用刚计算的哈希值
         
         last_cell = m_hashTbl->m_data[location]       ← 当前桶头
         
         if need_copy:
           copy_cell = GET_NTH_CELL(copy_cell_array, i)
           memcpy(copy_cell, cell, m_cellSize)          ← 拷贝 cell
           m_hashTbl->m_data[location] = copy_cell      ← 新 cell 成为桶头
           copy_cell->flag.m_next = last_cell            ← 链接旧桶头
         else:
           m_hashTbl->m_data[location] = cell            ← cell 直接成为桶头
           cell->flag.m_next = last_cell                  ← 链接旧桶头
    
    cell_head = source->getCell()   ← 获取下一个 cell 数组
```

### 21.3 NeedCopy 参数

| NeedCopy | 使用场景 | 说明 |
|---|---|---|
| false | MEMORY_HASH（PrepareProbe） | m_cache 中的 cell 在 m_hashContext 中，可以直接引用 |
| true | GRACE_HASH（preparePartition） | 文件源读取的 cell 在临时缓冲中，必须拷贝到持久内存 |

### 21.4 插入方式

采用**头插法**：新 cell 插入桶头，原桶头成为 m_next，形成单向链表。这种方式的优点是 O(1) 插入，无需遍历链表。

---

## 22. 关键数据结构

### 22.1 hashCell — 哈希表单元

```cpp
struct hashCell {
    union {
        hashCell* m_next;  // 哈希表阶段：指向同桶中下一个 cell
        int m_rows;        // 缓存阶段：该 cell 数组中的行数
    } flag;
    hashVal m_val[FLEXIBLE_ARRAY_MEMBER];  // 柔性数组，各列值
};

struct hashVal {
    ScalarValue val;   // 数据值
    uint8 flag;        // NULL 标记
};
```

**内存布局**（以 3 列 + 简单键为例）：
```
+----------+----------+----------+----------+
|  flag    | m_val[0] | m_val[1] | m_val[2] |
| (m_next  | val,flag | val,flag | val,flag |
|  或      |          |          |          |
|  m_rows) |          |          |          |
+----------+----------+----------+----------+
<--- m_cellSize = offsetof(hashCell, m_val) + 3 * sizeof(hashVal) --->
```

对于复杂键，末尾还有 `m_val[m_cols]` 存储哈希值。
对于 Right 类型 Join，`m_val[m_cols-1]` 用作匹配标记（build 时该列是额外添加的）。

### 22.2 vechashtable — 哈希表

```cpp
class vechashtable {
    int m_size;          // 桶数量（2 的幂）
    hashCell** m_data;   // 桶头指针数组
};
```

查找：`cell = m_data[hash_value & (m_size - 1)]`

### 22.3 m_cache — 内存缓存链表

```
m_cache: List* → [cell_arr_n] → [cell_arr_n-1] → ... → [cell_arr_1]
```

每个元素是一个 hashCell 数组（连续内存块），对应一个 VectorBatch 的数据。使用 `lcons` 头插法，最新的数据在链表头部。

### 22.4 hashSource 层次结构

```
hashSource (抽象基类)
  ├─ hashOpSource      — 从算子节点获取 VectorBatch（用于读取内/外表）
  ├─ hashMemSource     — 从 m_cache 链表获取 hashCell 数组（用于内存构建哈希表）
  ├─ hashFileSource    — 从临时文件获取数据（用于 GRACE_HASH）
  └─ hashSortSource    — 从排序结果获取数据
```

**hashMemSource::getCell()**：
```cpp
hashCell* getCell() {
    if (m_cell != NULL) {
        cell = lfirst(m_cell);
        m_cell = lnext(m_cell);
    }
    return cell;
}
```
按 m_cache 链表顺序逐个返回 cell 数组。

### 22.5 hashFileSource — 分区文件管理

```
hashFileSource:
  m_fileNum        ← 分区文件数量
  m_fileSize[]     ← 各分区文件的大小
  m_rownum[]       ← 各分区文件的行数
  m_spill_size     ← 累计溢出数据量（指向 instrument 统计字段）
  m_context        ← 文件 I/O 使用的内存上下文
```

写入方法：
- `writeBatch(batch, idx, key)` — 将批次的第 idx 行写入 `key & (m_fileNum - 1)` 号文件
- `writeBatchWithHashval(batch, idx, key)` — 同上，额外写入哈希值
- `writeCell(cell, key)` — 将单个 cell 写入对应分区文件

### 22.6 VecHashJoinState — 算子运行时状态

```cpp
struct VecHashJoinState : public HashJoinState {
    int joinState;                      // HASH_BUILD=0, HASH_PROBE=1, HASH_END=2
    void* hashTbl;                      // → HashJoinTbl 或 SonicHashJoin
    FmgrInfo* eqfunctions;              // 等值比较函数数组
    
    // LLVM JIT 编译函数指针
    vecqual_func jitted_joinqual;
    vecqual_func jitted_hashclause;
    char* jitted_innerjoin;
    char* jitted_matchkey;
    char* jitted_buildHashTable;        // buildHashTable 的 JIT 版本（无拷贝）
    char* jitted_buildHashTable_NeedCopy;  // buildHashTable 的 JIT 版本（有拷贝）
    char* jitted_probeHashTable;
    int enable_fast_keyMatch;
    
    // 布隆过滤器
    BloomFilterRuntime bf_runtime;
    char* jitted_hashjoin_bfaddLong;
    char* jitted_hashjoin_bfincLong;
};
```

---

## 23. LLVM 优化路径

Build 阶段支持以下 LLVM JIT 编译优化：

| JIT 字段 | 优化路径 | 说明 |
|---|---|---|
| `jitted_buildHashTable` | buildHashTable（NeedCopy=false） | 内存策略下的哈希表构建 |
| `jitted_buildHashTable_NeedCopy` | buildHashTable（NeedCopy=true） | GRACE 策略下的哈希表构建（需拷贝） |
| `jitted_hashjoin_bfaddLong` | PushDownFilterIfNeed | 布隆过滤器元素添加（INT 类型） |
| `jitted_hashjoin_bfincLong` | PushDownFilterIfNeed | 布隆过滤器包含检查（INT 类型） |

LLVM 编译版本将哈希计算和桶插入操作内联，消除函数调用开销，对于大表构建有显著性能提升。当 JIT 可用时，`instrument->isLlvmOpt` 会被设为 true，在 EXPLAIN ANALYZE 中显示。

---

## 24. 完整调用关系图

```
ExecVecHashJoin(node)
│
└─ [HASH_BUILD]
    │
    ├─ 首次调用: new HashJoinTbl(node)
    │   │
    │   ├─ initMemoryControl()
    │   │   └─ 计算 m_totalMem = operatorMemKB * 1024 + RackMemory
    │   │
    │   ├─ 创建内存上下文
    │   │   ├─ m_hashContext (STACK_CONTEXT, 带内存限制)
    │   │   └─ m_tmpContext  (临时计算)
    │   │
    │   ├─ 分析 Join 键
    │   │   ├─ 遍历 hj_InnerHashKeys → m_keyIdx[], m_simpletype[]
    │   │   ├─ 遍历 hj_OuterHashKeys → m_outKeyIdx[]
    │   │   └─ 判断 m_complicateJoinKey
    │   │
    │   ├─ 设置哈希函数
    │   │   ├─ m_innerHashFuncs[] (build 侧)
    │   │   └─ m_outerHashFuncs[] (probe 侧)
    │   │
    │   ├─ 初始化批次结构
    │   │   ├─ m_innerBatch, m_outerBatch
    │   │   ├─ m_inQualBatch, m_outQualBatch (如果有 joinqual)
    │   │   ├─ m_complicate_innerBatch/outerBatch (如果是复杂键)
    │   │   └─ m_result
    │   │
    │   ├─ 计算 m_cellSize
    │   │
    │   ├─ SetJoinType()
    │   │   └─ JOIN_INNER → HASH_JOIN_INNER, ...
    │   │
    │   └─ bindingFp<CK>()
    │       ├─ m_funBuild[0] = SaveToMemory<CK, simple>
    │       ├─ m_funBuild[1] = SaveToDisk<CK, true>
    │       ├─ m_probeFun[0/1] = probeMemory / probeGrace
    │       ├─ m_joinFunArray[] = { innerJoinT, leftJoinT, ... }
    │       ├─ m_joinFun = m_joinFunArray[...]
    │       └─ m_matchKeyFunction[] = matchKey<...>
    │
    └─ HashJoinTbl::Build()
        │
        ├─ 内表数据读取循环 ─────────────────────────────┐
        │   batch = VectorEngine(innerPlanState)          │
        │   RuntimeBinding(m_funBuild, m_strategy)(batch) │
        │   │                                              │
        │   ├─ [MEMORY_HASH] SaveToMemory<CK, simple>()  │
        │   │   ├─ HasEnoughMem(rows)                     │
        │   │   │   ├─ true: 继续内存存储                  │
        │   │   │   └─ false: 触发溢出 ──────┐            │
        │   │   │       ├─ calcSpillFile()    │            │
        │   │   │       ├─ m_strategy = GRACE │            │
        │   │   │       ├─ initFile()         │            │
        │   │   │       ├─ flushToDisk<CK>()  │            │
        │   │   │       │   └─ 遍历 m_cache   │            │
        │   │   │       │       hashCellArray()│            │
        │   │   │       │       writeCell()    │            │
        │   │   │       ├─ SaveToDisk(batch)   │            │
        │   │   │       └─ MemoryContextReset()│            │
        │   │   │                              │            │
        │   │   ├─ palloc0(rows * m_cellSize)              │
        │   │   ├─ [CK] CalcComplicateHashVal()            │
        │   │   ├─ 逐列填充 cell 值                        │
        │   │   │   ├─ 简单类型: 直接赋值                   │
        │   │   │   └─ encoded: addVariable()               │
        │   │   ├─ [CK] 存储哈希值到 m_val[m_cols]          │
        │   │   └─ m_cache = lcons(cell_arr, m_cache)       │
        │   │                                              │
        │   └─ [GRACE_HASH] SaveToDisk<CK, true>()       │
        │       ├─ [CK] CalcComplicateHashVal()            │
        │       ├─ [!CK] hashBatch()                       │
        │       └─ 逐行写入分区文件                         │
        │           writeBatch() / writeBatchWithHashval() │
        │                                                  │
        │   ← 循环直到内表耗尽 ────────────────────────────┘
        │
        ├─ PushDownFilterIfNeed()
        │   └─ [MEMORY_HASH 且满足条件]
        │       遍历布隆过滤器变量列表:
        │         createBloomFilter()
        │         遍历 m_cache:
        │           filter->addDatum(cell->m_val[idx].val)
        │         bf_array[pos] = filter
        │
        └─ PrepareProbe()
            │
            ├─ [MEMORY_HASH]
            │   ├─ m_probeStatus = PROBE_FETCH
            │   ├─ m_probOpSource = new hashOpSource(outerPlan)
            │   ├─ source = new hashMemSource(m_cache)
            │   └─ buildHashTable<CK, false>(source, m_rows)
            │       ├─ m_hashTbl = new vechashtable(hash_size)
            │       └─ 循环 source->getCell():
            │           ├─ [LLVM] jitted_buildHashTable()
            │           └─ [普通] 逐行:
            │               ├─ 计算桶号: hash & mask
            │               └─ 头插法: cell → m_data[location]
            │
            ├─ [GRACE_HASH]
            │   ├─ m_probeStatus = PROBE_PARTITION_FILE
            │   └─ m_probeIdx = 0
            │
            └─ m_runtime->joinState = HASH_PROBE  ← 转入探测阶段
```

---

## 总结

VecHashJoin Build 阶段的设计体现了以下核心思想：

1. **两阶段构建**：先以 hashCell 数组形式在 m_cache 链表中积累数据，所有数据读取完毕后才真正构建哈希表。这种设计使得：
   - 可以精确知道总行数来确定最优哈希表大小
   - 可以在构建哈希表前创建布隆过滤器
   - 支持溢出时将缓存数据批量刷盘

2. **自适应内存管理**：三级内存检查（算子级、系统级、Rack级）+ 内存自动扩展机制，在内存充足时保持内存哈希的高性能，内存不足时平滑切换到磁盘溢出

3. **向量化批处理**：所有数据操作以 VectorBatch 为单位，哈希计算、值拷贝等均以向量化方式批量执行，充分利用 CPU 缓存和流水线

4. **模板特化**：通过 `complicate_join_key`、`simple`、`need_copy` 等模板参数在编译期消除运行时分支，生成针对不同场景的特化代码

5. **函数指针零开销分发**：初始化时一次性绑定所有函数指针（Build 函数、Probe 函数、Join 函数、键匹配函数），运行时通过数组索引直接调用，无需条件判断

6. **LLVM JIT 加速**：`buildHashTable` 的核心循环支持 LLVM 编译优化，将哈希计算和桶插入内联到一个编译单元中

7. **布隆过滤器下推**：利用 Build 完成后内存中的数据构建布隆过滤器，下推到探测侧的扫描算子中，在数据源头过滤不可能匹配的行
