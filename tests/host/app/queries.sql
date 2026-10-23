-- name: sensor_last
SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts DESC LIMIT :n;

-- name: sensor_add
INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value);

-- name: sensor_stats
SELECT id, count(*), min(value), max(value), avg(value) FROM sensor GROUP BY id ORDER BY id;

-- name: config_set
INSERT INTO config (key, val) VALUES (:key, :val)
  ON CONFLICT (key) DO UPDATE SET val = excluded.val;

-- name: config_get
SELECT val FROM config WHERE key = :key;

-- name: sensor_purge
DELETE FROM sensor WHERE ts < :before;

-- name: event_add
INSERT INTO event (dev, seq, kind) VALUES (:dev, :seq, :kind);
