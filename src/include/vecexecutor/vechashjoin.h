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
/*
 * Hash join runtime states: track the major execution phase.
 * HASH_BUILD: Building the hash table from the inner (build) side relation.
 * HASH_PROBE: Probing the hash table with the outer (probe) side relation.
 * HASH_END:   Join completed; performing cleanup and returning final results.
 */
#define HASH_BUILD 0
#define HASH_PROBE 1
#define HASH_END 2

/*
 * hashJoinType: Enumerates all supported vectorized hash join types.
 * Each type corresponds to a different SQL join semantic and determines
 * how matched/unmatched tuples are handled during the probe phase.
 */
typedef enum {
    HASH_JOIN_INNER = 0,           /* Inner join: return only matched tuples from both sides */
    HASH_JOIN_LEFT,                /* Left outer join: return all outer tuples, NULLs for unmatched inner */
    HASH_JOIN_RIGHT,               /* Right outer join: return all inner tuples, NULLs for unmatched outer */
    HASH_JOIN_SEMI,                /* Left semi join: return outer tuples that have at least one match */
    HASH_JOIN_ANTI,                /* Left anti join: return outer tuples that have no match */
    HASH_JOIN_RIGHT_SEMI,          /* Right semi join: return inner tuples that have at least one match */
    HASH_JOIN_RIGHT_ANTI,          /* Right anti join: return inner tuples that have no match */
    HASH_JOIN_LEFT_ANTI_FULL,      /* Left anti full join: return unmatched tuples from both sides */
    HASH_JOIN_RIGHT_ANTI_FULL,     /* Right anti full join: return unmatched tuples from both sides */
#ifdef USE_SPQ
    HASH_JOIN_LASJ_NOTIN,         /* Left anti semi join NOT IN: handles NULL-aware anti join */
#endif
    HASH_JOIN_TYPE_NUM             /* Total number of join types (used for array sizing) */
} hashJoinType;

/*
 * Hash strategy: determines whether the hash join operates entirely
 * in memory or uses disk-based partitioning (Grace Hash) for large datasets.
 * MEMORY_HASH: All build-side data fits in memory; single-pass hash join.
 * GRACE_HASH:  Data exceeds memory; partitioned into files and processed
 *              partition-by-partition with possible recursive repartitioning.
 */
#define MEMORY_HASH 0
#define GRACE_HASH 1

/*
 * Probe status: tracks the sub-phase within the overall HASH_PROBE phase.
 * PROBE_FETCH:          Fetching the next batch from the outer (probe) side.
 * PROBE_PARTITION_FILE: Reading probe-side data from spilled partition files
 *                       (Grace Hash only).
 * PROBE_DATA:           Performing key matching and join on fetched data.
 * PROBE_FINAL:          Final pass to emit unmatched tuples for outer/anti joins.
 * PROBE_PREPARE_PAIR:   Preparing the next build/probe file pair for partition-
 *                       based processing in Grace Hash.
 */
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

/*
 * JoinStateLog: Saves the current probing position so that the join can
 * resume from where it left off in the next iteration. This is necessary
 * because a single probe-side tuple may match multiple build-side tuples
 * in the same hash bucket chain, and the result batch may fill up before
 * all matches are emitted.
 */
struct JoinStateLog {
    int lastBuildIdx;    /* Index of the last processed row in the probe batch.
                          * On restore, probing resumes from this row index. */
    hashCell* lastCell;  /* Pointer to the last visited cell in the hash bucket
                          * chain. On restore, chain traversal continues from
                          * the next cell after this one. */
    bool restore;        /* Flag indicating whether the join needs to restore
                          * from a saved position (true) or start fresh (false).
                          * Set to true when the result batch fills up mid-probe. */
};

/*
 * ReCheckCellLoc: Records the location of hash cells that passed the initial
 * hash key matching. These cells need further qualification checking (e.g.,
 * evaluating non-hashable join conditions or filter predicates) before they
 * can be included in the final result.
 */
struct ReCheckCellLoc {
    hashCell* cell;  /* Pointer to the matched hash cell in the build-side
                      * hash table that passed the key comparison. */
    int resultIdx;   /* Index in the result batch where this matched tuple
                      * will be placed if it passes qualification. */
    int oriIdx;      /* Original index in the probe-side batch, used to
                      * retrieve the corresponding probe tuple for building
                      * the combined result row. */
};

/*
 * HashJoinTbl: Vectorized hash join implementation class.
 *
 * This class implements the core logic for vectorized hash join operations,
 * supporting both in-memory (MEMORY_HASH) and disk-based (GRACE_HASH)
 * strategies. It processes data in vectorized batches (VectorBatch) for
 * improved CPU cache utilization and SIMD-friendly execution.
 *
 * Lifecycle:
 *   1. Construction: Initializes join configuration from the runtime state.
 *   2. Build():      Reads all inner-side batches, computes hash values,
 *                     and inserts them into the hash table.
 *   3. Probe():      Reads outer-side batches, looks up matching entries
 *                     in the hash table, and produces result batches.
 *   4. Cleanup:      Handled via ResetNecessary() or destructor.
 *
 * Inherits from hashBasedOperator which provides common hash table
 * infrastructure (hash table storage, memory management, hash functions).
 */
class HashJoinTbl : public hashBasedOperator {
public:
    HashJoinTbl(VecHashJoinState* runtimeContext);

    /* Build the hash table by reading all tuples from the inner (build) side. */
    void Build();

    /* Probe the hash table with outer-side tuples and return result batches.
     * Returns NULL when all results have been produced. */
    VectorBatch* Probe();

    /* Reset internal state for rescan operations (e.g., in nested loops). */
    void ResetNecessary();

public:
    /* ===== Outer (Probe) Side Column Configuration ===== */

    int m_outCols;             /* Total number of columns in the outer (probe) child
                                * plan node's output tuple descriptor. */

    int* m_outKeyIdx;          /* Array of column indices identifying which columns
                                * in the outer batch are join keys. Length equals
                                * the number of hash clauses. These indices map to
                                * positions in the outer batch's column array. */

    int* m_outOKeyIdx;         /* Array of original varattno values for outer join keys.
                                * Preserves the original attribute numbers before any
                                * projection or reordering, used for correct column
                                * reference resolution in complex expressions. */

    Oid* m_outKeyCollation;    /* Array of collation OIDs for outer join keys.
                                * Used when comparing string-type join keys to ensure
                                * locale-aware equality matching. One entry per key. */

    /* ===== Data Cache and Probe Status ===== */

    List* m_cache;             /* Linked list caching build-side hash cells in memory.
                                * Each list element points to the head of a batch's
                                * worth of hashCell entries. Used for bloom filter
                                * population and memory usage tracking. */

    int m_probeStatus;         /* Current sub-phase within the probe operation:
                                * PROBE_FETCH(0):          Fetching next outer batch.
                                * PROBE_PARTITION_FILE(1): Reading from spilled files.
                                * PROBE_DATA(2):           Performing key match/join.
                                * PROBE_FINAL(3):          Emitting unmatched tuples.
                                * PROBE_PREPARE_PAIR(4):   Preparing next file pair. */

    /* ===== Join Key Complexity ===== */

    bool m_complicateJoinKey;  /* True if any join key is an expression rather than
                                * a simple column reference (Var node). Expression-based
                                * keys require separate evaluation into m_cjVector before
                                * hash computation and comparison. */

    ScalarVector* m_cjVector;  /* Temporary vector for storing evaluated complex join key
                                * values. Only allocated when m_complicateJoinKey is true.
                                * Used during both build and probe phases to hold the
                                * result of evaluating join key expressions. */

    /* ===== Batch Encoding Flags ===== */

    bool m_outSimple;          /* True if the outer batch has a simple structure (no
                                * encoded/compressed columns). When true, data can be
                                * copied directly without decoding, enabling faster
                                * result batch construction. */

    bool m_innerSimple;        /* True if the inner batch has a simple structure (no
                                * encoded/compressed columns). Same optimization
                                * benefit as m_outSimple for the build side. */

    bool m_doProbeData;        /* Flag indicating whether there are remaining cells
                                * in the current hash bucket chain to check during
                                * probe. Set to true when a hash bucket has more cells
                                * to visit, triggering continued chain traversal. */

    /* ===== VectorBatch References ===== */

    VectorBatch* m_innerBatch;  /* Current batch from the inner (build) side child.
                                 * Points to the most recently fetched inner batch
                                 * during the build phase. */

    VectorBatch* m_outerBatch;  /* Current batch from the outer (probe) side child.
                                 * Points to the most recently fetched outer batch
                                 * during the probe phase. */

    VectorBatch* m_complicate_innerBatch;  /* Temporary batch for evaluating complex
                                            * join key expressions on inner-side data.
                                            * Used when m_complicateJoinKey is true. */

    VectorBatch* m_complicate_outerBatch;  /* Temporary batch for evaluating complex
                                            * join key expressions on outer-side data.
                                            * Used when m_complicateJoinKey is true. */

    VectorBatch* m_inQualBatch;   /* Batch holding inner-side columns arranged for
                                   * join qualification evaluation. Columns are laid
                                   * out to match the qualification expression's
                                   * expected input format. */

    VectorBatch* m_outQualBatch;  /* Batch holding outer-side columns arranged for
                                   * join qualification evaluation. Paired with
                                   * m_inQualBatch to evaluate join predicates. */

    VectorBatch* m_outRawBatch;   /* Raw output batch before applying final filters
                                   * or projections. Holds the combined inner+outer
                                   * columns from matched tuples. */

    VectorBatch* m_result;        /* Final result batch returned to the parent operator.
                                   * Contains the projected columns after join
                                   * qualification and any output filtering. */

    /* ===== Runtime and Join Configuration ===== */

    VecHashJoinState* m_runtime;  /* Back-pointer to the executor runtime state node.
                                   * Provides access to plan information, expression
                                   * contexts, JIT function pointers, and the overall
                                   * execution state. */

    hashJoinType m_joinType;      /* Type of hash join being performed (INNER, LEFT,
                                   * RIGHT, SEMI, ANTI, etc.). Determines which join
                                   * function template is bound to m_joinFun and how
                                   * matched/unmatched tuples are handled. */

    int m_strategy;               /* Hash join strategy:
                                   * MEMORY_HASH(0): Entire build side fits in memory.
                                   * GRACE_HASH(1): Build side exceeds memory; data is
                                   *   partitioned to disk files and processed
                                   *   partition-by-partition. */

    /* ===== Join State Management ===== */

    JoinStateLog m_joinStateLog;  /* Saves the current probing position (row index and
                                   * hash cell pointer) so that probing can resume in
                                   * the next call when the result batch fills up before
                                   * all matches in a bucket chain are processed. */

    hashOpSource* m_probOpSource; /* Data source for the probe side. In MEMORY_HASH mode,
                                   * this reads from the outer child plan node. In
                                   * GRACE_HASH mode, it reads from spilled partition
                                   * files. */

    /* ===== Match Result Arrays ===== */

    ReCheckCellLoc m_reCheckCell[BatchMaxSize];  /* Array recording cells that passed
                                                  * initial hash key matching and need
                                                  * further qualification checking.
                                                  * Indexed by result batch position. */

    bool m_match[BatchMaxSize];   /* Per-row match flags for the current probe batch.
                                   * m_match[i] is true if the i-th row in the probe
                                   * batch found a matching row in the build side.
                                   * Used by LEFT/ANTI joins to identify unmatched
                                   * outer tuples that need NULL-padded output. */

    int m_probeIdx;               /* Current partition file index during Grace Hash
                                   * processing. Iterates from 0 to (numFiles-1),
                                   * indicating which build/probe file pair is being
                                   * processed. */

    /* ===== Null Matching (Special Case) ===== */

    bool m_nulleqmatch[BatchMaxSize];  /* Per-row flags for NULL-equals-NULL matching.
                                        * Used when the join condition specifies that
                                        * NULL values should be treated as equal (e.g.,
                                        * IS NOT DISTINCT FROM). m_nulleqmatch[i] is
                                        * true if the i-th row matched via NULL equality. */

    /* ===== File Sources (Grace Hash) ===== */

    hashFileSource* m_buildFileSource;  /* File-based data source for build-side tuples
                                         * during Grace Hash processing. Manages reading
                                         * and writing of spilled build-side partitions. */

    hashFileSource* m_probeFileSource;  /* File-based data source for probe-side tuples
                                         * during Grace Hash processing. Manages reading
                                         * and writing of spilled probe-side partitions. */

    /* ===== Type Information ===== */

    bool* m_simpletype;         /* Per-key array indicating whether each join key is a
                                 * simple fixed-width type (INT1, INT2, INT4, INT8).
                                 * Simple types enable direct value comparison without
                                 * calling the full equality function via FmgrInfo. */

    Oid* m_outerkeyType;        /* Array of type OIDs for outer (probe) side join keys.
                                 * Used to select the appropriate comparison function
                                 * template and determine if fast-path integer comparison
                                 * can be used instead of generic function calls. */

    /* ===== Repartitioning State (Grace Hash) ===== */

    uint8* m_pLevel;            /* Per-partition array tracking the repartition depth level.
                                 * Starts at 0 and increments each time a partition is
                                 * recursively repartitioned because it still exceeds
                                 * available memory. */

    uint8 m_maxPLevel;          /* Maximum allowed repartition depth. Initialized in
                                 * initMemoryControl() based on system configuration.
                                 * When a partition reaches this level, a warning is
                                 * issued indicating potential data skew or insufficient
                                 * memory, and further repartitioning may be avoided. */

    bool* m_isValid;            /* Per-partition validity flags for repartitioning.
                                 * Set to false when repartitioning does not improve
                                 * partition sizes (e.g., due to many duplicate keys),
                                 * preventing infinite repartition loops. */

    /* ===== Semi-Join Support ===== */

    hashCell** cellPoint;       /* Array of pointers to hash cells for semi-join result
                                 * construction. In right semi joins, this array stores
                                 * pointers to the matched build-side cells so that
                                 * their column values can be included in the output. */

    /* ===== Warning and Performance Profiling ===== */

    bool m_isWarning;           /* Flag to prevent repeated warning messages. Set to true
                                 * after the first warning about excessive repartitioning
                                 * (exceeding m_maxPLevel) is printed, suppressing
                                 * duplicate warnings for subsequent partitions. */

    double m_build_time;        /* Accumulated wall-clock time (in seconds) spent in
                                 * the Build() phase. Used for performance profiling
                                 * and query execution statistics reporting. */

    double m_probe_time;        /* Accumulated wall-clock time (in seconds) spent in
                                 * the Probe() phase. Used for performance profiling
                                 * and query execution statistics reporting. */

private:
    /* Set m_joinType based on the plan node's join type (INNER, LEFT, etc.). */
    void SetJoinType();

    /* Initialize probe-side data sources and bind join function pointers. */
    void PrepareProbe();

    /*
     * Build the hash table from the given source.
     * Template parameters:
     *   complicateJoinKey: true if join keys are expressions requiring evaluation.
     *   NeedCopy: true if cell data must be deep-copied (e.g., from spilled files).
     */
    template <bool complicateJoinKey, bool NeedCopy>
    void buildHashTable(hashSource* source, int64 rownum);

    /*
     * Bind the appropriate join function pointer (m_joinFun) based on
     * join type and key complexity.
     */
    template <bool complicateJoinKey>
    void bindingFp();

    /* Initialize temporary files for Grace Hash partitioning. */
    void initFile(bool buildSide, VectorBatch* templateBatch, int fileNum);

    /* Probe the in-memory hash table (MEMORY_HASH strategy). */
    VectorBatch* probeMemory();

    /* Probe using Grace Hash: iterate over spilled partition file pairs. */
    VectorBatch* probeGrace();

    /* Core probe logic: look up probe batch in the hash table and produce matches. */
    VectorBatch* probeHashTable(hashSource* probSource);

    /* Partition probe-side data into files for Grace Hash processing. */
    template <bool complicateJoinKey>
    void probePartition();

    /* Repartition a specific file when a partition is still too large for memory. */
    template <bool complicateJoinKey, bool buildside>
    void RePartitionFileSource(hashFileSource* hashSource, int fileIdx);

    /* Write partition metadata to a log file for debugging/diagnostics. */
    void recordPartitionInfo(bool buildside, int fileIdx, int istart, int iend);

    /* Prepare the next build/probe file pair for partition-based processing. */
    void preparePartition();

    /* Initialize memory control parameters (work_mem limits, spill thresholds). */
    void initMemoryControl();

    /* Calculate which partition file to spill to when memory is exceeded. */
    int calcSpillFile();

    /* Finalize the join: emit remaining unmatched tuples for outer/anti joins. */
    VectorBatch* endJoin();

    /* Combine matched inner and outer tuples into the result batch,
     * optionally applying join qualification predicates. */
    VectorBatch* buildResult(VectorBatch* inBatch, VectorBatch* outBatch, bool checkqual);

    /* Evaluate join qualifications on candidate matches and return per-row pass flags. */
    bool* checkQual(VectorBatch* inBatch, VectorBatch* outBatch);

    /*
     * Compare join keys between probe-side values and build-side hash cells.
     * Template parameters:
     *   innerType/outerType: C++ types for direct comparison (e.g., int64).
     *   simpleType: true to use direct value comparison instead of function calls.
     *   nulleqnull: true to treat NULL = NULL as a match.
     */
    template <typename innerType, typename outerType, bool simpleType, bool nulleqnull>
    void matchKey(ScalarVector* key, int nrows, int hashValKeyIdx, int key_num);

    /* Check if the given OID is a simple fixed-width integer type. */
    bool simpletype(Oid type);

    /* Match keys for complex (expression-based) join conditions. */
    void matchComplicateKey(VectorBatch* batch);

    /* Dispatch to the correct inner-type-specialized matchKey function. */
    void DispatchKeyInnerFunction(int KeyIdx);

    /* Dispatch to the correct outer-type-specialized matchKey function. */
    template <typename innerType>
    void DispatchKeyOuterFunction(int KeyIdx);

    /* ===== Join Function Templates ===== */
    /* Each join type has a templated implementation parameterized by:
     *   complicateJoinKey: whether join keys are expressions.
     *   simpleKey: whether all join keys are simple integer types.
     * Variants with "WithQual" suffix handle additional non-hashable
     * join predicates that must be checked after key matching. */

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

    /* Push down bloom filters to the outer scan if conditions are met. */
    void PushDownFilterIfNeed();

private:
    /* Array of two build function pointers indexed by NeedCopy flag (0 or 1). */
    void (HashJoinTbl::*m_funBuild[2])(VectorBatch* batch);

    /* Evaluate complex join key expressions and compute hash values. */
    void CalcComplicateHashVal(VectorBatch* batch, List* hashKeys, bool inner);

    /* Check if there is enough memory to continue in-memory hash building. */
    bool HasEnoughMem(int nrows);

    /* Save a batch of tuples to the in-memory hash table. */
    template <bool complicateJoinKey, bool simple>
    void SaveToMemory(VectorBatch* batch);

    /* Flush in-memory data to disk when memory limit is exceeded (Grace Hash). */
    template <bool complicateJoinKey>
    void flushToDisk();

    /* Write a batch of tuples to a disk partition file. */
    template <bool complicateJoinKey, bool buildSide>
    void SaveToDisk(VectorBatch* batch);

    /* Array of two probe function pointers:
     * [0] = probeMemory (MEMORY_HASH), [1] = probeGrace (GRACE_HASH). */
    VectorBatch* (HashJoinTbl::*m_probeFun[2])();

    /* Bound join function pointer, selected based on join type, key complexity,
     * and whether additional qualifications exist. Points to one of the
     * *JoinT / *JoinWithQualT template instantiations. */
    VectorBatch* (HashJoinTbl::*m_joinFun)(VectorBatch* batch);

    /* Lookup table of all join function instantiations, indexed by
     * [joinType * 4 + complicateJoinKey * 2 + simpleKey]. */
#ifdef USE_SPQ
    VectorBatch* (HashJoinTbl::*m_joinFunArray[40])(VectorBatch* batch);
#else
    VectorBatch* (HashJoinTbl::*m_joinFunArray[36])(VectorBatch* batch);
#endif

    typedef void (HashJoinTbl::*pMatchKeyFunc)(ScalarVector* key, int nrows, int hashValKeyIdx, int key_num);

    /* Array of key matching function pointers, one per join key,
     * selected based on inner/outer type and simple-type optimization. */
    pMatchKeyFunc* m_matchKeyFunction;
};

#endif /* VECHASHJOIN_H_ */
