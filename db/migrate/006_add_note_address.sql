-- Copyright 2026 Rhett Creighton - Apache License 2.0
-- Migration 006: Add address column to wallet_sapling_notes.
-- Enables per-order payment matching by bech32 z-address.

ALTER TABLE wallet_sapling_notes ADD COLUMN address TEXT;
CREATE INDEX IF NOT EXISTS idx_snote_address ON wallet_sapling_notes(address) WHERE spent_txid IS NULL;
