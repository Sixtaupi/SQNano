-- Sensor log schema, version 1.
CREATE TABLE sensor (
    id INTEGER NOT NULL,
    ts INTEGER NOT NULL,
    value REAL
);
CREATE INDEX sensor_pk ON sensor(id, ts);

CREATE TABLE config (
    key TEXT PRIMARY KEY COLLATE NOCASE,
    val TEXT
);
