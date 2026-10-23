-- Initial schema.
CREATE TABLE sensor (
    rid INTEGER PRIMARY KEY,
    id INTEGER NOT NULL,
    ts INTEGER NOT NULL,
    value REAL,
    unit TEXT DEFAULT 'C'
);
CREATE INDEX sensor_pk ON sensor(id, ts);
CREATE TABLE config (
    key TEXT PRIMARY KEY COLLATE NOCASE,
    val TEXT,
    CHECK (length(key) > 0)
);
-- Device events, keyed by device and newest sequence first.
CREATE TABLE event (
    dev TEXT,
    seq INTEGER,
    kind TEXT NOT NULL,
    PRIMARY KEY (dev, seq DESC)
) WITHOUT ROWID;
