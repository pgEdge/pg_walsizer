MODULE_big = pg_walsizer

PGFILEDESC = "Extension to resize max_wal_size based on checkpoint activity"
EXTENSION = pg_walsizer
#DATA = pg_walsizer--1.0.sql
OBJS = walsizer.o

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
