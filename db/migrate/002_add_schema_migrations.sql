-- Migration 002: Add schema_migrations tracking table
-- Enables versioned migrations like Rails db:migrate

CREATE TABLE IF NOT EXISTS schema_migrations (
    version TEXT PRIMARY KEY,
    applied_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

INSERT OR IGNORE INTO schema_migrations(version) VALUES('001');
INSERT OR IGNORE INTO schema_migrations(version) VALUES('002');
