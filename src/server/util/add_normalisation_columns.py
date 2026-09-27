"""Add the per-node normalisation columns to an existing RotorHazard database.

Idempotent. Existing profiles get NULL, which the server reads as "no
calibration", so behaviour is unchanged until one is applied.

    python3 util/add_normalisation_columns.py <path-to-database.db>

A database carrying the superseded eq_* columns is left holding them: SQLite
drops a column awkwardly, and they cost nothing but space. The fits they hold
are not carried over - they were made by a different transfer function, so
every timer calibrates once against the new one.
"""
import sqlite3
import sys

COLUMNS = (
    ("norm_pivots", "VARCHAR(256)"),
    ("norm_offsets", "VARCHAR(256)"),
    ("norm_scales", "VARCHAR(256)"),
    ("norm_per_freq", "VARCHAR(4096)"),
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
