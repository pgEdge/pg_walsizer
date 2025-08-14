# The pg_walsizer Postgres Extension

One critical aspect of being a Postgres DBA is properly tuning configuration parameters based on system activity. Among these is [max_wal_size](https://www.postgresql.org/docs/current/runtime-config-wal.html#GUC-MAX-WAL-SIZE), which determines the maximum amount of data Postgres can store purely within the WAL between checkpoints. Any time this limit is exceeded before [checkpoint_timeout](https://www.postgresql.org/docs/current/runtime-config-wal.html#GUC-CHECKPOINT-TIMEOUT), Postgres will force a checkpoint to ensure this WAL data is written to the heap.

Benchmarks universally demonstrate that forced checkpoints _dramatically_ reduce OLTP performance. This penalty can exceed an order of magnitude depending on the underlying storage technology and filesystem. Thus it pays to set `max_wal_size` properly in active systems.

The default `max_wal_size` is a mere 1GB, and most DBAs are well advised to increase it. But to what? One method is to watch the Postgres log for these entries:

```
LOG:  checkpoints are occurring too frequently (2 seconds apart)
HINT:  Consider increasing the configuration parameter "max_wal_size".
```

It's a great warning, but what value should a DBA use in this circumstance? This extension removes the guesswork from performing this task. It is designed to monitor the number of forced checkpoints which occur over a `checkpoint_timeout` interval. It then automatically increases `max_wal_size` to prevent further forced checkpoints based on this telemetry. In the end, `max_wal_size` will always be set properly to accommodate the maximum workload experienced by the instance.

This is what happens when numerous forced checkpoints cross the threshold and trigger a modification of `max_wal_size`:

```
LOG:  detected 4 forced checkpoints over 60 seconds
LOG:  WAL request threshold (2) met, resizing max_wal_size
LOG:  current max_wal_size is 512, should be 2560
LOG:  received SIGHUP, reloading configuration files
LOG:  parameter "max_wal_size" changed to "2560"
```

Yes, I'm also surprised this actually works.

## Installation

Installing this extension is simple:

```bash
git clone git@github.com:pgedge/pg_walsizer.git
cd pg_walsizer
make
sudo make install
```

Then add it to `shared_preload_libraries`:

```sql
ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements, pg_walsizer';
```

Once Postgres is restarted, pg_walsizer will launch and manage `max_wal_size` until it's removed or disabled.

## Configuration

This extension currently accepts these parameters:

| Parameter | Default | Min | Max | Description |
|-----------|---------|-----|-----|-------------|
| walsizer.enable | true |  |  | Walsizer will modify `max_wal_size` when enabled. |
| walsizer.max | 4GB | 2MB | 2PB | Absolute maximum allowable value for `max_wal_size`. Walsizer will continue to recommend increases based on calculations, but will emit a warning rather than modify `max_wal_size`. |
| walsizer.threshold | 2 | 1 | 1000 | Forced checkpoints below this number are ignored. This essentially ignores occasional scheduled batch jobs which may cause one or two forced checkpoints, but `max_wal_size` is properly sized otherwise. |

All parameters can only be modified by SIGHUP, so must exist in `postgresql.conf`, `postgresql.auto.conf`, or a file included by one of these.

## Discussion

This extension may act as a learning exercise or skeleton for writing Postgres extensions which do the following:

* Launch a background worker.
* Properly manage a Postgres event loop using latches.
* Consume backend system statistics.
* Safely modify `postgresql.auto.conf`
* Signal the Postgres postmaster.
* Detect configuration changes.
* Manipulate Node trees and lists.

This extension is largely based on the following libraries:

* [SPI Worker test module](https://github.com/postgres/postgres/tree/master/src/test/modules/worker_spi)
* [Checkpointer library](https://github.com/postgres/postgres/tree/master/src/backend/utils/activity/pgstat_checkpointer.c)

In addition to the following documentation:

* [Postgres Background Worker Processes](https://www.postgresql.org/docs/current/bgworker.html)
* [C-Language Functions](https://www.postgresql.org/docs/current/xfunc-c.html)

As always, it's best to have a copy of the Postgres source code available for reference.

## Compatibility

This extension should be compatible with Postgres v15 or higher. This is when the backend stats system was reorganized to include `pgstat_fetch_stat_checkpointer` and related methods. Compatibility with older versions of Postgres is not a priority, but patches are always welcome.
