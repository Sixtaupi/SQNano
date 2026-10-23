-- name: dev_add
INSERT INTO device (id, name, fw) VALUES (:id, :name, :fw);
-- name: dev_count
SELECT count(*) FROM device;
-- name: dev_get
SELECT name, fw FROM device WHERE id = :id;
-- name: dev_set_fw
UPDATE device SET fw = :fw WHERE id = :id;
