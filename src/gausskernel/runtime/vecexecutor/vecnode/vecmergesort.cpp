/* -------------------------------------------------------------------------
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * vecmergesort.cpp
 *      Vectorized merge sort operator using the merge path algorithm.
 *
 *      This operator merges two pre-sorted input streams (left and right
 *      child plans) into a single sorted output. It uses the merge path
 *      algorithm to efficiently determine batch-level merge boundaries
 *      via binary search on the merge diagonal, enabling cache-friendly
 *      vectorized batch output.
 *
 * IDENTIFICATION
 *      Code/src/gausskernel/runtime/vecexecutor/vecnode/vecmergesort.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "executor/exec/execdebug.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "vecexecutor/vecnodes.h"
#include "vecexecutor/vecnodemergesort.h"
#include "vecexecutor/vecexecutor.h"
#include "vecexecutor/vectorbatch.h"
#include "utils/lsyscache.h"
#include "utils/sortsupport.h"
#include "miscadmin.h"

/*
 * CompareRows
 *
 * Compare a row from the left batch at position leftIdx with a row from
 * the right batch at position rightIdx, using the configured sort keys.
 *
 * Returns negative if left < right, positive if left > right, 0 if equal.
 */
static int CompareRows(VecMergeSortState* node,
    VectorBatch* leftBatch, int leftIdx,
    VectorBatch* rightBatch, int rightIdx)
{
    SortSupport sortKeys = node->sortKeys;
    int numCols = node->numCols;
    VecMergeSort* planNode = (VecMergeSort*)node->ps.plan;

    for (int i = 0; i < numCols; i++) {
        AttrNumber attno = planNode->sortColIdx[i] - 1;
        ScalarVector* leftCol = &leftBatch->m_arr[attno];
        ScalarVector* rightCol = &rightBatch->m_arr[attno];
        bool leftNull = IS_NULL(leftCol->m_flag[leftIdx]);
        bool rightNull = IS_NULL(rightCol->m_flag[rightIdx]);

        int cmp = ApplySortComparator(
            leftNull ? (Datum)0 : leftCol->m_vals[leftIdx], leftNull,
            rightNull ? (Datum)0 : rightCol->m_vals[rightIdx], rightNull,
            &sortKeys[i]);

        if (cmp != 0)
            return cmp;
    }
    return 0;
}

/*
 * FetchNextBatch
 *
 * Fetch the next batch from the specified child plan (left or right).
 * Returns the new batch, or NULL if the child is exhausted.
 */
static VectorBatch* FetchNextBatch(VecMergeSortState* node, bool isLeft)
{
    PlanState* childPlan = isLeft ? outerPlanState(node) : innerPlanState(node);
    VectorBatch* batch = VectorEngine(childPlan);

    if (BatchIsNull(batch)) {
        return NULL;
    }
    return batch;
}

/*
 * CopyRowToOutput
 *
 * Copy a single row at srcIdx from srcBatch into the output batch at dstIdx.
 */
static void CopyRowToOutput(VectorBatch* dstBatch, int dstIdx,
    VectorBatch* srcBatch, int srcIdx)
{
    for (int col = 0; col < dstBatch->m_cols; col++) {
        ScalarVector* dstCol = &dstBatch->m_arr[col];
        ScalarVector* srcCol = &srcBatch->m_arr[col];

        if (IS_NULL(srcCol->m_flag[srcIdx])) {
            SET_NULL(dstCol->m_flag[dstIdx]);
        } else {
            dstCol->m_flag[dstIdx] = srcCol->m_flag[srcIdx];
            if (srcCol->m_desc.encoded) {
                dstCol->m_vals[dstIdx] = dstCol->AddVar(srcCol->m_vals[srcIdx], dstIdx);
            } else {
                dstCol->m_vals[dstIdx] = srcCol->m_vals[srcIdx];
            }
        }
    }
}

/*
 * MergePathBinarySearch
 *
 * Merge Path Algorithm core: given two sorted sequences represented by
 * leftBatch[leftStart..leftEnd-1] and rightBatch[rightStart..rightEnd-1],
 * find the merge path diagonal crossing for producing `target` output elements.
 *
 * Returns the number of elements to take from the left sequence (i.e., leftCount).
 * The remaining (target - leftCount) come from the right sequence.
 *
 * The merge path diagonal at position k splits the merge into:
 *   - left[0..i-1]  and  right[0..j-1]  where i + j = k
 * We find the i that satisfies:
 *   left[i-1] <= right[j]  and  right[j-1] <= left[i]
 * using binary search on i in [max(0, k-n), min(k, m)].
 */
static int MergePathBinarySearch(VecMergeSortState* node,
    VectorBatch* leftBatch, int leftStart, int leftAvail,
    VectorBatch* rightBatch, int rightStart, int rightAvail,
    int target)
{
    /* Binary search bounds for how many from left */
    int lo = Max(0, target - rightAvail);
    int hi = Min(target, leftAvail);

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int rightIdx = target - mid - 1;

        /*
         * Compare left[leftStart + mid] with right[rightStart + rightIdx].
         * If left[mid] > right[target - mid - 1], we need fewer from left.
         */
        if (mid < leftAvail && rightIdx >= 0 && rightIdx < rightAvail) {
            int cmp = CompareRows(node,
                leftBatch, leftStart + mid,
                rightBatch, rightStart + rightIdx);
            if (cmp > 0) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        } else if (mid >= leftAvail) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    return lo;
}

/*
 * RefillBatch
 *
 * Refill a working batch from the specified child plan.
 * The batch object is always pre-allocated (non-NULL), so we Reset() it
 * and Copy in the next batch of data. Returns true if data was fetched,
 * false if the input is exhausted.
 */
static bool RefillBatch(VecMergeSortState* node, bool isLeft)
{
    VectorBatch* workBatch = isLeft ? node->m_leftBatch : node->m_rightBatch;

    workBatch->Reset();
    VectorBatch* batch = FetchNextBatch(node, isLeft);
    if (batch != NULL) {
        workBatch->Copy<true, false>(batch);
        if (isLeft) {
            node->m_leftPos = 0;
        } else {
            node->m_rightPos = 0;
        }
        return true;
    }
    return false;
}

/*
 * ProduceMergedBatch
 *
 * Produce one output batch by merging elements from the left and right
 * input batches using the merge path algorithm. This function fills
 * the output batch up to BatchMaxSize rows.
 *
 * Returns the number of rows written to the output batch.
 */
static int ProduceMergedBatch(VecMergeSortState* node, VectorBatch* outBatch)
{
    int outRows = 0;

    while (outRows < BatchMaxSize) {
        /*
         * Ensure we have data from both sides if not exhausted.
         * The batch objects are always allocated; check if current data
         * has been fully consumed (m_rows == 0 or pos >= rows).
         */
        if (!node->m_leftExhausted &&
            node->m_leftPos >= node->m_leftBatch->m_rows) {
            if (!RefillBatch(node, true))
                node->m_leftExhausted = true;
        }
        if (!node->m_rightExhausted &&
            node->m_rightPos >= node->m_rightBatch->m_rows) {
            if (!RefillBatch(node, false))
                node->m_rightExhausted = true;
        }

        int leftAvail = 0;
        int rightAvail = 0;

        if (!node->m_leftExhausted) {
            leftAvail = node->m_leftBatch->m_rows - node->m_leftPos;
        }
        if (!node->m_rightExhausted) {
            rightAvail = node->m_rightBatch->m_rows - node->m_rightPos;
        }

        /* Both inputs exhausted */
        if (leftAvail == 0 && rightAvail == 0)
            break;

        int remaining = BatchMaxSize - outRows;
        int totalAvail = leftAvail + rightAvail;
        int target = Min(remaining, totalAvail);

        if (leftAvail == 0) {
            /* Only right side has data - copy directly */
            int toCopy = Min(remaining, rightAvail);
            for (int i = 0; i < toCopy; i++) {
                CopyRowToOutput(outBatch, outRows + i,
                    node->m_rightBatch, node->m_rightPos + i);
            }
            outRows += toCopy;
            node->m_rightPos += toCopy;
        } else if (rightAvail == 0) {
            /* Only left side has data - copy directly */
            int toCopy = Min(remaining, leftAvail);
            for (int i = 0; i < toCopy; i++) {
                CopyRowToOutput(outBatch, outRows + i,
                    node->m_leftBatch, node->m_leftPos + i);
            }
            outRows += toCopy;
            node->m_leftPos += toCopy;
        } else {
            /*
             * Both sides have data - use merge path to determine
             * how many elements come from each side for this chunk.
             */
            int leftCount = MergePathBinarySearch(node,
                node->m_leftBatch, node->m_leftPos, leftAvail,
                node->m_rightBatch, node->m_rightPos, rightAvail,
                target);
            int rightCount = target - leftCount;

            /*
             * Now merge leftCount elements from left and rightCount
             * from right in sorted order into the output batch.
             */
            int li = 0;
            int ri = 0;
            for (int k = 0; k < target; k++) {
                bool takeLeft = false;
                if (li < leftCount && ri < rightCount) {
                    int cmp = CompareRows(node,
                        node->m_leftBatch, node->m_leftPos + li,
                        node->m_rightBatch, node->m_rightPos + ri);
                    takeLeft = (cmp <= 0);
                } else if (li < leftCount) {
                    takeLeft = true;
                } else {
                    takeLeft = false;
                }

                if (takeLeft) {
                    CopyRowToOutput(outBatch, outRows + k,
                        node->m_leftBatch, node->m_leftPos + li);
                    li++;
                } else {
                    CopyRowToOutput(outBatch, outRows + k,
                        node->m_rightBatch, node->m_rightPos + ri);
                    ri++;
                }
            }

            outRows += target;
            node->m_leftPos += leftCount;
            node->m_rightPos += rightCount;
        }
    }

    return outRows;
}

/* ----------------------------------------------------------------
 *
 *              ExecVecMergeSort
 *
 *      Main execution entry for the vectorized merge sort operator.
 *      Produces one output batch per call by merging the two sorted
 *      input streams using the merge path algorithm.
 *
 * ----------------------------------------------------------------
 */
VectorBatch* ExecVecMergeSort(VecMergeSortState* node)
{
    VectorBatch* outBatch = node->m_pCurrentBatch;

    CHECK_FOR_INTERRUPTS();

    /* Reset the output batch */
    outBatch->Reset(true);

    /* Produce a merged output batch */
    int rows = ProduceMergedBatch(node, outBatch);

    if (rows == 0) {
        return NULL;
    }

    /* Update row counts on output batch columns */
    outBatch->m_rows = rows;
    for (int i = 0; i < outBatch->m_cols; i++) {
        outBatch->m_arr[i].m_rows = rows;
    }

    return outBatch;
}

/* ----------------------------------------------------------------
 *
 *              ExecInitVecMergeSort
 *
 *      Initialize the vectorized merge sort state node.
 *
 * ----------------------------------------------------------------
 */
VecMergeSortState* ExecInitVecMergeSort(VecMergeSort* node, EState* estate, int eflags)
{
    VecMergeSortState* state = NULL;

    /*
     * Create state structure
     */
    state = makeNode(VecMergeSortState);
    state->ps.plan = (Plan*)node;
    state->ps.state = estate;
    state->ps.vectorized = true;

    state->merge_Done = false;
    state->numCols = node->numCols;

    /*
     * Miscellaneous initialization
     */
    ExecInitResultTupleSlot(estate, &state->ps);

    /*
     * Initialize child nodes.
     * Left child = outer plan, Right child = inner plan.
     * Both must produce sorted output.
     */
    outerPlanState(state) = ExecInitNode(outerPlan(node), estate, eflags);
    innerPlanState(state) = ExecInitNode(innerPlan(node), estate, eflags);

    /*
     * Initialize result tuple type from outer plan (both children should
     * produce same tuple descriptor).
     */
    TupleDesc outerDesc = ExecGetResultType(outerPlanState(state));
    ExecAssignResultType(&state->ps,
        outerDesc,
        outerDesc->td_tam_ops);
    state->ps.ps_ProjInfo = NULL;

    /*
     * Initialize the sort key comparison support
     */
    state->sortKeys = (SortSupport)palloc0(node->numCols * sizeof(SortSupportData));
    for (int i = 0; i < node->numCols; i++) {
        SortSupport sortKey = &state->sortKeys[i];
        sortKey->ssup_cxt = CurrentMemoryContext;
        sortKey->ssup_collation = node->collations[i];
        sortKey->ssup_nulls_first = node->nullsFirst[i];
        sortKey->ssup_attno = node->sortColIdx[i];
        sortKey->abbreviate = false;

        PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
    }

    /*
     * Create working batches
     */
    MemoryContext context = CurrentMemoryContext;
    state->m_pCurrentBatch = New(context) VectorBatch(context, outerDesc);
    state->m_leftBatch = New(context) VectorBatch(context, outerDesc);
    state->m_rightBatch = New(context) VectorBatch(context, outerDesc);

    state->m_leftPos = 0;
    state->m_rightPos = 0;
    state->m_leftExhausted = false;
    state->m_rightExhausted = false;

    /* Pre-fetch first batches from both children using RefillBatch */
    if (!RefillBatch(state, true))
        state->m_leftExhausted = true;
    if (!RefillBatch(state, false))
        state->m_rightExhausted = true;

    return state;
}

/* ----------------------------------------------------------------
 *
 *              ExecEndVecMergeSort
 *
 * ----------------------------------------------------------------
 */
void ExecEndVecMergeSort(VecMergeSortState* node)
{
    /*
     * clean out the tuple table
     */
    (void)ExecClearTuple(node->ps.ps_ResultTupleSlot);

    /*
     * shut down the child subplans
     */
    ExecEndNode(outerPlanState(node));
    ExecEndNode(innerPlanState(node));
}

/* ----------------------------------------------------------------
 *
 *              ExecReScanVecMergeSort
 *
 * ----------------------------------------------------------------
 */
void ExecReScanVecMergeSort(VecMergeSortState* node)
{
    node->merge_Done = false;
    node->m_leftPos = 0;
    node->m_rightPos = 0;
    node->m_leftExhausted = false;
    node->m_rightExhausted = false;

    if (node->m_leftBatch != NULL)
        node->m_leftBatch->Reset();
    if (node->m_rightBatch != NULL)
        node->m_rightBatch->Reset();
    if (node->m_pCurrentBatch != NULL)
        node->m_pCurrentBatch->Reset();

    /*
     * If chgParam of subnode is not null then plan will be re-scanned by
     * first ExecProcNode.
     */
    if (outerPlanState(node)->chgParam == NULL)
        VecExecReScan(outerPlanState(node));
    if (innerPlanState(node)->chgParam == NULL)
        VecExecReScan(innerPlanState(node));

    /* Re-fetch first batches using RefillBatch */
    if (!RefillBatch(node, true))
        node->m_leftExhausted = true;
    if (!RefillBatch(node, false))
        node->m_rightExhausted = true;
}

/*
 * @Description: Early free the memory for VecMergeSort.
 *
 * @param[IN] node:  vector executor state for MergeSort
 * @return: void
 */
void ExecEarlyFreeVecMergeSort(VecMergeSortState* node)
{
    PlanState* plan_state = &node->ps;

    if (plan_state->earlyFreed)
        return;

    (void)ExecClearTuple(node->ps.ps_ResultTupleSlot);

    EARLY_FREE_LOG(elog(LOG,
        "Early Free: After early freeing VecMergeSort "
        "at node %d, memory used %d MB.",
        plan_state->plan->plan_node_id,
        getSessionMemoryUsageMB()));

    plan_state->earlyFreed = true;
    ExecEarlyFree(outerPlanState(node));
    ExecEarlyFree(innerPlanState(node));
}
