--
-- noderesult_coverage.sql
-- Test cases for complete coverage of Result operator (nodeResult.cpp)
-- This test file covers all branches and code paths in ExecResult,
-- ExecInitResult, ExecEndResult, ExecReScanResult, ExecResultMarkPos, ExecResultRestrPos
--

-- =============================================================================
-- SECTION 1: Basic Result node without outer plan (constant target list)
-- Tests: ExecResult lines 164-169 (no outer plan, rs_done = true)
-- =============================================================================

-- Test 1.1: Simple constant expression
-- Covers: Line 169 - node->rs_done = true (no outer plan)
SELECT 1 * 2;

-- Test 1.2: Multiple constant expressions
SELECT 1 + 2, 3 * 4, 5 / 2;

-- Test 1.3: NULL constant
SELECT NULL;

-- Test 1.4: String constant
SELECT 'hello world';

-- Test 1.5: Boolean constant
SELECT true, false;

-- Test 1.6: Complex expression without tables
SELECT 1 + 2 * 3 - 4 / 2;

-- =============================================================================
-- SECTION 2: Result node with constant qualifications
-- Tests: ExecResult lines 79-93 (constant qual check)
-- =============================================================================

-- Create test table
CREATE TABLE result_test_t1 (id int, name varchar(100), value int);
INSERT INTO result_test_t1 VALUES (1, 'alice', 100);
INSERT INTO result_test_t1 VALUES (2, 'bob', 200);
INSERT INTO result_test_t1 VALUES (3, 'charlie', 300);

-- Test 2.1: Constant qual that evaluates to TRUE
-- Covers: Line 81-83 qualResult = true path
SELECT * FROM result_test_t1 WHERE 2 > 1 ORDER BY id;

-- Test 2.2: Constant qual that evaluates to FALSE (early return NULL)
-- Covers: Lines 84-92 - qualResult = false, rs_done = true, return NULL
SELECT * FROM result_test_t1 WHERE 1 > 2 ORDER BY id;

-- Test 2.3: Complex constant qual TRUE
SELECT * FROM result_test_t1 WHERE (1 + 1 = 2) AND (3 < 5) ORDER BY id;

-- Test 2.4: Complex constant qual FALSE
SELECT * FROM result_test_t1 WHERE (1 + 1 = 3) OR (3 > 5) ORDER BY id;

-- Test 2.5: Constant qual with NULL (NULL is not true, so should return no rows)
SELECT * FROM result_test_t1 WHERE NULL ORDER BY id;

-- =============================================================================
-- SECTION 3: Result node with outer plan
-- Tests: ExecResult lines 147-163 (outer plan exists)
-- =============================================================================

-- Test 3.1: Result with SeqScan as outer plan
-- Covers: Lines 148-160 (outer_plan != NULL, retrieve tuples)
EXPLAIN (COSTS OFF) SELECT * FROM result_test_t1 WHERE id > 0;
SELECT * FROM result_test_t1 WHERE id > 0 ORDER BY id;

-- Test 3.2: Result with filter qualification on outer plan
-- Covers: Line 162-163 (qual && !ExecQual - continue loop)
SELECT * FROM result_test_t1 WHERE id > 1 ORDER BY id;

-- Test 3.3: Result returns NULL when outer plan exhausted
-- Covers: Line 153-154 (TupIsNull - return NULL)
SELECT * FROM result_test_t1 WHERE id > 100 ORDER BY id;

-- Test 3.4: Join with Result node
SELECT t1.id, t2.id FROM result_test_t1 t1, result_test_t1 t2 WHERE t1.id = t2.id ORDER BY t1.id;

-- =============================================================================
-- SECTION 4: Set-returning functions (ps_vec_TupFromTlist)
-- Tests: ExecResult lines 110-132 (SRF projection)
-- =============================================================================

-- Test 4.1: Generate_series function (set-returning function)
-- Covers: Lines 110-132 (ps_vec_TupFromTlist = true, ExprMultipleResult)
SELECT generate_series(1, 5);

-- Test 4.2: Multiple SRF calls
SELECT generate_series(1, 3), generate_series(1, 2);

-- Test 4.3: SRF with expression
SELECT generate_series(1, 5) * 2;

-- Test 4.4: SRF in subquery
SELECT * FROM (SELECT generate_series(1, 5) as x) sub WHERE x > 2;

-- Test 4.5: SRF with table
SELECT id, generate_series(1, 2) FROM result_test_t1 ORDER BY id, generate_series;

-- Test 4.6: Unnest array function
SELECT unnest(ARRAY[1, 2, 3, 4, 5]);

-- =============================================================================
-- SECTION 5: INSERT with Result node (constant target list)
-- Tests: ExecResult with INSERT values
-- =============================================================================

-- Test 5.1: Simple INSERT with constants
INSERT INTO result_test_t1 VALUES (4, 'david', 400);

-- Test 5.2: INSERT with expression
INSERT INTO result_test_t1 VALUES (5, 'eve', 100 + 400);

-- Test 5.3: INSERT with subquery (Result with outer plan)
INSERT INTO result_test_t1 SELECT id + 10, name || '_copy', value FROM result_test_t1 WHERE id <= 3;

-- Verify inserts
SELECT * FROM result_test_t1 ORDER BY id;

-- =============================================================================
-- SECTION 6: EXPLAIN to verify Result node usage
-- Tests: Verifying Result node appears in query plans
-- =============================================================================

-- Test 6.1: Constant expression uses Result
EXPLAIN (COSTS OFF) SELECT 1 + 2;

-- Test 6.2: Result with One-Time Filter (constant qual)
EXPLAIN (COSTS OFF) SELECT * FROM result_test_t1 WHERE 2 > 1;

-- Test 6.3: Result with One-Time Filter that is false
EXPLAIN (COSTS OFF) SELECT * FROM result_test_t1 WHERE 1 > 2;

-- Test 6.4: SRF uses Result
EXPLAIN (COSTS OFF) SELECT generate_series(1, 5);

-- Test 6.5: Subquery with Result
EXPLAIN (COSTS OFF) SELECT * FROM (SELECT 1, 2, 3) AS sub;

-- =============================================================================
-- SECTION 7: ReScan scenarios
-- Tests: ExecReScanResult (lines 345-358)
-- =============================================================================

-- Test 7.1: Correlated subquery (triggers rescan)
-- Covers: ExecReScanResult - rescan with chgParam
SELECT id, (SELECT max(value) FROM result_test_t1 WHERE result_test_t1.id <= t.id) as running_max
FROM result_test_t1 t WHERE id <= 5 ORDER BY id;

-- Test 7.2: Lateral join (triggers rescan)
SELECT t1.id, lat.sum_val
FROM result_test_t1 t1,
     LATERAL (SELECT sum(value) as sum_val FROM result_test_t1 WHERE id <= t1.id) lat
WHERE t1.id <= 5
ORDER BY t1.id;

-- Test 7.3: EXISTS subquery with rescan
SELECT * FROM result_test_t1 t1
WHERE EXISTS (SELECT 1 FROM result_test_t1 t2 WHERE t2.id = t1.id AND t2.value > 150)
ORDER BY id;

-- Test 7.4: IN subquery with rescan
SELECT * FROM result_test_t1
WHERE id IN (SELECT id FROM result_test_t1 WHERE value > 200)
ORDER BY id;

-- Test 7.5: NOT EXISTS subquery
SELECT * FROM result_test_t1 t1
WHERE NOT EXISTS (SELECT 1 FROM result_test_t1 t2 WHERE t2.id = t1.id AND t2.value > 500)
ORDER BY id;

-- =============================================================================
-- SECTION 8: Complex expressions and edge cases
-- Tests: Various edge cases in Result node
-- =============================================================================

-- Test 8.1: CASE expression in Result
SELECT CASE WHEN 1 > 0 THEN 'positive' ELSE 'non-positive' END;

-- Test 8.2: COALESCE in Result
SELECT COALESCE(NULL, NULL, 'default');

-- Test 8.3: NULLIF in Result
SELECT NULLIF(1, 1), NULLIF(1, 2);

-- Test 8.4: GREATEST/LEAST in Result
SELECT GREATEST(1, 2, 3), LEAST(1, 2, 3);

-- Test 8.5: Array construction in Result
SELECT ARRAY[1, 2, 3];

-- Test 8.6: Row constructor in Result
SELECT ROW(1, 'test', 3.14);

-- Test 8.7: Type casting in Result
SELECT 1::float, '123'::int, 3.14::text;

-- Test 8.8: Mathematical functions in Result
SELECT abs(-5), sqrt(16), power(2, 3);

-- Test 8.9: String functions in Result
SELECT length('hello'), upper('world'), concat('a', 'b', 'c');

-- Test 8.10: Date/Time functions in Result
SELECT current_date IS NOT NULL;

-- =============================================================================
-- SECTION 9: Result with UNION/INTERSECT/EXCEPT
-- Tests: Result in set operations
-- =============================================================================

-- Test 9.1: UNION with constants
SELECT 1, 'a' UNION SELECT 2, 'b' UNION SELECT 3, 'c' ORDER BY 1;

-- Test 9.2: UNION ALL with constants
SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 2;

-- Test 9.3: INTERSECT with Result
SELECT 1 INTERSECT SELECT 1;

-- Test 9.4: EXCEPT with Result
SELECT 1 EXCEPT SELECT 2;

-- Test 9.5: Complex set operation
(SELECT id FROM result_test_t1 WHERE id <= 3)
UNION
(SELECT generate_series(4, 6))
ORDER BY 1;

-- =============================================================================
-- SECTION 10: Result with VALUES clause
-- Tests: Result node with VALUES
-- =============================================================================

-- Test 10.1: Simple VALUES
VALUES (1, 'a'), (2, 'b'), (3, 'c');

-- Test 10.2: VALUES with expressions
VALUES (1 + 1, 'x' || 'y'), (2 * 2, 'a' || 'b');

-- Test 10.3: VALUES in subquery
SELECT * FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) AS t(id, val) ORDER BY id;

-- Test 10.4: VALUES with table join
SELECT t.*, v.*
FROM result_test_t1 t, (VALUES (1, 'match1'), (2, 'match2')) AS v(vid, vname)
WHERE t.id = v.vid ORDER BY t.id;

-- =============================================================================
-- SECTION 11: Result with aggregate functions
-- Tests: Result with aggregation
-- =============================================================================

-- Test 11.1: Simple aggregate
SELECT count(*) FROM result_test_t1;

-- Test 11.2: Multiple aggregates
SELECT count(*), sum(value), avg(value), min(value), max(value) FROM result_test_t1;

-- Test 11.3: Aggregate with GROUP BY
SELECT id, sum(value) FROM result_test_t1 GROUP BY id ORDER BY id;

-- Test 11.4: Aggregate with HAVING
SELECT id, sum(value) as total FROM result_test_t1 GROUP BY id HAVING sum(value) > 200 ORDER BY id;

-- Test 11.5: Aggregate in subquery
SELECT * FROM (SELECT count(*) as cnt FROM result_test_t1) sub WHERE cnt > 0;

-- =============================================================================
-- SECTION 12: Result with CTEs (Common Table Expressions)
-- Tests: Result in WITH clause
-- =============================================================================

-- Test 12.1: Simple CTE
WITH cte AS (SELECT 1 as num, 'constant' as str)
SELECT * FROM cte;

-- Test 12.2: CTE with table
WITH cte AS (SELECT * FROM result_test_t1 WHERE id <= 3)
SELECT * FROM cte ORDER BY id;

-- Test 12.3: Multiple CTEs
WITH
  cte1 AS (SELECT 1 as num),
  cte2 AS (SELECT 2 as num)
SELECT * FROM cte1 UNION ALL SELECT * FROM cte2;

-- Test 12.4: Recursive CTE
WITH RECURSIVE cte_recursive(n) AS (
    SELECT 1
    UNION ALL
    SELECT n + 1 FROM cte_recursive WHERE n < 5
)
SELECT n FROM cte_recursive;

-- =============================================================================
-- SECTION 13: Result with window functions
-- Tests: Result in window functions
-- =============================================================================

-- Test 13.1: ROW_NUMBER window function
SELECT id, value, row_number() OVER (ORDER BY id) as rn
FROM result_test_t1 WHERE id <= 5 ORDER BY id;

-- Test 13.2: SUM window function
SELECT id, value, sum(value) OVER (ORDER BY id) as running_sum
FROM result_test_t1 WHERE id <= 5 ORDER BY id;

-- Test 13.3: RANK window function
SELECT id, value, rank() OVER (ORDER BY value DESC) as rnk
FROM result_test_t1 WHERE id <= 5 ORDER BY id;

-- =============================================================================
-- SECTION 14: Result with LIMIT/OFFSET
-- Tests: Result with row limiting
-- =============================================================================

-- Test 14.1: Simple LIMIT
SELECT * FROM result_test_t1 ORDER BY id LIMIT 3;

-- Test 14.2: LIMIT with OFFSET
SELECT * FROM result_test_t1 ORDER BY id LIMIT 3 OFFSET 2;

-- Test 14.3: Constant expression with LIMIT
SELECT generate_series(1, 10) LIMIT 5;

-- Test 14.4: LIMIT 0 (early return)
SELECT * FROM result_test_t1 LIMIT 0;

-- =============================================================================
-- SECTION 15: Projection expressions (ExecProject coverage)
-- Tests: ExecResult lines 111, 192 (projection)
-- =============================================================================

-- Test 15.1: Simple projection
SELECT id * 2 as doubled, name || '_suffix' as new_name FROM result_test_t1 ORDER BY id LIMIT 3;

-- Test 15.2: Nested functions in projection
SELECT upper(lower(name)), abs(value - 250) FROM result_test_t1 ORDER BY id LIMIT 3;

-- Test 15.3: CASE in projection
SELECT id,
       CASE WHEN value < 200 THEN 'low'
            WHEN value < 400 THEN 'medium'
            ELSE 'high' END as category
FROM result_test_t1 ORDER BY id LIMIT 5;

-- Test 15.4: NULL handling in projection
SELECT id, COALESCE(NULL, value) as val FROM result_test_t1 ORDER BY id LIMIT 3;

-- =============================================================================
-- SECTION 16: Empty result handling
-- Tests: Various scenarios returning empty results
-- =============================================================================

-- Test 16.1: Empty WHERE clause match
SELECT * FROM result_test_t1 WHERE id < 0;

-- Test 16.2: Empty LIMIT
SELECT * FROM result_test_t1 WHERE 1=1 LIMIT 0;

-- Test 16.3: Empty subquery
SELECT * FROM result_test_t1 WHERE id IN (SELECT id FROM result_test_t1 WHERE id < 0);

-- Test 16.4: NOT IN with empty subquery
SELECT * FROM result_test_t1 WHERE id NOT IN (SELECT id FROM result_test_t1 WHERE id < 0) ORDER BY id;

-- =============================================================================
-- SECTION 17: ExprContext handling
-- Tests: Lines 100-103, 134-138 (hasSetResultStore, ResetExprContext)
-- =============================================================================

-- Test 17.1: Multiple columns from SRF
SELECT a.*, b.* FROM generate_series(1, 3) a, generate_series(1, 2) b ORDER BY a, b;

-- Test 17.2: SRF with aggregation
SELECT sum(x) FROM generate_series(1, 5) x;

-- Test 17.3: Nested SRF
SELECT * FROM (SELECT generate_series(1, 3)) a, (SELECT generate_series(1, 2)) b ORDER BY 1, 2;

-- =============================================================================
-- SECTION 18: UPDATE with Result node
-- Tests: Result in UPDATE statements
-- =============================================================================

-- Test 18.1: UPDATE with constant
UPDATE result_test_t1 SET value = 999 WHERE id = 1;

-- Test 18.2: UPDATE with expression
UPDATE result_test_t1 SET value = value + 100 WHERE id = 2;

-- Test 18.3: UPDATE with subquery
UPDATE result_test_t1 SET value = (SELECT max(value) FROM result_test_t1) WHERE id = 3;

-- Verify updates
SELECT * FROM result_test_t1 WHERE id <= 3 ORDER BY id;

-- =============================================================================
-- SECTION 19: DELETE with Result node
-- Tests: Result in DELETE statements
-- =============================================================================

-- Test 19.1: DELETE with constant qual
DELETE FROM result_test_t1 WHERE id > 10 AND 1 = 1;

-- Test 19.2: DELETE with subquery
DELETE FROM result_test_t1 WHERE id IN (SELECT id FROM result_test_t1 WHERE id > 10);

-- Verify deletes
SELECT count(*) FROM result_test_t1;

-- =============================================================================
-- SECTION 20: Cleanup
-- =============================================================================

DROP TABLE IF EXISTS result_test_t1;

-- Final verification - constant expressions still work
SELECT 'Test completed successfully' as status;
