-- name: dev_lazy
SELECT label, status, note FROM device WHERE id = :id;
