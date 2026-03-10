/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 * 
 * vechashjoin.h
 *     Prototypes for vectorized hash join
 * 
 * IDENTIFICATION
 *        src/include/vecexecutor/vechashjoin.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef VECHASHJOIN_H_
#define VECHASHJOIN_H_

#include "vecexecutor/vechashtable.h"
#include "vecexecutor/vecnodes.h"
#include "workload/workload.h"
// hash join runtime state
#define HASH_BUILD 0
#define HASH_PROBE 1
#define HASH_END 2

typedef enum {
    HASH_JOIN_INNER = 0,
    HASH_JOIN_LEFT,
    HASH_JOIN_RIGHT,
    HASH_JOIN_SEMI,
    HASH_JOIN_ANTI,
    HASH_JOIN_RIGHT_SEMI,
    HASH_JOIN_RIGHT_ANTI,
    HASH_JOIN_LEFT_ANTI_FULL,
    HASH_JOIN_RIGHT_ANTI_FULL,
#ifdef USE_SPQ
    HASH_JOIN_LASJ_NOTIN,
#endif
    HASH_JOIN_TYPE_NUM
} hashJoinType;

// hash strategy
#define MEMORY_HASH 0
#define GRACE_HASH 1

// probe status
#define PROBE_FETCH 0
#define PROBE_PARTITION_FILE 1
#define PROBE_DATA 2
#define PROBE_FINAL 3
#define PROBE_PREPARE_PAIR 4

extern VecHashJoinState* ExecInitVecHashJoin(VecHashJoin* node, EState* estate, int eflags);
extern VectorBatch* ExecVecHashJoin(VecHashJoinState* node);
extern void ExecEndVecHashJoin(VecHashJoinState* node);
extern void ExecReScanVecHashJoin(VecHashJoinState* node);
extern long ExecGetMemCostVecHash(VecHashJoin*);
extern void ExecEarlyFreeVecHashJoin(VecHashJoinState* node);

// Save current probing place for next join iteration
//
struct JoinStateLog {
    int lastBuildIdx;
    hashCell* lastCell;
    bool restore;
};

// Record hash key matching passed cells
//
struct ReCheckCellLoc {
    hashCell* cell;
    int resultIdx;
    int oriIdx;
};

// Vectorized hash join implementation class.
// The executor code often describes this object as the vectorized hash join
// operator; the concrete implementation is HashJoinTbl. The members below are
// grouped by function so it is easier to understand what each runtime field
// stores during build/probe/spill processing.
//
class HashJoinTbl : public hashBasedOperator {
public:
    HashJoinTbl(VecHashJoinState* runtimeContext);

    void Build();

    VectorBatch* Probe();

    void ResetNecessary();

public:
    // Number of columns produced by the outer child plan. This is used when
    // building result batches and when locating outer-side key/value columns.
    //
    int m_outCols;

    // Zero-based column indexes of the outer-side hash keys in the current
    // outer batch. Fast-path probing reads join keys directly through them.
    //
    int* m_outKeyIdx;

    // Original attribute numbers for the outer-side join keys. They preserve
    // the source-table column ids for cases such as index columns where the
    // attribute number in the current batch no longer matches the source slot.
    int* m_outOKeyIdx;

    // Collation OIDs of the outer-side join keys, used when comparing or
    // hashing collatable data types.
    Oid* m_outKeyCollation;

    // Temporary cache of build-side rows/cells before they are inserted into
    // the in-memory hash table or written to spill files.
    List* m_cache;

    // Current probe state machine stage, for example fetching outer input,
    // reading partition files, or producing final unmatched rows.
    int m_probeStatus;

    // True when at least one join key is not a simple Var/RelabelType-to-Var
    // reference and therefore must be evaluated through expression logic.
    bool m_complicateJoinKey;

    // Scratch vector that stores evaluated values for complex join-key
    // expressions. It is only allocated when m_complicateJoinKey is true.
    //
    ScalarVector* m_cjVector;

    // Whether all outer-side key columns are simple fixed-width types that can
    // use the simple-type fast path.
    bool m_outSimple;

    // Whether all inner/build-side key columns are simple fixed-width types.
    bool m_innerSimple;

    // Indicates whether the current probe step should continue checking the
    // hash-key equality after hash lookup succeeds.
    bool m_doProbeData;

    // Current batch fetched from the inner/build child; it feeds hash-table
    // construction or repartitioning.
    VectorBatch* m_innerBatch;

    // Current batch fetched from the outer/probe child.
    VectorBatch* m_outerBatch;

    // Materialized build-side batch used when complex join keys need extra
    // expression evaluation/storage.
    VectorBatch* m_complicate_innerBatch;

    // Materialized probe-side batch used when complex join keys need extra
    // expression evaluation/storage.
    VectorBatch* m_complicate_outerBatch;

    // Build-side batch projected into the shape required by join quals.
    VectorBatch* m_inQualBatch;

    // Probe-side batch projected into the shape required by join quals.
    VectorBatch* m_outQualBatch;

    // Raw outer batch kept before result projection so late stages can still
    // reference the original probe rows.
    VectorBatch* m_outRawBatch;

    // Final output batch returned to the parent executor node after join and
    // qualification processing.
    VectorBatch* m_result;

    // Executor runtime state (PlanState, quals, LLVM function pointers, etc.)
    // shared with ExecInitVecHashJoin/ExecVecHashJoin.
    VecHashJoinState* m_runtime;

    // Normalized hash-join type used by the internal dispatch tables.
    hashJoinType m_joinType;

    // Current hash strategy: pure in-memory hash join or grace hash join with
    // partition spill/reload.
    int m_strategy;

    // Saved cursor into the current hash bucket chain so the next executor call
    // can resume scanning the same probe row from the correct position.
    JoinStateLog m_joinStateLog;
    // Reader for the normal probe input (the outer child) when probing directly
    // from execution rather than from spilled partition files.
    hashOpSource* m_probOpSource;

    // Locations of build-side cells that passed hash-key recheck; used to
    // reconstruct result pairs after batch-oriented probing.
    ReCheckCellLoc m_reCheckCell[BatchMaxSize];

    // Per-row flags showing whether each probe row has found a qualifying match.
    bool m_match[BatchMaxSize];

    // Index of the current partition/file being probed during grace hash join.
    int m_probeIdx;

    // Per-row flags for NULL-eq-NULL special handling in joins that treat two
    // NULL keys as matching under nulleq semantics.
    bool m_nulleqmatch[BatchMaxSize];

    // Spill-file manager for build-side partitions written during grace hash
    // join or repartition.
    hashFileSource* m_buildFileSource;

    // Spill-file manager for probe-side partitions written during grace hash
    // join or repartition.
    hashFileSource* m_probeFileSource;

    // For each join key, records whether the key type is simple enough to use
    // the specialized fast comparison/hash path.
    bool* m_simpletype;

    // Data types of the outer-side join keys, mainly used by hashing, key
    // comparison, and bloom-filter-related logic.
    Oid* m_outerkeyType;

    /* Repartition depth of each spill file. A larger value means the file has
     * already been repartitioned more times during grace hash processing. */
    uint8* m_pLevel;

    /* Highest partition depth seen so far; instrumentation and warning logic
     * use it to summarize how severe spilling/repartition became. */
    uint8 m_maxPLevel;

    /* Whether each partition/file is still valid for further processing. Some
     * partitions can be skipped once they are proven empty or unnecessary. */
    bool* m_isValid;

    /* For semi/right-semi/right-anti style joins, keeps the matching build-side
     * cell pointer so the executor can still return values from the right tree. */
    hashCell** cellPoint;

    /* Set after spill/repartition crosses the warning threshold so the message
     * is emitted once instead of once per partition. */
    bool m_isWarning;

    // Accumulated build-side time reported through instrumentation/explain.
    double m_build_time;
    // Accumulated probe-side time reported through instrumentation/explain.
    double m_probe_time;

private:
    void SetJoinType();
    void PrepareProbe();

    template <bool complicateJoinKey, bool NeedCopy>
    void buildHashTable(hashSource* source, int64 rownum);

    template <bool complicateJoinKey>
    void bindingFp();

    // prepare for disk hash.
    void initFile(bool buildSide, VectorBatch* templateBatch, int fileNum);

    // Entry of the in-memory probe path. This is only a thin wrapper: the real
    // branch/state-machine logic is implemented in probeHashTable().
    VectorBatch* probeMemory();

    // probe the hash table in a grace way.
    VectorBatch* probeGrace();

    // Shared probe state machine used by both pure in-memory probing and the
    // per-partition probe phase of grace hash join.
    VectorBatch* probeHashTable(hashSource* probSource);

    // probe the partition.
    template <bool complicateJoinKey>
    void probePartition();

    /* repartition file source of a specific file */
    template <bool complicateJoinKey, bool buildside>
    void RePartitionFileSource(hashFileSource* hashSource, int fileIdx);

    /* record partition info into log file */
    void recordPartitionInfo(bool buildside, int fileIdx, int istart, int iend);

    // prepare the partition join.
    void preparePartition();

    // init memory control parameter
    void initMemoryControl();

    // calc the spilling file.
    int calcSpillFile();

    // Emit the final unmatched build-side rows required by right/right-anti
    // style joins after probe-side input has been exhausted.
    VectorBatch* endJoin();

    // build result batch.
    VectorBatch* buildResult(VectorBatch* inBatch, VectorBatch* outBatch, bool checkqual);

    bool* checkQual(VectorBatch* inBatch, VectorBatch* outBatch);

    // match key

    template <typename innerType, typename outerType, bool simpleType, bool nulleqnull>
    void matchKey(ScalarVector* key, int nrows, int hashValKeyIdx, int key_num);

    bool simpletype(Oid type);

    void matchComplicateKey(VectorBatch* batch);

    void DispatchKeyInnerFunction(int KeyIdx);

    template <typename innerType>
    void DispatchKeyOuterFunction(int KeyIdx);

    // different join function
    // full join
    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* innerJoinT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* leftJoinT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* leftJoinWithQualT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* rightJoinT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* rightJoinWithQualT(VectorBatch* batch);

    // semi join
    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* semiJoinT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* semiJoinWithQualT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* antiJoinT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* antiJoinWithQualT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* rightSemiJoinT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* rightSemiJoinWithQualT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* rightAntiJoinT(VectorBatch* batch);

    template <bool complicateJoinKey, bool simpleKey>
    VectorBatch* rightAntiJoinWithQualT(VectorBatch* batch);

    void PushDownFilterIfNeed();

private:
    // build function array.
    void (HashJoinTbl::*m_funBuild[2])(VectorBatch* batch);  // the build function;

    // Evaluate non-trivial hash-key expressions and fold them into m_cacheLoc[]
    // so complex join keys can reuse the same probe/build pipeline.
    void CalcComplicateHashVal(VectorBatch* batch, List* hashKeys, bool inner);

    bool HasEnoughMem(int nrows);

    template <bool complicateJoinKey, bool simple>
    void SaveToMemory(VectorBatch* batch);

    template <bool complicateJoinKey>
    void flushToDisk();

    template <bool complicateJoinKey, bool buildSide>
    void SaveToDisk(VectorBatch* batch);

    VectorBatch* (HashJoinTbl::*m_probeFun[2])();  // the probe function;

    VectorBatch* (HashJoinTbl::*m_joinFun)(VectorBatch* batch);  // join function
#ifdef USE_SPQ
    VectorBatch* (HashJoinTbl::*m_joinFunArray[40])(VectorBatch* batch);
#else
    VectorBatch* (HashJoinTbl::*m_joinFunArray[36])(VectorBatch* batch);
#endif
    typedef void (HashJoinTbl::*pMatchKeyFunc)(ScalarVector* key, int nrows, int hashValKeyIdx, int key_num);

    pMatchKeyFunc* m_matchKeyFunction;
};

#endif /* VECHASHJOIN_H_ */
