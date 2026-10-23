-- Queries named o_* return rows in a defined order; the others are compared
-- as multisets.

-- name: ins_t
INSERT INTO t (id, a, b, c, d, e, f) VALUES (:id, :a, :b, :c, :d, :e, :f);
-- name: ins_t_auto
INSERT INTO t (a, b, c, d, e, f) VALUES (:a, :b, :c, :d, :e, :f);
-- name: rep_t
INSERT OR REPLACE INTO t (id, a, c) VALUES (:id, :a, :c);
-- name: ign_t
INSERT OR IGNORE INTO t (id, a, b) VALUES (:id, :a, :b);
-- name: ins_t_ret
INSERT INTO t (a, c) VALUES (:a, :c) RETURNING id, a, c, typeof(a);
-- name: ins_multi
INSERT INTO t (a, c, f) VALUES (:a, :c, :f), (:a + 1, :c || 'x', upper(:f));
-- name: upd_a
UPDATE t SET a = a + :d WHERE a BETWEEN :lo AND :hi;
-- name: upd_c
UPDATE t SET c = upper(c), f = :f WHERE id % :m = 0;
-- name: upd_id
UPDATE OR IGNORE t SET id = id + 1000 WHERE id > :x AND id < 1000;
-- name: upd_ret
UPDATE t SET b = b * 2 WHERE c = :c RETURNING id, b;
-- name: del_range
DELETE FROM t WHERE a > :lo AND a < :hi;
-- name: del_ret
DELETE FROM t WHERE id = :id RETURNING *;
-- name: o_all
SELECT * FROM t ORDER BY id;
-- name: sel_a_eq
SELECT id, a, typeof(a) FROM t WHERE a = :v;
-- name: sel_a_range
SELECT id, a FROM t WHERE a >= :lo AND a < :hi;
-- name: o_a_desc
SELECT id, a FROM t WHERE a > :lo ORDER BY a DESC, id DESC LIMIT :n;
-- name: sel_c_eq
SELECT id, c, f FROM t WHERE c = :c;
-- name: sel_cf
SELECT id FROM t WHERE c = :c AND f = :f;
-- name: sel_f_nocase
SELECT id, f FROM t WHERE f = :f;
-- name: sel_like
SELECT id FROM t WHERE c LIKE :p;
-- name: sel_glob
SELECT id FROM t WHERE c GLOB :p;
-- name: sel_expr
SELECT id, a + b, a * 2 - 1, a / 3, a % 7, c || '-' || a, -a, abs(a), b / 0, a & 6, a | 1, a << 2, a >> 1 FROM t WHERE id <= :n;
-- name: sel_case
SELECT id, CASE WHEN a IS NULL THEN 'null' WHEN a < 0 THEN 'neg' ELSE 'pos' END, CASE a % 3 WHEN 0 THEN 'z' WHEN 1 THEN 'o' END, coalesce(c, f, 'none'), ifnull(b, -1), nullif(a, 0) FROM t;
-- name: sel_cast
SELECT id, CAST(c AS INTEGER), CAST(c AS REAL), CAST(a AS TEXT), CAST(b AS INTEGER), CAST(c AS NUMERIC), typeof(CAST(d AS TEXT)) FROM t;
-- name: sel_cmp_text
SELECT id FROM t WHERE c > :v;
-- name: sel_cmp_d
SELECT id, typeof(d) FROM t WHERE d = :v;
-- name: sel_e
SELECT id, e, typeof(e) FROM t WHERE e < :v;
-- name: sel_e_eq
SELECT id FROM t WHERE e = :v;
-- name: sel_in
SELECT id FROM t WHERE a IN (1, 2, :v, NULL) OR c IN ('a', :w);
-- name: sel_not_in
SELECT id FROM t WHERE a NOT IN (1, 2, 3) AND c NOT LIKE '%a%';
-- name: sel_null
SELECT id FROM t WHERE b IS NULL AND c IS NOT NULL;
-- name: sel_is
SELECT id FROM t WHERE a IS :v;
-- name: sel_fn
SELECT id, length(c), lower(c), upper(f), substr(c, :s, :l), round(b, 2), round(b), trim(c), instr(c, 'a'), replace(c, 'a', 'XY'), hex(c), typeof(b), min(a, id), max(a, b, id), sign(a) FROM t;
-- name: sel_logic
SELECT id, a > 0 AND b > 0, a > 0 OR b > 0, NOT (a > 0), a > 0 AND NULL, b > 0 OR NULL FROM t;
-- name: agg_all
SELECT count(*), count(a), sum(a), total(b), avg(b), min(c), max(c), min(b), max(a) FROM t;
-- name: agg_where
SELECT count(*), sum(b) FROM t WHERE a > :v;
-- name: grp_a
SELECT a % 5, count(*), sum(b), max(c) FROM t GROUP BY a % 5;
-- name: o_grp_having
SELECT c, count(*) AS n FROM t GROUP BY c HAVING count(*) > :cnt ORDER BY n DESC, c;
-- name: o_grp_order
SELECT lower(f), avg(a), count(*) FROM t WHERE f IS NOT NULL GROUP BY lower(f) ORDER BY 1 LIMIT :n;
-- name: dist
SELECT DISTINCT a % 4, c FROM t;
-- name: o_dist_order
SELECT DISTINCT c FROM t WHERE c IS NOT NULL ORDER BY c;
-- name: o_limit
SELECT id, a FROM t ORDER BY b DESC, id LIMIT :n OFFSET :o;
-- name: o_sort_expr
SELECT id, a * b AS p FROM t ORDER BY p, id LIMIT 20;
-- name: rowid_range
SELECT id FROM t WHERE id > :lo AND id <= :hi;
-- name: o_rowid_desc
SELECT id FROM t WHERE id < :hi ORDER BY id DESC LIMIT 5;
-- name: o_index_order
SELECT id, c, f FROM t WHERE c >= :c ORDER BY c, f DESC, id LIMIT 10;
-- name: noref
SELECT :x + 1, :x || 'z', typeof(:x), 7 / 2, 7.0 / 2, 1 = 1.0, 'abc' < 'abd', NULL IS NULL;
-- name: u_put
INSERT INTO u (k, v, w) VALUES (:k, :v, :w) ON CONFLICT (k) DO UPDATE SET v = v + excluded.v, w = excluded.w;
-- name: u_put_nothing
INSERT INTO u (k, v) VALUES (:k, :v) ON CONFLICT DO NOTHING;
-- name: u_ins
INSERT INTO u (k, v, w) VALUES (:k, :v, :w);
-- name: o_u_all
SELECT k, v, w FROM u ORDER BY k;
-- name: u_del
DELETE FROM u WHERE v < :v;
-- name: o_grp_idx
SELECT a, count(*), sum(b), max(c) FROM t WHERE a > :lo GROUP BY a ORDER BY a;
-- name: grp_e
SELECT e, count(*), min(id) FROM t GROUP BY e HAVING count(*) >= :cnt;
-- name: o_grp_idx_desc
SELECT c, count(*) FROM t GROUP BY c ORDER BY c DESC LIMIT :n;
-- name: upd_fail
UPDATE OR FAIL u SET v = v + 1, w = w - :w;
-- name: upd_abort
UPDATE u SET v = v + 1, w = w - :w;
-- name: ins_multi_fail
INSERT OR FAIL INTO u (k, v) VALUES (:k, :v), ('k1', 1), ('k9', 2);
-- name: ins_multi_abort
INSERT INTO u (k, v) VALUES ('k8', :v), (:k, :v), ('k2', 3);
-- name: ins_rollback
INSERT OR ROLLBACK INTO u (k, v) VALUES (:k, :v);
-- name: j_inner
SELECT t.id, t.c, u.v FROM t JOIN u ON u.k = t.c WHERE t.a > :lo;
-- name: j_left
SELECT t.id, u.k, u.v FROM t LEFT JOIN u ON u.k = t.c AND u.v > :x;
-- name: j_left_where
SELECT t.id, u.v FROM t LEFT JOIN u ON u.k = t.c WHERE u.v IS NULL;
-- name: j_using
SELECT * FROM u JOIN u AS u2 USING (k) WHERE u.v < :v;
-- name: j_comma
SELECT t.id, u.k FROM t, u WHERE t.a = u.v AND t.a > :lo;
-- name: j_nocase
SELECT t.id, u.k FROM t JOIN u ON t.f = u.k;
-- name: o_j_order
SELECT t.id, u.v FROM t LEFT JOIN u ON u.k = t.c ORDER BY t.id LIMIT :n;
-- name: j_agg
SELECT u.k, count(t.id), sum(t.a) FROM u LEFT JOIN t ON t.c = u.k GROUP BY u.k;
-- name: o_j_grp_order
SELECT t.a, count(*) FROM t JOIN u ON u.k = t.c GROUP BY t.a ORDER BY 2 DESC, 1;
-- name: j_natural
SELECT * FROM u NATURAL JOIN u AS u2 WHERE u.v > :v;
-- name: j_three
SELECT a.id, b.id, u.k FROM t AS a JOIN t AS b ON b.a = a.a JOIN u ON u.v = b.a WHERE a.a > :lo;
-- name: j_star_table
SELECT u.*, t.id FROM u JOIN t ON t.a = u.v;
-- name: j_distinct
SELECT DISTINCT u.k FROM u JOIN t ON t.c = u.k;
-- name: j_left_count
SELECT count(*), count(u.k) FROM t LEFT JOIN u ON u.v = t.a;
-- name: s_scalar
SELECT id, (SELECT max(v) FROM u) FROM t WHERE id < :id;
-- name: s_corr
SELECT id, (SELECT v FROM u WHERE u.k = t.c) FROM t;
-- name: s_corr_agg
SELECT k, (SELECT count(*) FROM t WHERE t.c = u.k) FROM u;
-- name: s_in
SELECT id FROM t WHERE c IN (SELECT k FROM u WHERE v > :v);
-- name: s_not_in
SELECT id FROM t WHERE a NOT IN (SELECT v FROM u);
-- name: s_in_null
SELECT id, a IN (SELECT w FROM u), a NOT IN (SELECT w FROM u WHERE v > :v) FROM t;
-- name: s_exists
SELECT k FROM u WHERE EXISTS (SELECT 1 FROM t WHERE t.c = u.k AND t.a > :x);
-- name: s_not_exists
SELECT k FROM u WHERE NOT EXISTS (SELECT 1 FROM t WHERE t.c = u.k);
-- name: s_where_cmp
SELECT id FROM t WHERE a > (SELECT avg(a) FROM t);
-- name: s_order_limit
SELECT k, (SELECT id FROM t WHERE t.c = u.k ORDER BY id DESC LIMIT 1) FROM u;
-- name: s_top1
SELECT k, (SELECT a FROM t WHERE t.c = u.k ORDER BY b DESC, id) FROM u;
-- name: s_in_limit
SELECT id FROM t WHERE id IN (SELECT id FROM t ORDER BY id DESC LIMIT :n);
-- name: s_affinity
SELECT id FROM t WHERE c = (SELECT v FROM u WHERE k = :k);
-- name: s_coll
SELECT id FROM t WHERE f = (SELECT k FROM u WHERE k = :k);
-- name: s_nested
SELECT k FROM u WHERE v IN (SELECT a FROM t WHERE EXISTS (SELECT 1 FROM u AS u2 WHERE u2.v = t.a));
-- name: s_noform
SELECT (SELECT 1), (SELECT :x), EXISTS (SELECT 1 WHERE 0), :x IN (SELECT 1);
-- name: del_sub
DELETE FROM t WHERE a > (SELECT avg(a) FROM t) + :d;
-- name: del_in_self
DELETE FROM t WHERE a IN (SELECT a + 1 FROM t WHERE id < :id);
-- name: upd_sub
UPDATE u SET v = (SELECT count(*) FROM t WHERE t.c = u.k) WHERE k = :k;
-- name: upd_self
UPDATE t SET b = (SELECT max(b) FROM t) WHERE id = :id;
-- name: ins_sub
INSERT INTO u (k, v) VALUES (:k, (SELECT count(*) FROM t));
-- name: ins_sub_self
INSERT INTO t (a, c) VALUES ((SELECT max(a) FROM t) + 1, :c);
-- name: p_ins
INSERT INTO p (id, name, grp) VALUES (:pk, :name, :grp);
-- name: p_del
DELETE FROM p WHERE id = :pk;
-- name: p_del_grp
DELETE FROM p WHERE grp = :grp;
-- name: p_upd_id
UPDATE p SET id = id + 20 WHERE id = :pk;
-- name: p_upd_name
UPDATE p SET name = :name WHERE id = :pk;
-- name: p_rep
INSERT OR REPLACE INTO p (id, name, grp) VALUES (:pk, :name, :grp);
-- name: ch_ins
INSERT INTO ch (pid, pname, note) VALUES (:pid, :name, :note);
-- name: ch_upd
UPDATE ch SET pid = :pid WHERE id % 4 = :grp;
-- name: ch_del
DELETE FROM ch WHERE pid = :pid;
-- name: node_ins
INSERT INTO node (id, parent, v) VALUES (:pk, :pid, :grp);
-- name: node_del
DELETE FROM node WHERE id = :pk;
-- name: node_del_v
DELETE FROM node WHERE v > :grp;
-- name: na_ins
INSERT INTO na (pid) VALUES (:pid);
-- name: na_del
DELETE FROM na WHERE pid = :pid;
-- name: rs_ins
INSERT INTO rs (pid) VALUES (:pid);
-- name: rs_del
DELETE FROM rs WHERE pid = :pid;
-- name: df_ins
INSERT INTO df (pid) VALUES (:pid);
-- name: df_fix
DELETE FROM df WHERE pid NOT IN (SELECT id FROM p);
-- name: sd_ins
INSERT INTO sd (pid) VALUES (:pid);
-- name: o_p_all
SELECT id, name, grp FROM p ORDER BY id;
-- name: o_ch_all
SELECT id, pid, pname, note FROM ch ORDER BY id;
-- name: o_node_all
SELECT id, parent, v FROM node ORDER BY id;
-- name: o_na_all
SELECT id, pid FROM na ORDER BY id;
-- name: o_rs_all
SELECT id, pid FROM rs ORDER BY id;
-- name: o_df_all
SELECT id, pid FROM df ORDER BY id;
-- name: o_sd_all
SELECT id, pid FROM sd ORDER BY id;
-- name: is_into_u
INSERT INTO u (k, v, w) SELECT c, a, b FROM t WHERE id = :id;
-- name: is_self
INSERT INTO t (a, c, f) SELECT a + 1, c, f FROM t WHERE id < :id ORDER BY id DESC LIMIT 3;
-- name: is_ignore
INSERT OR IGNORE INTO u (k, v) SELECT k || 'x', v FROM u WHERE length(k) < 3;
-- name: is_agg_upsert
INSERT INTO u (k, v) SELECT 'g' || a, count(*) FROM t WHERE a < :lo GROUP BY a ON CONFLICT (k) DO UPDATE SET v = v + excluded.v;
-- name: is_join
INSERT OR IGNORE INTO p (name, grp) SELECT DISTINCT u.k, 1 FROM u JOIN t ON t.c = u.k;
-- name: is_ret
INSERT INTO u (k, v) SELECT 'r' || id, a FROM t WHERE id = :id RETURNING k, v;
-- name: c_union_all
SELECT a, c FROM t WHERE a < :lo UNION ALL SELECT v, k FROM u;
-- name: c_union
SELECT a FROM t UNION SELECT v FROM u;
-- name: o_c_union_order
SELECT c AS x, a FROM t WHERE id < :id UNION SELECT k, v FROM u ORDER BY x DESC, 2 LIMIT :n;
-- name: c_intersect
SELECT c FROM t INTERSECT SELECT k FROM u;
-- name: c_except_ci
SELECT f FROM t EXCEPT SELECT k FROM u;
-- name: c_chain
SELECT a FROM t WHERE id < :id UNION ALL SELECT v FROM u UNION SELECT a + 1 FROM t EXCEPT SELECT :x;
-- name: o_c_all_limit
SELECT id, a FROM t UNION ALL SELECT 0, v FROM u ORDER BY 1, 2 LIMIT :n OFFSET 2;
-- name: c_limit
SELECT v FROM u WHERE v < 0 UNION ALL SELECT b FROM t LIMIT :n;
-- name: c_nocase_ci
SELECT f FROM t UNION SELECT name FROM p;
-- name: c_grp
SELECT a, count(*) FROM t GROUP BY a UNION SELECT v, 1 FROM u;
-- name: is_union
INSERT OR IGNORE INTO u (k, v) SELECT c, a FROM t WHERE id = :id UNION SELECT k || 'u', v FROM u WHERE v > :v;
-- Large sorts: products of t with itself, beyond a small sort buffer.
-- name: o_xsort
SELECT x.id, y.id, x.c, y.a FROM t x CROSS JOIN t y WHERE x.id <= :hi + 20 ORDER BY x.c DESC, y.a, x.id, y.id;
-- name: o_xsort_long
SELECT x.id, y.id, x.d FROM t x CROSS JOIN t y WHERE x.id < :id AND y.id < 40 ORDER BY typeof(x.d), length(x.d), x.id DESC, y.id;
-- name: o_xsort_limit
SELECT x.a + y.a AS s, x.id, y.id FROM t x CROSS JOIN t y ORDER BY s, x.id, y.id LIMIT abs(:n) + 2 OFFSET 3;
-- name: xgroup
SELECT x.a % 5, y.c, count(*), sum(y.a), max(x.c), min(y.f) FROM t x CROSS JOIN t y WHERE y.id <= :hi + 30 GROUP BY x.a % 5, y.c;
-- name: o_xgroup_ord
SELECT y.c, count(*) AS n, total(x.b) FROM t x CROSS JOIN t y GROUP BY y.c ORDER BY n DESC, y.c;
-- name: xdistinct
-- (x.a + y.a could make an integer and a real that are equal; which one
-- DISTINCT keeps depends on the scan order.)
SELECT DISTINCT x.a + y.id, x.c FROM t x CROSS JOIN t y WHERE x.id <= :hi + 20;
-- name: xunion
SELECT x.c, y.a FROM t x CROSS JOIN t y WHERE x.id <= :hi UNION SELECT c, a FROM t;
-- name: xexcept
SELECT x.a, y.c FROM t x CROSS JOIN t y WHERE x.id < 70 EXCEPT SELECT a, c FROM t WHERE id < :id;
-- name: xintersect_ci
SELECT x.f, y.a FROM t x CROSS JOIN t y WHERE y.id < 50 INTERSECT SELECT y.f, x.a FROM t x CROSS JOIN t y WHERE x.id > :lo AND x.id < 60;

-- WITHOUT ROWID tables.
-- name: wr_ins
INSERT INTO wr (g, n, v, w) VALUES (:k, :s, :b, :c);
-- name: wr_rep
INSERT OR REPLACE INTO wr (g, n, v, w) VALUES (:k, :s, :b, :c);
-- name: wr_ign
INSERT OR IGNORE INTO wr (g, n, v) VALUES (:k, :s, :b);
-- name: wr_upsert
INSERT INTO wr (g, n, v) VALUES (:k, :s, :b)
    ON CONFLICT (g, n) DO UPDATE SET v = coalesce(v, 0) + coalesce(excluded.v, 1);
-- name: wr_upd
UPDATE wr SET v = v + 1, w = :c WHERE g = :k;
-- name: wr_upd_key
UPDATE OR IGNORE wr SET n = n + 1 WHERE g = :k AND n > :s;
-- name: wr_upd_case
UPDATE wr SET g = upper(g) WHERE n = :s RETURNING g, n;
-- name: wr_del
DELETE FROM wr WHERE g = :k AND n < :s;
-- name: wr_del_v
DELETE FROM wr WHERE v > :x RETURNING g, n;
-- name: wr_scan
SELECT * FROM wr;
-- name: o_wr_pk
SELECT n, v FROM wr WHERE g = :k ORDER BY n DESC;
-- name: o_wr_order
SELECT g, n FROM wr ORDER BY g, n DESC;
-- name: wr_range
SELECT g, n FROM wr WHERE g = :k AND n BETWEEN :lo AND :hi;
-- name: wr_by_v
SELECT g, n, v FROM wr WHERE v > :x;
-- name: wr_by_w
SELECT g, n FROM wr WHERE w = :c;
-- name: wr_group
SELECT lower(g), count(*), sum(v) FROM wr GROUP BY g;
-- name: wr_join
SELECT t.id, wr.n FROM t JOIN wr ON wr.n = t.a WHERE wr.g = :k;
-- name: wp_ins
INSERT OR IGNORE INTO wp (code, label) VALUES (:k, :c);
-- name: wp_upd
UPDATE wp SET code = code || 'x' WHERE code = :k;
-- name: wp_del
DELETE FROM wp WHERE code = :k;
-- name: wc_ins
INSERT INTO wc (code, seq, x) VALUES (:k, :s, :a);
-- name: wc_upd
UPDATE wc SET seq = seq + 10 WHERE code = :k;
-- name: wc_all
SELECT * FROM wc;
-- name: wc_by_x
SELECT code, seq FROM wc WHERE x = :a;
-- name: o_wc_x
SELECT x, code, seq FROM wc WHERE x > :lo ORDER BY x DESC, code, seq;
