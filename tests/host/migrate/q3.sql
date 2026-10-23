-- name: dev_note
UPDATE device SET note = :note WHERE id = :id;
-- name: dev_get_note
SELECT note FROM device WHERE id = :id;
