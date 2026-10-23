-- Lazy: the new column and the changes of v4_lazy.sql are computed when an
-- old row is read, so the migration itself rewrites no row.
ALTER TABLE device ADD COLUMN label TEXT;
