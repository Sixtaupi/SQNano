-- Not expand-only (new index, data transformation): firmware v1 cannot open v2.
ALTER TABLE device ADD COLUMN status TEXT NOT NULL DEFAULT 'new';
CREATE INDEX device_name ON device(name);
CREATE TABLE event (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device INTEGER NOT NULL,
    kind TEXT
);
