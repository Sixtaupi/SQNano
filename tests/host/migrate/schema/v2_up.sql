UPDATE device SET status = 'migrated' WHERE fw < 3;
INSERT INTO event (device, kind) VALUES (0, 'migration');
