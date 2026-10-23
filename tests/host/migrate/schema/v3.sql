-- Expand-only: firmware v2 keeps working on a v3 database.
ALTER TABLE device ADD COLUMN note TEXT;
