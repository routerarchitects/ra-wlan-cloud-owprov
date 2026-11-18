-- Migration: 001_inventory_serial_canonical_unique_idx.sql
-- Description: Create canonical case-insensitive unique index on inventory (lower(serialnumber))
-- Execution Prerequisite: Run once before deploying updated OWPROV instances.
--                         Must execute OUTSIDE an explicit transaction block (CONCURRENTLY requirement).

-- 1. Audit check: detect if any duplicate canonical serial numbers exist before indexing
DO $$
DECLARE
    dup_count INT;
BEGIN
    SELECT COUNT(*) INTO dup_count
    FROM (
        SELECT lower(serialnumber)
        FROM inventory
        WHERE serialnumber IS NOT NULL
        GROUP BY lower(serialnumber)
        HAVING COUNT(*) > 1
    ) sub;

    IF dup_count > 0 THEN
        RAISE EXCEPTION 'Cannot build unique index: % duplicate canonical serial number groups detected in inventory table. Please deduplicate records before applying this migration.', dup_count;
    END IF;
END $$;

-- 2. Controlled non-blocking index build outside transaction
CREATE UNIQUE INDEX CONCURRENTLY IF NOT EXISTS inventory_serial_canonical_unique_idx
ON inventory (lower(serialnumber));

-- 3. Post-verification: ensure the index exists and is marked valid by PostgreSQL
DO $$
DECLARE
    idx_valid BOOLEAN;
BEGIN
    SELECT i.indisvalid INTO idx_valid
    FROM pg_index i
    JOIN pg_class c ON c.oid = i.indexrelid
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE c.relname = 'inventory_serial_canonical_unique_idx'
      AND n.nspname = CURRENT_SCHEMA();

    IF idx_valid IS NOT TRUE THEN
        RAISE EXCEPTION 'Migration failed: inventory_serial_canonical_unique_idx was not created or is invalid.';
    END IF;
END $$;
