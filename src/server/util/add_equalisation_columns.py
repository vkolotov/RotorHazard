"""Add the per-node equalisation columns to an existing RotorHazard database.

Idempotent. Uncalibrated profiles get NULL. The original integration
branch's eq_kups/eq_klos are converted without changing its output scale.
Keep FULL_RSSI_RESOLUTION enabled when upgrading that 12-bit deployment.

    python3 util/add_equalisation_columns.py <path-to-database.db>
"""
import json
import sqlite3
import sys

COLUMNS = (
    ("eq_pivots", "VARCHAR(256)"),
    ("eq_offset_ups", "VARCHAR(256)"),
    ("eq_slope_ups", "VARCHAR(256)"),
    ("eq_offset_los", "VARCHAR(256)"),
    ("eq_slope_los", "VARCHAR(256)"),
)


def main(db_path):
    conn = sqlite3.connect(db_path)
    try:
        existing = {row[1] for row in conn.execute("PRAGMA table_info(profiles)")}
        if not existing:
            print("ERROR: no 'profiles' table in {}".format(db_path))
            return 1
        added = []
        for name, coltype in COLUMNS:
            if name in existing:
                continue
            conn.execute("ALTER TABLE profiles ADD COLUMN {} {}".format(name, coltype))
            added.append(name)
        # The original integration used a fixed target of 300 and two slopes.
        # Convert once, retaining that output scale and all existing thresholds.
        # Keep the old columns intact so the backed-up deployment can be restored.
        if {'eq_kups', 'eq_klos'}.issubset(existing):
            for profile_id, raw_pivots, raw_ups, raw_los in conn.execute(
                    "SELECT id, eq_pivots, eq_kups, eq_klos FROM profiles "
                    "WHERE eq_offset_ups IS NULL AND eq_kups IS NOT NULL "
                    "AND eq_klos IS NOT NULL AND eq_pivots IS NOT NULL"):
                pivots = json.loads(raw_pivots)['v']
                ups, los = json.loads(raw_ups)['v'], json.loads(raw_los)['v']
                if len(pivots) != len(ups) or len(pivots) != len(los):
                    raise ValueError('Mismatched equalisation arrays in profile {}'.format(profile_id))
                def offsets(slopes):
                    return [int(round(pivot - 300 * 256.0 / slope)) if pivot else 0
                            for pivot, slope in zip(pivots, slopes)]
                converted = [offsets(ups), ups, offsets(los), los]
                if any(not -32768 <= v <= 32767 for vals in (converted[0], converted[2]) for v in vals):
                    raise ValueError('Equalisation offset out of range in profile {}'.format(profile_id))
                conn.execute(
                    "UPDATE profiles SET eq_offset_ups=?, eq_slope_ups=?, "
                    "eq_offset_los=?, eq_slope_los=? WHERE id=?",
                    [json.dumps({'v': vals}) for vals in converted] + [profile_id])
                conn.execute("UPDATE profiles SET eq_pivots=? WHERE id=?",
                             [json.dumps({'v': pivots, 'adc_bits': [12] * len(pivots)}), profile_id])
                print('converted legacy equalisation for profile {}'.format(profile_id))
        conn.commit()
        print("added: {}".format(", ".join(added)) if added else "nothing to do")
        return 0
    finally:
        conn.close()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
