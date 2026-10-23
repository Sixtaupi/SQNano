-- Queries of the application. Each named query also gets a typed C API
-- (q_<name>_open/bind/step) in the generated sqnano_gen.h.

-- name: sensor_last
SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts DESC LIMIT :n;

-- name: sensor_stats
SELECT id, count(*), min(value), max(value), avg(value) FROM sensor GROUP BY id;

-- name: sensor_purge
DELETE FROM sensor WHERE ts < :before;
