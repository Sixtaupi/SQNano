-- name: counter_bump
INSERT INTO counter (id, value) VALUES (:id, 1)
  ON CONFLICT (id) DO UPDATE SET value = value + 1;
-- name: counter_get
SELECT value FROM counter WHERE id = :id;
