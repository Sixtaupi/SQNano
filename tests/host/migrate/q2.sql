-- name: dev_by_name
SELECT id, status FROM device WHERE name = :name;
-- name: status_count
SELECT status, count(*) FROM device GROUP BY status ORDER BY status;
-- name: ev_count
SELECT count(*) FROM event;
