"""Add the per-node equalisation columns to an existing RotorHazard database.

Idempotent. Existing profiles get NULL, which the server reads as "no
calibration", so behaviour is unchanged until one is applied.

    python3 util/add_equalisation_columns.py <path-to-database.db>
"""
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
