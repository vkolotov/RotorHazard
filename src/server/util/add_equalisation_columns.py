"""Add the RSSI-equalisation columns to an existing RotorHazard database.

Idempotent: safe to run repeatedly. Existing profiles get NULL, which the
server reads as the identity values (offset 0, scale 256), so behaviour is
unchanged until values are set.

    python3 util/add_equalisation_columns.py <path-to-database.db>
"""
import sqlite3
import sys

COLUMNS = (
    ("floor_offsets", "VARCHAR(256)"),
    ("scale_factors", "VARCHAR(256)"),
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
                print("already present: {}".format(name))
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
