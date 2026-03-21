# VecHashJoin Probe 阶段全流程分析

本文档对 openGauss 向量化哈希连接（VecHashJoin）的 **Probe（探测）阶段** 进行全面深入的源码级分析，涵盖从顶层入口到各类 Join 实现的完整调用链路。

> **源码位置**
> - 实现文件：`src/gausskernel/runtime/vecexecutor/vecnode/vechashjoin.cpp`
> - 头文件：`src/include/vecexecutor/vechashjoin.h`
> - 哈希表定义：`src/include/vecexecutor/vechashtable.h`
> - 节点状态：`src/include/vecexecutor/vecnodes.h`

---

## 目录

1. [整体架构概览](#1-整体架构概览)
2. [顶层入口：ExecVecHashJoin](#2-顶层入口execvechashjoin)
3. [策略分发：Probe 函数指针](#3-策略分发probe-函数指针)
4. [内存探测：probeMemory](#4-内存探测probememory)
5. [核心状态机：probeHashTable](#5-核心状态机probehashtable)
6. [哈希值计算与桶定位](#6-哈希值计算与桶定位)
7. [键匹配机制](#7-键匹配机制)
8. [Join 类型函数分发](#8-join-类型函数分发)
9. [各 Join 类型详细流程](#9-各-join-类型详细流程)
   - [9.1 Inner Join](#91-inner-join-innerjoint)
   - [9.2 Left Join](#92-left-join-leftjoint)
   - [9.3 Left Join With Qual](#93-left-join-with-qual-leftjoinwithqualt)
   - [9.4 Right Join](#94-right-join-rightjoint)
   - [9.5 Semi Join](#95-semi-join-semijoint)
   - [9.6 Anti Join](#96-anti-join-antijoint)
   - [9.7 Right Semi Join](#97-right-semi-join-rightsemijoint)
   - [9.8 Right Anti Join](#98-right-anti-join-rightantijoint)
10. [结果构建：buildResult](#10-结果构建buildresult)
11. [Qual 检查：checkQual](#11-qual-检查checkqual)
12. [终结处理：endJoin](#12-终结处理endjoin)
13. [GRACE 哈希探测：probeGrace](#13-grace-哈希探测probegrace)
14. [分区探测：probePartition](#14-分区探测probepartition)
15. [分区准备：preparePartition](#15-分区准备preparepartition)
16. [重分区：RePartitionFileSource](#16-重分区repartitionfilesource)
17. [关键数据结构](#17-关键数据结构)
18. [LLVM 优化路径](#18-llvm-优化路径)
19. [完整调用关系图](#19-完整调用关系图)

---

## 1. 整体架构概览

VecHashJoin 的 Probe 阶段是在 Build 阶段完成哈希表构建之后执行的。其整体架构采用 **多层状态机 + 函数指针分发** 的设计模式：

```
ExecVecHashJoin()                    ← 顶层执行入口
  └─ HashJoinTbl::Probe()            ← 策略分发器
       ├─ probeMemory()              ← 内存策略（哈希表在内存中）
       │    └─ probeHashTable(m_probOpSource)
       │         ├─ PROBE_FETCH      ← 获取外表批次、计算哈希、定位桶
       │         ├─ PROBE_DATA       ← 调用 Join 函数匹配并输出
       │         └─ PROBE_FINAL      ← endJoin() 处理右侧未匹配行
       │
       └─ probeGrace()               ← GRACE 策略（数据溢出到磁盘）
            ├─ PROBE_PARTITION_FILE  ← 将外表分区写入临时文件
            ├─ PROBE_PREPARE_PAIR    ← 为当前分区对构建哈希表
            └─ PROBE_FETCH/DATA/FINAL← 复用 probeHashTable 逐分区探测
```

**核心设计特点：**
- **向量化处理**：以 VectorBatch（默认最多 1000 行）为单位批量处理
- **模板特化**：Join 函数按 `complicate_join_key` 和 `simple_key` 两个布尔参数特化，消除运行时分支
- **函数指针分发**：初始化时根据 Join 类型和是否有 joinqual 选定具体函数指针，探测时零开销分发
- **状态保存与续传**：通过 `JoinStateLog` 保存中断点，允许在批次满时返回结果并恢复继续处理
- **LLVM JIT 优化**：核心热路径可选用 LLVM 编译执行

---

## 2. 顶层入口：ExecVecHashJoin

**位置**：`vechashjoin.cpp:235-315`

```
ExecVecHashJoin(VecHashJoinState* node)
```

该函数是向量化执行器调用 VecHashJoin 算子的入口，使用 `node->joinState` 状态机控制 Build 和 Probe 两个阶段：

### 2.1 HASH_BUILD 阶段

- 创建 `HashJoinTbl` 对象（或 `SonicHashJoin`）
- 调用 `Build()` 方法从内表（innerPlanState）构建哈希表
- 提前释放内表子节点资源（`ExecEarlyFree`）
- **短路优化**：对于 INNER/SEMI/RIGHT_SEMI/RIGHT/RIGHT_ANTI 类型，如果哈希表行数为 0，直接返回 NULL（无需读取外表）

### 2.2 HASH_PROBE 阶段

- 调用 `HashJoinTbl::Probe()` 获取结果批次
- 记录探测耗时（`m_probe_time`）用于性能统计
- 当 `Probe()` 返回 NULL 时表示探测完成：
  - 将探测时间写入 instrument 信息
  - 提前释放外表子节点资源
- 返回结果批次给上层算子

---

## 3. 策略分发：Probe 函数指针

**位置**：`vechashjoin.cpp:1242-1245`

```cpp
VectorBatch* HashJoinTbl::Probe()
{
    return RuntimeBinding(m_probeFun, m_strategy)();
}
```

`Probe()` 方法通过 `RuntimeBinding` 宏根据 `m_strategy` 的值分发到不同的探测实现：

| m_strategy 值 | 策略 | 目标函数 | 说明 |
|---|---|---|---|
| 0 | MEMORY_HASH | `probeMemory()` | 哈希表完全在内存中 |
| 1 | GRACE_HASH | `probeGrace()` | 数据溢出到磁盘，分区处理 |

`m_probeFun` 数组在初始化阶段的 `bindingFp()` 方法中设置：
```cpp
m_probeFun[0] = &HashJoinTbl::probeMemory;
m_probeFun[1] = &HashJoinTbl::probeGrace;
```

---

## 4. 内存探测：probeMemory

**位置**：`vechashjoin.cpp:1386-1389`

```cpp
VectorBatch* HashJoinTbl::probeMemory()
{
    return probeHashTable(m_probOpSource);
}
```

`probeMemory()` 是一个轻量包装，将 `m_probOpSource`（外表数据源，类型为 `hashOpSource`）传给核心探测函数 `probeHashTable()`。

`hashOpSource` 是 `hashSource` 的子类，其 `getBatch()` 方法直接调用 `VectorEngine(m_op)` 从外表算子获取下一个批次。

---

## 5. 核心状态机：probeHashTable

**位置**：`vechashjoin.cpp:1739-1806`

这是 Probe 阶段最核心的函数，实现了一个三状态状态机：

### 状态定义

```cpp
#define PROBE_FETCH          0   // 获取外表下一批次
#define PROBE_PARTITION_FILE 1   // 分区外表数据到临时文件（GRACE 专用）
#define PROBE_DATA           2   // 执行 Join 匹配
#define PROBE_FINAL          3   // 终结处理（右侧未匹配行）
#define PROBE_PREPARE_PAIR   4   // 准备分区对（GRACE 专用）
```

### PROBE_FETCH 状态详细流程

1. **重置上下文**：如果存在 `m_probeFileSource`，重置其内存上下文
2. **获取批次**：调用 `probSource->getBatch()` 获取外表的下一个 VectorBatch
3. **空批次处理**：如果批次为 NULL，转入 `PROBE_FINAL` 状态
4. **LLVM 快速路径**：如果 `jitted_probeHashTable` 已编译，直接调用 LLVM 编译版本完成哈希计算和桶定位
5. **普通路径**：
   - 计算哈希值：
     - 复杂键（`m_complicateJoinKey=true`）且 GRACE 模式：哈希值已在批次的最后一列中
     - 复杂键，内存模式：调用 `CalcComplicateHashVal()` 计算
     - 简单键：调用 `hashBatch()` 计算
   - 对批次中的每一行：
     - `m_cacheLoc[i] = hash_value & (m_hashTbl->m_size - 1)` — 计算桶号
     - `m_cellCache[i] = m_hashTbl->m_data[m_cacheLoc[i]]` — 定位桶头
     - `m_match[i] = false` — 初始化匹配标记为"未匹配"
     - `m_keyMatch[i] = true` — 初始化键匹配标记为"待检查"
6. **状态转移**：设置 `m_probeStatus = PROBE_DATA`

### PROBE_DATA 状态

调用 Join 函数（通过函数指针 `m_joinFun`）执行实际匹配：
```cpp
res_batch = (this->*m_joinFun)(m_outRawBatch);
```

如果返回非空批次，直接返回给调用者。如果为空，说明当前批次处理完毕，Join 函数内部已将状态重置为 `PROBE_FETCH`。

### PROBE_FINAL 状态

调用 `endJoin()` 处理右连接类型中内表未匹配的行。

---

## 6. 哈希值计算与桶定位

### 6.1 简单键哈希：hashBatch

**位置**：`vechashtable.h:646-668`

对于非复杂表达式的 Join 键，使用 `hashBatch()` 函数计算哈希值：

1. 对第一个键列调用 `hashColT<false>()` 生成初始哈希值
2. 对后续键列调用 `hashColT<true>()` 将哈希值与已有值合并（旋转+异或）
3. 最后通过 `hash_uint32()` 进行二次哈希，避免与分布键使用相同的哈希函数导致的数据倾斜

### 6.2 复杂键哈希：CalcComplicateHashVal

**位置**：`vechashjoin.cpp:3668-3731`

对于复杂表达式的 Join 键，使用 `CalcComplicateHashVal()` 函数：

1. 设置表达式上下文（外表或内表方向）
2. 遍历哈希键表达式列表（`hj_OuterHashKeys`）
3. 对每个键调用 `VectorExprEngine()` 计算表达式值
4. 第一个键：直接通过 `FunctionCall1(&hash_functions[j], key)` 得到哈希值
5. 后续键：将当前哈希值左旋1位后与新键的哈希值异或
6. 最终通过 `hash_uint32()` 进行二次哈希

### 6.3 桶定位

```cpp
m_cacheLoc[i] = m_cacheLoc[i] & mask;  // mask = m_hashTbl->m_size - 1
m_cellCache[i] = m_hashTbl->m_data[m_cacheLoc[i]];  // 桶头指针
```

哈希表大小始终是 2 的幂，因此用按位与代替取模运算。`m_cellCache[i]` 指向该桶的链表头节点。

---

## 7. 键匹配机制

### 7.1 简单键匹配：matchKey

**位置**：`vechashjoin.cpp:3634-3666`

模板函数 `matchKey<innerType, outerType, simpleType, nulleqnull>` 对批次中的每一行进行键值比较：

```
对于每行 i:
  if m_keyMatch[i] == true 且 m_cellCache[i] 非空:
    if 内表值非 NULL 且外表值非 NULL:
      if simpleType:
        直接比较: (innerType)cell_value == (outerType)outer_value
      else:
        调用等值比较函数: cmp_fun(&fc_info)
    else if nulleqnull 且两侧都为 NULL:
      m_keyMatch[i] = true  (NULL = NULL 语义)
    else:
      m_keyMatch[i] = false
```

**类型特化组合**：
- 简单类型（int4/int8 等）：直接用 C++ 的 `==` 运算符，性能最高
- 非简单类型：调用 `m_eqfunctions[key_num]` 注册的比较函数
- `nulleqnull` 模式：在 null-safe 等值连接时使用

多个 Join 键时，对每个键依次调用 `matchKey()`，`m_keyMatch[i]` 的结果会被逐步"与"缩窄。

### 7.2 复杂键匹配：matchComplicateKey

**位置**：`vechashjoin.cpp:3517-3616`

当 Join 键是表达式而非简单列引用时，使用 `matchComplicateKey()`：

1. 将 `m_cellCache[i]` 中的内表值和批次中的外表值分别拷贝到 `m_complicate_innerBatch` 和 `m_complicate_outerBatch`
2. 调用 `VectorExprEngine()` 或 LLVM 编译版本执行 `hashclauses` 表达式
3. 处理 `nulleqqual` 特殊情况（null-safe 连接）
4. 根据表达式结果更新 `m_keyMatch[]` 数组

---

## 8. Join 类型函数分发

### 8.1 Join 类型枚举

```cpp
typedef enum {
    HASH_JOIN_INNER = 0,          // 内连接
    HASH_JOIN_LEFT,               // 左外连接
    HASH_JOIN_RIGHT,              // 右外连接
    HASH_JOIN_SEMI,               // 左半连接
    HASH_JOIN_ANTI,               // 左反连接
    HASH_JOIN_RIGHT_SEMI,         // 右半连接
    HASH_JOIN_RIGHT_ANTI,         // 右反连接
    HASH_JOIN_LEFT_ANTI_FULL,     // 左反全连接
    HASH_JOIN_RIGHT_ANTI_FULL,    // 右反全连接
} hashJoinType;
```

### 8.2 函数指针表初始化

**位置**：`vechashjoin.cpp:1247-1331`（`bindingFp()` 模板函数）

`InitJoinTemplate` 宏将各类 Join 函数按固定顺序填入 `m_joinFunArray[]`：

| 数组索引 | 函数 |
|---|---|
| 0 | `innerJoinT` |
| 1 | `leftJoinT` |
| 2 | `leftJoinWithQualT` |
| 3 | `rightJoinT` |
| 4 | `rightJoinWithQualT` |
| 5 | `semiJoinT` |
| 6 | `semiJoinWithQualT` |
| 7 | `antiJoinT` |
| 8 | `antiJoinWithQualT` |
| 9 | `rightSemiJoinT` |
| 10 | `rightSemiJoinWithQualT` |
| 11 | `rightAntiJoinT` |
| 12 | `rightAntiJoinWithQualT` |
| 13 | `antiJoinT` (LEFT_ANTI_FULL 无 qual) |
| 14 | `antiJoinWithQualT` (LEFT_ANTI_FULL 有 qual) |
| 15 | `rightAntiJoinT` (RIGHT_ANTI_FULL 无 qual) |
| 16 | `rightAntiJoinWithQualT` (RIGHT_ANTI_FULL 有 qual) |

**分发逻辑**：
```
array_idx = 有 joinqual ? (2 * m_joinType) + 1 : (2 * m_joinType)
m_joinFun = m_joinFunArray[base_idx + idx_array[array_idx]]
```

`base_idx` 根据 `m_keySimple`（键类型是否简单）确定偏移量，用于选择 `<complicate_join_key, true>` 或 `<complicate_join_key, false>` 版本的模板实例。

---

## 9. 各 Join 类型详细流程

### 9.1 Inner Join (innerJoinT)

**位置**：`vechashjoin.cpp:2158-2252`

**语义**：只输出两侧都匹配的行。

**流程**：

```
1. LLVM 快速路径检查：
   if (jitted_innerjoin 已编译):
     调用 LLVM 编译版本，直接返回结果
     
2. 普通路径 — 外层 while(m_doProbeData) 循环：
   a. 键匹配：
      - 复杂键 → matchComplicateKey(batch)
      - 简单键 → 对每个键调用 matchKey()
      
   b. 遍历批次中的每一行 (row_idx):
      if m_keyMatch[row_idx] == true:
        ① 从 m_cellCache[row_idx]->m_val 获取内表值
        ② 将内表值写入 m_innerBatch[result_row]
        ③ 将外表值写入 m_outerBatch[result_row]
        ④ result_row++
        
      if result_row == BatchMaxSize:
        保存断点(lastBuildIdx, restore=true)
        返回 buildResult()
        
   c. 链表推进（处理同一桶中的下一个 cell）：
      对每行: m_cellCache[row_idx] = m_cellCache[row_idx]->flag.m_next
      if 任何行还有后续 cell: m_doProbeData = true（继续循环）
      
3. 循环结束后：
   m_probeStatus = PROBE_FETCH（准备获取下一个批次）
   返回 buildResult() 或 NULL
```

**关键点**：
- Inner Join 不需要匹配标记（不输出未匹配行）
- 支持 LLVM JIT 优化（热路径）
- `buildResult()` 的第三个参数 `check_qual=true`，会检查 joinqual

### 9.2 Left Join (leftJoinT)

**位置**：`vechashjoin.cpp:1895-1998`

**语义**：输出所有外表行。匹配的外表行与匹配的内表行配对输出；未匹配的外表行与 NULL 内表行配对输出。

**流程**：

```
1. 外层 while(m_doProbeData) 循环 — 处理匹配行：
   a. 键匹配 + 恢复断点处理
   b. 遍历每行:
      if m_keyMatch[row_idx] == true:
        m_match[row_idx] = true  ← 标记"已找到匹配"
        输出内表值 + 外表值到结果
        result_row++
      if result_row == BatchMaxSize: 保存断点并返回
   c. 链表推进（同 Inner Join）
   
2. 循环结束后 — 处理未匹配行：
   遍历每行:
     if m_match[row_idx] == false:
       输出外表值 + NULL 内表值到结果
       result_row++
     if result_row == BatchMaxSize: 保存断点并返回
     
3. 转入 PROBE_FETCH 状态，返回结果
```

**关键点**：
- `m_match[]` 数组跟踪每个外表行是否找到了至少一个匹配
- 同一外表行可能匹配多个内表行（多对多关系），每个匹配都会输出一行
- 未匹配行使用 `SET_NULL()` 宏将内表侧的 flag 设为 NULL
- `buildResult()` 的 `check_qual=false`（无额外 joinqual 检查）

### 9.3 Left Join With Qual (leftJoinWithQualT)

**位置**：`vechashjoin.cpp:2001-2155`

**语义**：与 Left Join 相同，但有额外的 joinqual 条件需要检查。

**流程差异**（相对于 leftJoinT）：

```
1. 匹配阶段增加 joinqual 检查：
   a. 键匹配后，不直接输出结果
   b. 将匹配行写入 m_inQualBatch/m_outQualBatch（临时检查批次）
   c. 调用 checkQual() 执行 joinqual 表达式
   d. 根据检查结果更新：
      - 通过检查 → m_match[org_idx] = true（真正匹配）
      - 未通过 → m_keyMatch[org_idx] = false（虽然键匹配但条件不满足）
   e. 只将 m_keyMatch 仍为 true 的行输出到结果

2. 未匹配行处理与 leftJoinT 相同
```

**关键点**：
- 使用 `m_reCheckCell[]` 数组记录每个候选行对应的原始行索引
- `checkQual()` 可利用 LLVM 编译的 `jitted_joinqual`

### 9.4 Right Join (rightJoinT)

**位置**：`vechashjoin.cpp:3205-3279`

**语义**：输出所有内表行。匹配的内表行与外表行配对输出；未匹配的内表行在 `endJoin()` 中与 NULL 外表行配对输出。

**流程**：

```
1. 外层 while(m_doProbeData) 循环：
   a. 键匹配
   b. 遍历每行:
      if m_keyMatch[row_idx] == true:
        ① 设置匹配标记: m_cellCache[row_idx]->m_val[m_cols-1].val = 1
        ② 输出内表值 + 外表值
        ③ result_row++
      if result_row == BatchMaxSize: 保存断点并返回
   c. 链表推进
   
2. 转入 PROBE_FETCH 状态
3. 所有外表批次处理完成后，进入 PROBE_FINAL → endJoin()
```

**关键点**：
- 匹配标记存储在哈希 cell 的最后一列（`m_val[m_cols-1].val`）中，利用哈希表构建时预留的额外列
- `endJoin()` 扫描整个哈希表，输出 `m_val[m_cols-1].val == 0`（未匹配）的内表行

### 9.5 Semi Join (semiJoinT)

**位置**：`vechashjoin.cpp:2255-2316`

**语义**：对每个外表行，只要在内表中找到至少一个匹配，就输出该外表行（只输出一次）。

**流程**：

```
while(m_doProbeData):
  键匹配
  m_doProbeData = false（假设本轮结束）
  
  遍历每行:
    if m_keyMatch[row_idx] == true:
      输出内表值 + 外表值（虽然通常只需外表值，但内表值在某些场景有用）
      result_row++
      m_cellCache[row_idx] = NULL  ← 关键：标记为"已匹配，无需继续"
    else:
      推进到下一个 cell
      if 有后续 cell: m_doProbeData = true
      重置 m_keyMatch[row_idx] = true
      
结果行不会超过 BatchMaxSize（因为每个外表行最多匹配一次）
```

**关键点**：
- 一旦找到匹配，将 `m_cellCache[row_idx] = NULL` 停止该行的后续匹配
- 与 Anti Join 互补：Semi 输出有匹配的行，Anti 输出无匹配的行

### 9.6 Anti Join (antiJoinT)

**位置**：`vechashjoin.cpp:2832-2891`

**语义**：输出在内表中没有任何匹配的外表行。

**流程**：

```
1. while(m_doProbeData) 循环 — 标记匹配行：
   键匹配
   m_doProbeData = false
   
   遍历每行:
     if m_keyMatch[row_idx] == true:
       m_match[row_idx] = true  ← 标记"已匹配"
       m_cellCache[row_idx] = NULL  ← 停止进一步匹配
     else:
       推进到下一个 cell
       if 有后续 cell: m_doProbeData = true
       重置 m_keyMatch[row_idx] = true

2. 循环结束后 — 输出未匹配行：
   遍历每行:
     if m_match[row_idx] == false:
       输出外表值 + NULL 内表值
       result_row++
       
3. 返回结果
```

**关键点**：
- 必须遍历完该行在桶中的所有 cell 后才能确定"无匹配"
- 内表侧全部填 NULL

### 9.7 Right Semi Join (rightSemiJoinT)

**位置**：`vechashjoin.cpp:2463-2625`

**语义**：输出在外表中找到至少一个匹配的内表行（每个内表行只输出一次）。

**流程**：

```
1. while(m_doProbeData) 循环：
   a. 键匹配 + 断点恢复
   b. 遍历每行:
      if m_keyMatch[row_idx] 且 m_cellCache[row_idx]->m_val[k].val == 0:
        ① 设置标记: val[k].val = 1（标记为"已匹配"）
        ② 输出内表值 + 外表值
        ③ result_row++
      if result_row == BatchMaxSize: 保存断点并返回
      
   c. 链表推进 + 从哈希表中删除已匹配 cell：
      - 非桶头的已匹配 cell：直接从链表中摘除
      - 桶头的已匹配 cell：标记 m_match[row_idx] = true，延迟删除
      
   d. 删除已匹配的桶头：
      遍历标记为 m_match 的行：
        p_data[i] = p_data[i]->flag.m_next（桶头指向下一个）

2. 返回结果
```

**关键点**：
- 匹配后从哈希表中物理删除 cell，避免重复输出
- 使用 `cellPoint[]` 辅助数组跟踪前一个未匹配 cell（用于链表摘除操作）
- `m_val[k].val` 的双重检查防止多个外表行同时指向同一 cell 时的重复输出
- 删除的 cell 内存不立即释放（仍在 HashContext 中），等 Context 整体释放

### 9.8 Right Anti Join (rightAntiJoinT)

**位置**：`vechashjoin.cpp:3024-3085`

**语义**：输出在外表中没有任何匹配的内表行。结果在 `endJoin()` 中统一输出。

**流程**：

```
1. while(m_doProbeData) 循环：
   a. 键匹配
   b. 标记匹配 cell: m_cellCache[row_idx]->m_val[k].val = 1
   c. 从哈希表中删除已匹配的 cell（与 rightSemiJoinT 相同的链表操作）
   
2. 设置 m_probeStatus = PROBE_FETCH
3. 返回 NULL（不在此处输出结果）

实际结果由 endJoin() 输出：扫描哈希表中 m_val[k].val == 0 的 cell
```

**关键点**：
- 在探测阶段只做标记和删除，不输出任何行
- `endJoin()` 遍历哈希表中剩余的（未被匹配的）cell 输出结果
- 删除已匹配 cell 是优化手段，减少 `endJoin()` 需要扫描的数据量

---

## 10. 结果构建：buildResult

**位置**：`vechashjoin.cpp:3437-3515`

`buildResult()` 将 `m_innerBatch` 和 `m_outerBatch` 中的中间结果转化为最终输出批次：

```
1. 重置结果批次和表达式上下文
2. 设置 econtext 的内外表引用

3. if check_qual 且有 joinqual:
   - 使用 LLVM 编译版本或 ExecVecQual() 执行 joinqual
   - 不满足条件的行被标记为 false
   - 如果全部不满足，返回 NULL

4. if 有 ps.qual（一般过滤条件）:
   - 执行 ExecVecQual() 过滤
   - 不满足的行被标记为 false

5. if 有 qual 被执行过:
   - 调用 PackT<true, false>() 紧凑化批次（去除不满足条件的行）

6. if 有投影（ps_ProjInfo）:
   - 调用 ExecVecProject() 执行列投影
   else:
   - 直接使用当前结果

7. 重置 m_innerBatch 和 m_outerBatch
8. 返回最终结果批次
```

---

## 11. Qual 检查：checkQual

**位置**：`vechashjoin.cpp:3400-3435`

`checkQual()` 专门用于 `leftJoinWithQualT` 等带 joinqual 的 Join 类型中，对候选匹配行进行条件检查：

```
1. 设置 econtext，将 in_batch/out_batch 作为内外表引用
2. 使用 LLVM 编译版本或 ExecVecQual() 执行 joinqual
3. 重置 in_batch 和 out_batch
4. 返回 econtext->ecxt_scanbatch->m_sel（选择向量）
```

返回的布尔数组指示每个候选行是否通过了 joinqual 检查。

---

## 12. 终结处理：endJoin

**位置**：`vechashjoin.cpp:1808-1892`

`endJoin()` 在所有外表数据处理完毕后（PROBE_FINAL 状态）调用，用于 Right Join、Right Anti Join 和 Right Anti Full Join 类型：

```
if m_doProbeData == false: 返回 NULL（已处理完毕）

if m_joinType 是 RIGHT/RIGHT_ANTI/RIGHT_ANTI_FULL:
  k = m_cols - 1（匹配标记所在列）
  
  恢复上次断点（如果有）
  
  遍历哈希表所有桶:
    遍历桶中每个 cell:
      if cell->m_val[k].val == 0（未匹配）:
        ① 外表侧全部填 NULL
        ② 内表侧填入 cell 中的值
        ③ result_row++
        
        if result_row == BatchMaxSize:
          保存断点（桶索引 + 下一个 cell）
          返回 buildResult()
          
  标记 m_doProbeData = false
  返回最后的结果（可能不满一批）
```

**关键点**：
- 支持跨批次续传（通过 `JoinStateLog` 保存桶索引和 cell 指针）
- 只处理 Right 类型的 Join（Left 类型在各 joinT 函数中直接处理未匹配行）

---

## 13. GRACE 哈希探测：probeGrace

**位置**：`vechashjoin.cpp:1692-1737`

当内存不足以容纳整个哈希表时，使用 GRACE 哈希策略。`probeGrace()` 实现了一个五状态状态机：

```
while(true):
  switch(m_probeStatus):
    case PROBE_PARTITION_FILE:
      将整个外表数据按哈希值分区写入临时文件
      m_probeStatus = PROBE_PREPARE_PAIR
      
    case PROBE_PREPARE_PAIR:
      为当前分区对（m_probeIdx）准备哈希表
      调用 preparePartition()
      if 所有分区已处理完: 返回 NULL
      m_probeStatus = PROBE_FETCH
      
    case PROBE_FETCH / PROBE_DATA / PROBE_FINAL:
      调用 probeHashTable(m_probeFileSource) 探测当前分区
      if 有结果: 返回结果
      else:
        关闭当前分区文件
        m_probeIdx++（移到下一个分区）
        m_probeStatus = PROBE_PREPARE_PAIR
```

**GRACE 探测的整体流程**：
1. 一次性将外表所有数据分区写入磁盘
2. 逐对处理分区：
   a. 从内表分区文件构建哈希表
   b. 从外表分区文件逐批读取并探测
   c. 处理完当前分区后关闭文件，处理下一对

---

## 14. 分区探测：probePartition

**位置**：`vechashjoin.cpp:1392-1422`

将外表数据按哈希值分区写入临时文件：

```
1. 初始化外表分区文件（文件数量与内表分区一致）
2. 循环从外表算子获取批次：
   batch = VectorEngine(outer_node)
   if batch 为 NULL: 结束
   调用 SaveToDisk<complicate_join_key, false>(batch) 写入分区文件
3. 释放文件处理器缓冲区
```

`SaveToDisk` 根据每行的哈希值决定写入哪个分区文件。

---

## 15. 分区准备：preparePartition

**位置**：`vechashjoin.cpp:1536-1690`

为每个分区对准备哈希表，是 GRACE 策略的核心调度函数：

```
遍历所有分区（从 m_probeIdx 开始）:

1. 判断是否需要处理该分区:
   - INNER/SEMI/RIGHT_SEMI: 两侧都有数据才处理
   - LEFT/ANTI/LEFT_ANTI_FULL: 外表有数据就处理
   - RIGHT/RIGHT_ANTI/RIGHT_ANTI_FULL: 内表有数据就处理
   
2. if 需要处理:
   a. 准备文件缓冲区
   b. 重置哈希上下文
   c. 回绕文件到起始位置
   
   d. 检查是否需要重分区:
      if 分区有效 且 内表文件大小 > 可用内存:
        调用 RePartitionFileSource() 对内表和外表进行二次分区
        检查重分区是否有效（最大子分区行数不等于原分区行数）
        关闭当前分区，继续下一个
      
   e. 从内表分区文件构建哈希表:
      调用 buildHashTable<...>(m_buildFileSource, row_count)
      
   f. 可选：输出哈希表统计信息（冲突分析）
   g. break — 准备就绪
   
3. if 不需要处理:
   关闭该分区文件对
   m_probeIdx++（跳到下一个分区）
```

---

## 16. 重分区：RePartitionFileSource

**位置**：`vechashjoin.cpp:1431-1534`

当某个分区仍然太大无法放入内存时，进行递归重分区：

```
1. 计算新的分区数:
   build 侧: 基于文件大小和可用内存计算，范围 [2, 1024]
   probe 侧: 与 build 侧新增的分区数对齐

2. 扩展文件源（增加新的分区文件）

3. 更新分区级别数组 m_pLevel[]（递增1，用于哈希位旋转）

4. 重分区过程:
   遍历当前分区文件中的每个批次:
     对每行:
       获取哈希值 hkey
       计算旋转位数: rbit = m_pLevel[file_idx] * 10
       新文件索引 = old_file_num + (leftrot(hkey, rbit) & (file_num - 1))
       写入对应的新分区文件

5. 释放新文件的缓冲区

6. 警告检查:
   if 分区级别 >= WARNING_SPILL_TIME: 记录警告
```

**关键设计**：
- 使用哈希值的不同位进行重分区（通过 `leftrot` 旋转），避免与之前的分区使用相同的位
- 检测无效重分区（所有数据仍落入同一个子分区），标记 `m_isValid[i] = false` 防止无限递归
- 支持多级递归重分区（`m_pLevel` 跟踪每个分区的递归深度）

---

## 17. 关键数据结构

### 17.1 hashCell — 哈希表单元

```cpp
struct hashCell {
    union {
        hashCell* m_next;   // 链表指针，指向同桶中的下一个 cell
        int m_rows;         // 批量加载时用于记录行数
    } flag;
    hashVal m_val[FLEXIBLE_ARRAY_MEMBER];  // 柔性数组，存储各列值
};

struct hashVal {
    ScalarValue val;  // 实际数据值
    uint8 flag;       // NULL 标记
};
```

对于 Right Join/Right Semi/Right Anti 类型，`m_val[m_cols-1]` 被用作匹配标记列。

### 17.2 vechashtable — 哈希表

```cpp
class vechashtable {
    int m_size;          // 哈希表大小（2 的幂）
    hashCell** m_data;   // 桶头指针数组
};
```

查找：`cell = m_data[hash_value & (m_size - 1)]`

### 17.3 JoinStateLog — 断点保存

```cpp
struct JoinStateLog {
    bool restore;        // 是否需要恢复
    int lastBuildIdx;    // 上次处理到的行索引
    hashCell* lastCell;  // 上次处理到的 cell 指针
};
```

用于在结果批次满（BatchMaxSize = 1000）时保存处理进度，下次调用时从断点继续。

### 17.4 Probe 相关数组

| 数组 | 用途 |
|---|---|
| `m_cacheLoc[BatchMaxSize]` | 每行对应的桶号 |
| `m_cellCache[BatchMaxSize]` | 每行当前正在比较的 cell 指针 |
| `m_match[BatchMaxSize]` | 标记每行是否找到了匹配 |
| `m_keyMatch[BatchMaxSize]` | 标记每行的键比较结果 |

### 17.5 hashSource 层次结构

```
hashSource (抽象基类)
  ├─ hashOpSource     — 从算子节点获取批次（用于内存探测）
  └─ hashFileSource   — 从临时文件获取批次（用于 GRACE 探测）
```

---

## 18. LLVM 优化路径

VecHashJoin 在以下三个热路径支持 LLVM JIT 编译优化：

| JIT 字段 | 优化路径 | 说明 |
|---|---|---|
| `jitted_probeHashTable` | PROBE_FETCH 阶段 | 哈希计算 + 桶定位 |
| `jitted_innerjoin` | Inner Join 匹配 | 键匹配 + 结果组装 |
| `jitted_hashclause` | 复杂键匹配 | hashclauses 表达式求值 |
| `jitted_joinqual` | buildResult/checkQual | joinqual 条件检查 |
| `jitted_matchkey` | 键匹配 | matchKey 函数 |

当 LLVM 编译版本可用时，`instrument->isLlvmOpt` 会被设为 `true` 用于 EXPLAIN ANALYZE 输出。

---

## 19. 完整调用关系图

```
ExecVecHashJoin(node)
│
├─ [HASH_BUILD] HashJoinTbl::Build()
│   └─ 构建哈希表（内表 → hashCell → vechashtable）
│
└─ [HASH_PROBE] HashJoinTbl::Probe()
    │
    ├─ probeMemory() ──────── [MEMORY_HASH 策略]
    │   │
    │   └─ probeHashTable(m_probOpSource)
    │       │
    │       ├─ [PROBE_FETCH]
    │       │   ├─ probSource->getBatch()         ← 获取外表批次
    │       │   ├─ hashBatch() / CalcComplicateHashVal()  ← 计算哈希
    │       │   ├─ 桶定位：m_cellCache[i] = m_hashTbl->m_data[hash & mask]
    │       │   └─ → PROBE_DATA
    │       │
    │       ├─ [PROBE_DATA]
    │       │   └─ (this->*m_joinFun)(batch)      ← 分发到具体 Join 函数
    │       │       ├─ innerJoinT()    ─┐
    │       │       ├─ leftJoinT()      │
    │       │       ├─ rightJoinT()     │ 每个函数内部:
    │       │       ├─ semiJoinT()      │ ① matchKey/matchComplicateKey
    │       │       ├─ antiJoinT()      │ ② 组装结果到 m_innerBatch/m_outerBatch
    │       │       ├─ rightSemiJoinT() │ ③ 链表推进 (m_cellCache[i] = next)
    │       │       ├─ rightAntiJoinT() │ ④ buildResult() → 投影 + 过滤
    │       │       └─ *WithQualT()   ─┘
    │       │
    │       └─ [PROBE_FINAL]
    │           └─ endJoin()                      ← Right 类型处理未匹配内表行
    │
    └─ probeGrace() ──────── [GRACE_HASH 策略]
        │
        ├─ [PROBE_PARTITION_FILE]
        │   └─ probePartition()                   ← 外表分区到临时文件
        │
        ├─ [PROBE_PREPARE_PAIR]
        │   └─ preparePartition()                 ← 准备分区对
        │       ├─ 检查是否需要重分区
        │       │   └─ RePartitionFileSource()    ← 递归重分区
        │       └─ buildHashTable() 从内表分区文件构建哈希表
        │
        └─ [PROBE_FETCH/DATA/FINAL]
            └─ probeHashTable(m_probeFileSource)  ← 复用核心探测逻辑
```

---

## 总结

VecHashJoin Probe 阶段的设计体现了以下核心思想：

1. **向量化处理**：以批次（VectorBatch，最多 1000 行）为单位处理数据，充分利用 CPU 缓存和 SIMD 潜力
2. **多层状态机**：ExecVecHashJoin（BUILD/PROBE）→ probeGrace（PARTITION/PREPARE/FETCH/DATA/FINAL）→ probeHashTable（FETCH/DATA/FINAL）→ joinT 内部循环
3. **模板特化消除分支**：通过 `<complicate_join_key, simple_key>` 模板参数在编译期消除运行时分支
4. **函数指针零开销分发**：初始化时选定 Join 函数指针，探测时直接调用，无需运行时判断
5. **自适应内存管理**：内存不足时自动从 MEMORY_HASH 切换到 GRACE_HASH，支持递归重分区
6. **LLVM JIT 加速**：核心热路径支持运行时编译优化
7. **断点续传**：通过 JoinStateLog 支持在批次满时保存进度并恢复，保证批次大小不超过 BatchMaxSize
