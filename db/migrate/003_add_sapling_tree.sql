-- Migration 003: Add Sapling commitment tree tracking
-- Stores serialized tree state per block for disconnect support,
-- and per-note witness data for spend proof construction.

ALTER TABLE blocks ADD COLUMN sapling_tree_data BLOB;
ALTER TABLE wallet_sapling_notes ADD COLUMN witness_data BLOB;
ALTER TABLE wallet_sapling_notes ADD COLUMN witness_height INTEGER DEFAULT 0;
