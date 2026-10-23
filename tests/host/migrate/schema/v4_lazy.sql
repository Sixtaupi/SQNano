UPDATE device SET label = upper(name) || '-' || coalesce(fw, 0);
UPDATE device SET status = 'fw' || coalesce(fw, 0) WHERE status = 'new';
-- Not idempotent: a row converted twice would show it.
UPDATE device SET note = coalesce(note, '') || '!' WHERE fw > 3;
