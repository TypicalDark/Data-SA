-- Grid of SA data store, migration 2: newer metro stands and extra value sources.
-- Apply after 001_core.sql:  psql -d gsa -f schema/002_metro_values.sql      (idempotent)
--
-- core.parcel.stand          the city's own stand label (Joburg "RE/54/239-IQ"); shown instead of "Erf n ptn m"
-- core.parcel.superseded_by  set on an older SG parcel whose centre lies inside a newer metro stand with another
--                            SG code (stands are split and merged after the 2022 SG copy). The row is kept, never
--                            deleted; exports leave superseded parcels out so the map shows each plot once.
-- core.valuation.zoning      zoning from rolls that print it (Joburg GV2023)
-- core.valuation.ext_ref     the source's own property key (Joburg property_id), for lookups back at the source
-- value_flag 'superseded'    a lower-priority value for a plot another roll already values (KZN COGTA layer rows
--                            for plots a municipal roll covers). Kept for the plot card; not summed in roll-ups.
ALTER TABLE core.parcel    ADD COLUMN IF NOT EXISTS stand text;
ALTER TABLE core.parcel    ADD COLUMN IF NOT EXISTS superseded_by char(21);
ALTER TABLE core.valuation ADD COLUMN IF NOT EXISTS zoning text;
ALTER TABLE core.valuation ADD COLUMN IF NOT EXISTS ext_ref text;
CREATE INDEX IF NOT EXISTS valuation_roll ON core.valuation (roll);
CREATE INDEX IF NOT EXISTS parcel_muni ON core.parcel (muni_id);
-- Plots the exports draw: not superseded by a newer stand.
CREATE OR REPLACE VIEW core.parcel_live AS SELECT * FROM core.parcel WHERE superseded_by IS NULL;
-- Values that count: not zero placeholders, not superseded by a higher-priority roll.
CREATE OR REPLACE VIEW core.valuation_live AS
  SELECT * FROM core.valuation WHERE coalesce(value_flag, '') NOT IN ('zero', 'superseded');
