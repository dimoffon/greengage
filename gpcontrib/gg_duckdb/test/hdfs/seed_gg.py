"""Iceberg tables in the Hive Metastore for the gg_duckdb HDFS checks.

    spark3-submit seed_gg.py <warehouse_uri>

Creates the database `gg` with two tables and prints the reference aggregates
run.sh compares against, one per line, prefixed with GGREF:

  events      partitioned by day, written in two snapshots, copy-on-write.
              The second snapshot exists so that a reader that stops at the
              first metadata file is caught.
  events_mor  the same rows, then a merge-on-read DELETE, so the table carries
              positional delete files: a reader that lists data files and
              reads them directly would return the deleted rows, which is
              exactly what the file-sharding path must refuse to do.
"""
import sys

from pyspark.sql import SparkSession

WAREHOUSE = sys.argv[1]
DB = "gg"
CAT = "spark_catalog"

spark = (SparkSession.builder
         .appName("gg-duckdb-hdfs-seed")
         .config("spark.sql.extensions",
                 "org.apache.iceberg.spark.extensions.IcebergSparkSessionExtensions")
         .config("spark.sql.catalog.ib", "org.apache.iceberg.spark.SparkCatalog")
         .config("spark.sql.catalog.ib.type", "hive")
         .enableHiveSupport()
         .getOrCreate())

spark.sql(f"DROP DATABASE IF EXISTS {CAT}.{DB} CASCADE")
spark.sql(f"CREATE DATABASE {CAT}.{DB} LOCATION '{WAREHOUSE}/{DB}.db'")


def create(table, mode):
    spark.sql(f"""
        CREATE TABLE {CAT}.{DB}.{table} (
            id       bigint,
            day      date,
            region   string,
            amount   decimal(12,2),
            note     string)
        USING iceberg
        PARTITIONED BY (day)
        TBLPROPERTIES (
            'format-version' = '2',
            'write.delete.mode' = '{mode}',
            'write.update.mode' = '{mode}')""")
    # 2000 rows over 4 days and 3 regions; the amounts are exact decimals, so
    # both engines must agree on the sum to the cent.
    spark.sql(f"""
        INSERT INTO {CAT}.{DB}.{table}
        SELECT id,
               date_add(date '2026-01-01', CAST(id % 4 AS int)),
               element_at(array('east','west','north'), CAST(id % 3 AS int) + 1),
               CAST(id % 977 AS decimal(12,2)) / 100,
               concat('note-', CAST(id AS string))
        FROM range(0, 2000)""")


create("events", "copy-on-write")
# a second snapshot: the same shape, later ids
spark.sql(f"""
    INSERT INTO {CAT}.{DB}.events
    SELECT id,
           date_add(date '2026-01-01', CAST(id % 4 AS int)),
           element_at(array('east','west','north'), CAST(id % 3 AS int) + 1),
           CAST(id % 977 AS decimal(12,2)) / 100,
           concat('note-', CAST(id AS string))
    FROM range(2000, 2500)""")

create("events_mor", "merge-on-read")
spark.sql(f"DELETE FROM {CAT}.{DB}.events_mor WHERE id % 7 = 0")

for table in ("events", "events_mor"):
    row = spark.sql(f"""
        SELECT count(*) AS n,
               CAST(sum(amount) AS string) AS amount,
               count(DISTINCT day) AS days,
               CAST(min(id) AS string) AS lo,
               CAST(max(id) AS string) AS hi
        FROM {CAT}.{DB}.{table}""").collect()[0]
    print(f"GGREF {DB}.{table} n={row['n']} amount={row['amount']} "
          f"days={row['days']} lo={row['lo']} hi={row['hi']}")
    for part in spark.sql(f"""
            SELECT CAST(day AS string) AS day, count(*) AS n
            FROM {CAT}.{DB}.{table} GROUP BY day ORDER BY day""").collect():
        print(f"GGREF {DB}.{table} day={part['day']} n={part['n']}")

spark.stop()
