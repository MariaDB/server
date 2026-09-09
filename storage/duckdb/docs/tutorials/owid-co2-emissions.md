# MariaDB DuckDB Tutorial: OWID CO₂ Emissions

## Overview

This tutorial loads the [Our World in Data CO₂ and Greenhouse Gas Emissions dataset][owid-data]
into an `ENGINE=DuckDB` table and analyzes it through the regular `mariadb` client.

OWID publishes its work under [Creative Commons BY 4.0][owid-license] and notes that data from
third-party providers remains subject to the providers' license terms.

## Connect to MariaDB

The DuckDB plugin must be loaded and `run_in_duckdb()` must be enabled:

```ini
[mysqld]
plugin-maturity=gamma
plugin-load-add=ha_duckdb.so
duckdb-allow-run-in-duckdb=ON
```

See the [DuckDB storage engine README](../../README.md#building) for build and installation
instructions.

> **Note:** `run_in_duckdb()` is OFF by default and requires the `SUPER` privilege. It executes
> arbitrary DuckDB SQL in-process as the server OS user, and DuckDB applies no access control of its
> own. Enable it only on a host you control, and read
> [`security-model.md`](../security-model.md) before using it in a shared or multi-tenant
> deployment.

Start the client with a UTF-8 character set:

```sh
mariadb --default-character-set=utf8mb4
```

Check that the engine and `run_in_duckdb()` are available:

```sql
SHOW ENGINES;
SELECT @@duckdb_allow_run_in_duckdb;
```

`SHOW ENGINES` should list `DUCKDB` with `Support` set to `YES`. The variable should be `1`.

## Download the Data Set

Download a copy pinned to a Git commit so that the results below remain reproducible:

```sh
curl --fail --location -o /tmp/owid-co2-data.csv \
  https://raw.githubusercontent.com/owid/co2-data/382ee6c662b0ece26e111f263b44c029afad7787/owid-co2-data.csv
```

Run this command on the MariaDB server host and make sure the account running `mariadbd` can read
the file.

## Create a Table

Create a database and a DuckDB table for a subset of the CSV columns:

```sql
CREATE DATABASE IF NOT EXISTS owid;
USE owid;

CREATE TABLE co2_emissions (
  country                       VARCHAR(60) NOT NULL,
  year                          INT NOT NULL,
  iso_code                      VARCHAR(10),
  population                    BIGINT,
  gdp                           DOUBLE,
  co2                           DOUBLE,
  co2_growth_prct               DOUBLE,
  co2_per_capita                DOUBLE,
  coal_co2                      DOUBLE,
  gas_co2                       DOUBLE,
  oil_co2                       DOUBLE,
  cement_co2                    DOUBLE,
  cumulative_co2                DOUBLE,
  share_global_co2              DOUBLE,
  share_global_cumulative_co2   DOUBLE,
  temperature_change_from_co2   DOUBLE,
  PRIMARY KEY (country, year)
) ENGINE=DuckDB DEFAULT CHARSET=utf8mb4;
```

With the default `duckdb_require_primary_key=ON`, the table needs a primary key. DuckDB table
columns accept `utf8`, `utf8mb3`, `utf8mb4`, and `ascii`; this example uses `utf8mb4`.
`VARCHAR(60)` keeps the `country` key part below MariaDB's 255-byte key length limit (60 × 4 bytes).

## Add the Data Set

`run_in_duckdb()` sends the SQL string to DuckDB's vectorized engine for execution and returns DuckDB's own textual rendering of the result as a single string value. Use DuckDB's CSV reader to load the file inside the server process:

```sql
SELECT run_in_duckdb('INSERT INTO owid.co2_emissions
  (country, year, iso_code, population, gdp, co2, co2_growth_prct,
   co2_per_capita, coal_co2, gas_co2, oil_co2, cement_co2,
   cumulative_co2, share_global_co2, share_global_cumulative_co2,
   temperature_change_from_co2)
SELECT
  country, year, iso_code, population, gdp, co2, co2_growth_prct,
  co2_per_capita, coal_co2, gas_co2, oil_co2, cement_co2,
  cumulative_co2, share_global_co2, share_global_cumulative_co2,
  temperature_change_from_co2
FROM read_csv_auto(''/tmp/owid-co2-data.csv'')');
```

`run_in_duckdb()` is the bulk-load path, not the only write path: ordinary `INSERT`, `UPDATE`, and
`DELETE` work against an `ENGINE=DuckDB` table too. With the default `duckdb_dml_in_batch=ON` those
writes are buffered and applied to DuckDB at commit, so a write is not visible to later reads in the
same transaction. This tutorial only reads, so it loads once and queries from there.

Make sure the data was imported:

```sql
SELECT COUNT(*) AS total_rows FROM co2_emissions;
SELECT MIN(year) AS earliest,
       MAX(year) AS latest,
       COUNT(DISTINCT country) AS locations
FROM co2_emissions;
```

```text
total_rows
50411

earliest  latest  locations
1750      2024    254
```

## Analyze the Data

Start with the ten largest country-level emitters in the latest year:

```sql
SELECT country,
       ROUND(co2, 1) AS co2_mt,
       ROUND(co2_per_capita, 2) AS co2_per_capita_t
FROM co2_emissions
WHERE year = (SELECT MAX(year) FROM co2_emissions WHERE co2 IS NOT NULL)
  AND co2 IS NOT NULL
  AND iso_code IS NOT NULL
ORDER BY co2 DESC
LIMIT 10;
```

```text
country        co2_mt   co2_per_capita_t
China          12289.0  8.66
United States  4904.1   14.20
India          3193.5   2.20
Russia         1780.5   12.30
Japan          961.9    7.77
Indonesia      812.2    2.87
Iran           792.6    8.66
Saudi Arabia   692.1    20.38
South Korea    583.7    11.29
Germany        572.3    6.77
```

`iso_code IS NOT NULL` removes aggregate rows such as `World` and `Europe` from the ranking.

Next, group country-level emissions by decade:

```sql
SELECT FLOOR(year / 10) * 10 AS decade,
       ROUND(SUM(co2), 1) AS total_co2_mt
FROM co2_emissions
WHERE iso_code IS NOT NULL
  AND co2 IS NOT NULL
GROUP BY FLOOR(year / 10) * 10
ORDER BY decade DESC
LIMIT 5;
```

```text
decade  total_co2_mt
2020    181243.9
2010    342507.4
2000    279781.4
1990    229151.2
1980    196853.2
```

Use `LAG()` to calculate China's annual change:

```sql
SELECT year,
       ROUND(co2, 1) AS co2_mt,
       ROUND((co2 - LAG(co2) OVER (ORDER BY year)) /
             LAG(co2) OVER (ORDER BY year) * 100, 2) AS yoy_growth_pct
FROM co2_emissions
WHERE country = 'China'
  AND co2 IS NOT NULL
ORDER BY year DESC
LIMIT 5;
```

```text
year  co2_mt   yoy_growth_pct
2024  12289.0  0.96
2023  12172.0  3.93
2022  11711.8  3.79
2021  11284.4  3.56
2020  10896.5  1.71
```

## Clean Up

```sql
DROP DATABASE owid;
```

The [NYC taxi tutorial](nyc-taxi-trips.md) loads Parquet instead of CSV and joins DuckDB data with
an InnoDB reference table.

[owid-data]: https://github.com/owid/co2-data
[owid-license]: https://github.com/owid/co2-data/tree/382ee6c662b0ece26e111f263b44c029afad7787#license
