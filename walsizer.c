// These are almost always necessary for a bgworker

#include "postgres.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"

// Worker-related includes for SPI, IPC, signal handling.

#include "storage/ipc.h"
#include "executor/spi.h"
#include "tcop/utility.h"

// Usage specific includes

#include "postmaster/bgwriter.h"
#include "pgstat.h"

// Our own include comes last.

#include "walsizer.h"

// Some invisible variables for our GUCs

static bool walsizer_enable;
static int walsizer_max;
static int walsizer_threshold;

// Start the Postgres module!

PG_MODULE_MAGIC;

void _PG_init(void);

/**
 * Resize max_wal_size to avoid forced checkpoints
 * 
 * This worker process will monitor pg_stat_checkpointer at the same interval
 * as checkpoint_timeout. In the event there are any forced checkpoints over
 * a configurable threshold, it will increase max_wal_size by a multiple of
 * the amount of forced checkpoints over that period. This will continue up to
 * a configurable absolute maximum, to avoid exhausting available WAL storage
 * on the backing device.
 * 
 * This is generally only necessary on extremely active systems, and once a
 * max_wal_size maximum is found for the existing workload, it might make sense
 * to disable this extension from making further changes.
 * 
 * Based on src/test/modules/worker_spi/worker_spi.c
 */
void
walsizer_main(Datum main_arg)
{
	PgStat_CheckpointerStats *stats;
	int prev_req = 0;

	int32_t want_max = max_wal_size_mb;

	// These variables are used to build the ALTER SYSTEM ... statement nodes.
	// It's also possible to use raw_parser for this.

	AlterSystemStmt *alter_stmt = makeNode(AlterSystemStmt);
	A_Const *useval = makeNode(A_Const);

	// Add an atomic signal variable to skip any signals the extension sends.
	// This extension signals the postmaster to cause a global config reload,
	// and that would cause our event loop to repeat prematurely otherwise.

	static volatile sig_atomic_t my_signal = false;

	// Thankfully the AlterSystemStmt struct is very simple. It consists of a
	// single VariableSetStmt, which is a single name = value argument. This is
	// where we build the initial structure, barring the setstmt->args portion,
	// as that's a list, even if it's a single-item list. That will be set in
	// the main loop.

	alter_stmt->setstmt = makeNode(VariableSetStmt);
	alter_stmt->setstmt->kind = VAR_SET_VALUE;
	alter_stmt->setstmt->name = "max_wal_size";
	alter_stmt->setstmt->is_local = false;

	useval->val.ival.type = T_Integer;

	// Set the usual signal handlers and then let Postgres know the extension
	// is ready to operate. Then make it a bit more obvious we're running.

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	SetConfigOption("application_name", 
					 MyBgworkerEntry->bgw_name,
					 PGC_BACKEND, PGC_S_OVERRIDE);

	elog(LOG, "pg_walsizer worker successfully launched");

	// Connect to the database by supplying no DB or user, as this extension
	// operates as a superuser.

	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	// Create a resource owner. This is necessary for AlterSystemSetConfigFile
	// as one or more downstream calls uses ResourceOwnerEnlarge.

	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "walsizer");

	for (;;) {
		int32_t requested;

		WaitLatch(MyLatch,
				  WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
				  CheckPointTimeout * 1000L, PG_WAIT_EXTENSION);

		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		// If we sent the postmaster SIGHUP, that will clear our latch
		// prematurely. Continue the loop if that happens to resume normal
		// latch behavior. We should also handle any config reloads passed
		// to us explicitly.

		if (my_signal) {
			my_signal = false;
			continue;
		}

		if (ConfigReloadPending) {
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (!walsizer_enable)
			continue;

		// Now we calculate what the max_wal_size *should* be. This is done by
		// grabbing checkpointer statistics:
		//
		// src/backend/utils/activity/pgstat_checkpointer.c
		//
		// Currently only supports Postgres v15+ due to pg_stat_checkpointer
		// change. If there's enough demand, I'll support pg_stat_bgwriter in
		// v14 and older.

		pgstat_clear_snapshot();
		stats = pgstat_fetch_stat_checkpointer();

		if (prev_req == 0) {
			elog(DEBUG1, "no previous stats yet, skipping");
			#if PG_MAJORVERSION_NUM >= 17
			prev_req = stats->num_requested;
			#elif PG_MAJORVERSION_NUM >= 15
			prev_req = stats->requested_checkpoints;
			#endif
			continue;
		}

		#if PG_MAJORVERSION_NUM >= 17
		requested = stats->num_requested - prev_req;
		prev_req = stats->num_requested;
		#elif PG_MAJORVERSION_NUM >= 15
		requested = stats->requested_checkpoints - prev_req;
		prev_req = stats->requested_checkpoints;
		#endif

		if (requested < walsizer_threshold)
			continue;

		elog(LOG, "detected %d forced checkpoints over %d seconds", 
			requested, CheckPointTimeout);

		// The "algorithm" here is simple. Every forced checkpoint means we 
		// wrote enough WAL to exhaust the current max_wal_size over checkpoint
		// timeout. Increase the current max_wal_size by:
		//   requests * max_wal_size
		// If we go over the specified walsizer max, use that instead.

		want_max = max_wal_size_mb * (requested + 1);
		if (want_max > walsizer_max) {
			elog(WARNING, 
				 "requested max_wal_size of %d is greater than maximum of %d;"
				 " using maximum. Consider increasing walsizer.max",
				 want_max, walsizer_max);
			want_max = walsizer_max;
		}

		// Short-circuit to handle case if we're already at the max allowed.

		if (max_wal_size_mb == want_max)
			continue;

		elog(LOG, "WAL request threshold (%d) met, resizing max_wal_size",
		     walsizer_threshold);
		elog(LOG, "current max_wal_size is %d, should be %d",
			 max_wal_size_mb, want_max);

		useval->val.ival.ival = want_max;

		// To avoid having to maintain a memory context, we'll free the
		// argument list explicitly.

		if (alter_stmt->setstmt->args != NULL)
			list_free(alter_stmt->setstmt->args);

		alter_stmt->setstmt->args = list_make1(useval);

		// This is where we actually modify the max_wal_size setting. We built
		// the alter_stmt node tree for AlterSystemSetConfigFile to skip a lot
		// of internal parsing. We also signal Postgres to reload the config
		// file, and make sure to skip the next iteration of this loop that
		// signal will cause.

		StartTransactionCommand();
		AlterSystemSetConfigFile(alter_stmt);
		CommitTransactionCommand();

		my_signal = true;
		kill(PostmasterPid, SIGHUP);
	}

	proc_exit(0);

} // walsizer_main


/**
 * Set our expected GUCs and register the background worker callback
 * 
 * Current GUC list under 'walsizer' prefix:
 * - enable - Boolean to control modification of max_wal_size.
 * - max - Absolute maximum extension will never exceed.
 * - threshold - Forced checkpoints below this amount will be ignored.
 */
void
_PG_init(void)
{
	BackgroundWorker bgw = {0};

	DefineCustomBoolVariable(
		"walsizer.enable",
		"Enable automatic resizing of max_wal_size parameter.",
		NULL,
		&walsizer_enable,
		true,
		PGC_SIGHUP,				// Only supported by daemon reload
		0,						// No flags for this GUC
		NULL, NULL, NULL		// No hooks necessary
	);

	DefineCustomIntVariable(
		"walsizer.max",
		"Maximum size for max_wal_size that wal_sizer will not exceed.",
		"This should be set lower than the available storage of the WAL device.",
		&walsizer_max,
		4096, 2, MAX_KILOBYTES, // Basically the same limits as max_wal_size
		PGC_SIGHUP,				// Only supported by daemon reload
		GUC_UNIT_MB,			// This can be assigned in KB, MB, GB, etc.
		NULL, NULL, NULL		// No hooks necessary
	);

	DefineCustomIntVariable(
		"walsizer.threshold",
		"Amount of forced checkpoints per timeout before increasing max_wal_size.",
		"Set this to a higher value to ignore occasional WAL created by large batch jobs.",
		&walsizer_threshold,
		2, 1, 1000,
		PGC_SIGHUP,				// Only supported by daemon reload
		0,						// No flags for this GUC
		NULL, NULL, NULL		// No hooks necessary
	);

	MarkGUCPrefixReserved("walsizer");

	// Register and start the background worker. This seems like a lot of
	// boilerplate to start a worker using the walsizer_main function, but hey,
	// whatever works.

	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "pg_walsizer");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "walsizer_main");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "Walsizer worker");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "pg_walsizer");
	bgw.bgw_restart_time = CheckPointTimeout;
	bgw.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&bgw);

} // _PG_init
