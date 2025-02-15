/*-------------------------------------------------------------------------
 *
 * backup.c: backup DB cluster, archived WAL
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#if PG_VERSION_NUM < 110000
#include "catalog/catalog.h"
#endif
#include "catalog/pg_tablespace.h"
#include "pgtar.h"
#include "receivelog.h"
#include "streamutil.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "utils/thread.h"
#include "utils/file.h"


/*
 * Macro needed to parse ptrack.
 * NOTE Keep those values syncronised with definitions in ptrack.h
 */
#define PTRACK_BITS_PER_HEAPBLOCK 1
#define HEAPBLOCKS_PER_BYTE (BITS_PER_BYTE / PTRACK_BITS_PER_HEAPBLOCK)

static int	standby_message_timeout = 10 * 1000;	/* 10 sec = default */
static XLogRecPtr stop_backup_lsn = InvalidXLogRecPtr;
static XLogRecPtr stop_stream_lsn = InvalidXLogRecPtr;

/*
 * How long we should wait for streaming end in seconds.
 * Retreived as checkpoint_timeout + checkpoint_timeout * 0.1
 */
static uint32 stream_stop_timeout = 0;
/* Time in which we started to wait for streaming end */
static time_t stream_stop_begin = 0;

const char *progname = "pg_probackup";

/* list of files contained in backup */
static parray *backup_files_list = NULL;

/* We need critical section for datapagemap_add() in case of using threads */
static pthread_mutex_t backup_pagemap_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * We need to wait end of WAL streaming before execute pg_stop_backup().
 */
typedef struct
{
	const char *basedir;
	PGconn	   *conn;

	/*
	 * Return value from the thread.
	 * 0 means there is no error, 1 - there is an error.
	 */
	int			ret;

	XLogRecPtr	startpos;
	TimeLineID	starttli;
} StreamThreadArg;

static pthread_t stream_thread;
static StreamThreadArg stream_thread_arg = {"", NULL, 1};

static int is_ptrack_enable = false;
bool is_ptrack_support = false;
bool exclusive_backup = false;

/* PostgreSQL server version from "backup_conn" */
static int server_version = 0;
static char server_version_str[100] = "";

/* Is pg_start_backup() was executed */
static bool backup_in_progress = false;
/* Is pg_stop_backup() was sent */
static bool pg_stop_backup_is_sent = false;

/*
 * Backup routines
 */
static void backup_cleanup(bool fatal, void *userdata);

static void *backup_files(void *arg);

static void do_backup_instance(PGconn *backup_conn);

static void pg_start_backup(const char *label, bool smooth, pgBackup *backup,
							PGconn *backup_conn, PGconn *master_conn);
static void pg_switch_wal(PGconn *conn);
static void pg_stop_backup(pgBackup *backup, PGconn *pg_startbackup_conn);
static int checkpoint_timeout(PGconn *backup_conn);

//static void backup_list_file(parray *files, const char *root, )
static XLogRecPtr wait_wal_lsn(XLogRecPtr lsn, bool is_start_lsn,
							   bool wait_prev_segment);
static void wait_replica_wal_lsn(XLogRecPtr lsn, bool is_start_backup, PGconn *backup_conn);
static void make_pagemap_from_ptrack(parray* files, PGconn* backup_conn);
static void *StreamLog(void *arg);

static void check_external_for_tablespaces(parray *external_list,
										   PGconn *backup_conn);

/* Ptrack functions */
static void pg_ptrack_clear(PGconn *backup_conn);
static bool pg_ptrack_support(PGconn *backup_conn);
static bool pg_ptrack_enable(PGconn *backup_conn);
static bool pg_ptrack_get_and_clear_db(Oid dbOid, Oid tblspcOid,
									   PGconn *backup_conn);
static char *pg_ptrack_get_and_clear(Oid tablespace_oid,
									 Oid db_oid,
									 Oid rel_oid,
									 size_t *result_size,
									 PGconn *backup_conn);
static XLogRecPtr get_last_ptrack_lsn(PGconn *backup_conn);

/* Check functions */
static bool pg_checksum_enable(PGconn *conn);
static bool pg_is_in_recovery(PGconn *conn);
static void check_server_version(PGconn *conn);
static void confirm_block_size(PGconn *conn, const char *name, int blcksz);
static void set_cfs_datafiles(parray *files, const char *root, char *relative, size_t i);

static void
backup_stopbackup_callback(bool fatal, void *userdata)
{
	PGconn *pg_startbackup_conn = (PGconn *) userdata;
	/*
	 * If backup is in progress, notify stop of backup to PostgreSQL
	 */
	if (backup_in_progress)
	{
		elog(WARNING, "backup in progress, stop backup");
		pg_stop_backup(NULL, pg_startbackup_conn);	/* don't care stop_lsn on error case */
	}
}

/*
 * Take a backup of a single postgresql instance.
 * Move files from 'pgdata' to a subdirectory in 'backup_path'.
 */
static void
do_backup_instance(PGconn *backup_conn)
{
	int			i;
	char		database_path[MAXPGPATH];
	char		external_prefix[MAXPGPATH]; /* Temp value. Used as template */
	char		dst_backup_path[MAXPGPATH];
	char		label[1024];
	XLogRecPtr	prev_backup_start_lsn = InvalidXLogRecPtr;

	/* arrays with meta info for multi threaded backup */
	pthread_t	*threads;
	backup_files_arg *threads_args;
	bool		backup_isok = true;

	pgBackup   *prev_backup = NULL;
	parray	   *prev_backup_filelist = NULL;
	parray	   *backup_list = NULL;
	parray	   *external_dirs = NULL;

	pgFile	   *pg_control = NULL;
	PGconn	   *master_conn = NULL;
	PGconn	   *pg_startbackup_conn = NULL;

	elog(LOG, "Database backup start");
	if(current.external_dir_str)
	{
		external_dirs = make_external_directory_list(current.external_dir_str,
													 false);
		check_external_for_tablespaces(external_dirs, backup_conn);
	}

	/* Obtain current timeline */
	current.tli = get_current_timeline(false);

	/*
	 * In incremental backup mode ensure that already-validated
	 * backup on current timeline exists and get its filelist.
	 */
	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE ||
		current.backup_mode == BACKUP_MODE_DIFF_PTRACK ||
		current.backup_mode == BACKUP_MODE_DIFF_DELTA)
	{
		char		prev_backup_filelist_path[MAXPGPATH];

		/* get list of backups already taken */
		backup_list = catalog_get_backup_list(INVALID_BACKUP_ID);

		prev_backup = catalog_get_last_data_backup(backup_list, current.tli);
		if (prev_backup == NULL)
			elog(ERROR, "Valid backup on current timeline is not found. "
						"Create new FULL backup before an incremental one.");

		pgBackupGetPath(prev_backup, prev_backup_filelist_path,
						lengthof(prev_backup_filelist_path), DATABASE_FILE_LIST);
		/* Files of previous backup needed by DELTA backup */
		prev_backup_filelist = dir_read_file_list(NULL, NULL, prev_backup_filelist_path, FIO_BACKUP_HOST);

		/* If lsn is not NULL, only pages with higher lsn will be copied. */
		prev_backup_start_lsn = prev_backup->start_lsn;
		current.parent_backup = prev_backup->start_time;

		write_backup(&current);
	}

	/*
	 * It`s illegal to take PTRACK backup if LSN from ptrack_control() is not
	 * equal to stop_lsn of previous backup.
	 */
	if (current.backup_mode == BACKUP_MODE_DIFF_PTRACK)
	{
		XLogRecPtr	ptrack_lsn = get_last_ptrack_lsn(backup_conn);

		if (ptrack_lsn > prev_backup->stop_lsn || ptrack_lsn == InvalidXLogRecPtr)
		{
			elog(ERROR, "LSN from ptrack_control %X/%X differs from STOP LSN of previous backup %X/%X.\n"
						"Create new full backup before an incremental one.",
						(uint32) (ptrack_lsn >> 32), (uint32) (ptrack_lsn),
						(uint32) (prev_backup->stop_lsn >> 32),
						(uint32) (prev_backup->stop_lsn));
		}
	}

	/* Clear ptrack files for FULL and PAGE backup */
	if (current.backup_mode != BACKUP_MODE_DIFF_PTRACK && is_ptrack_enable)
		pg_ptrack_clear(backup_conn);

	/* notify start of backup to PostgreSQL server */
	time2iso(label, lengthof(label), current.start_time);
	strncat(label, " with pg_probackup", lengthof(label) -
			strlen(" with pg_probackup"));

	/* Create connection to master server needed to call pg_start_backup */
	if (current.from_replica && exclusive_backup)
	{
		master_conn = pgut_connect(instance_config.master_conn_opt.pghost,
								   instance_config.master_conn_opt.pgport,
								   instance_config.master_conn_opt.pgdatabase,
								   instance_config.master_conn_opt.pguser);
		pg_startbackup_conn = master_conn;
	}
	else
		pg_startbackup_conn = backup_conn;

	pg_start_backup(label, smooth_checkpoint, &current,
					backup_conn, pg_startbackup_conn);

	/* For incremental backup check that start_lsn is not from the past */
	if (current.backup_mode != BACKUP_MODE_FULL &&
		prev_backup->start_lsn > current.start_lsn)
			elog(ERROR, "Current START LSN %X/%X is lower than START LSN %X/%X of previous backup %s. "
				"It may indicate that we are trying to backup PostgreSQL instance from the past.",
				(uint32) (current.start_lsn >> 32), (uint32) (current.start_lsn),
				(uint32) (prev_backup->start_lsn >> 32), (uint32) (prev_backup->start_lsn),
				base36enc(prev_backup->start_time));

	/* Update running backup meta with START LSN */
	write_backup(&current);

	pgBackupGetPath(&current, database_path, lengthof(database_path),
					DATABASE_DIR);
	pgBackupGetPath(&current, external_prefix, lengthof(external_prefix),
					EXTERNAL_DIR);

	/* start stream replication */
	if (stream_wal)
	{
		/* How long we should wait for streaming end after pg_stop_backup */
		stream_stop_timeout = checkpoint_timeout(backup_conn);
		stream_stop_timeout = stream_stop_timeout + stream_stop_timeout * 0.1;

		join_path_components(dst_backup_path, database_path, PG_XLOG_DIR);
		fio_mkdir(dst_backup_path, DIR_PERMISSION, FIO_BACKUP_HOST);

		stream_thread_arg.basedir = dst_backup_path;

		/*
		 * Connect in replication mode to the server.
		 */
		stream_thread_arg.conn = pgut_connect_replication(instance_config.conn_opt.pghost,
														  instance_config.conn_opt.pgport,
														  instance_config.conn_opt.pgdatabase,
														  instance_config.conn_opt.pguser);

		if (!CheckServerVersionForStreaming(stream_thread_arg.conn))
		{
			PQfinish(stream_thread_arg.conn);
			/*
			 * Error message already written in CheckServerVersionForStreaming().
			 * There's no hope of recovering from a version mismatch, so don't
			 * retry.
			 */
			elog(ERROR, "Cannot continue backup because stream connect has failed.");
		}

		/*
		 * Identify server, obtaining start LSN position and current timeline ID
		 * at the same time, necessary if not valid data can be found in the
		 * existing output directory.
		 */
		if (!RunIdentifySystem(stream_thread_arg.conn, NULL, NULL, NULL, NULL))
		{
			PQfinish(stream_thread_arg.conn);
			elog(ERROR, "Cannot continue backup because stream connect has failed.");
		}

        /* By default there are some error */
		stream_thread_arg.ret = 1;
		/* we must use startpos as start_lsn from start_backup */
		stream_thread_arg.startpos = current.start_lsn;
		stream_thread_arg.starttli = current.tli;

		thread_interrupted = false;
		pthread_create(&stream_thread, NULL, StreamLog, &stream_thread_arg);
	}

	/* initialize backup list */
	backup_files_list = parray_new();

	/* list files with the logical path. omit $PGDATA */
	dir_list_file(backup_files_list, instance_config.pgdata,
				  true, true, false, 0, FIO_DB_HOST);

	/*
	 * Append to backup list all files and directories
	 * from external directory option
	 */
	if (external_dirs)
		for (i = 0; i < parray_num(external_dirs); i++)
			/* External dirs numeration starts with 1.
			 * 0 value is not external dir */
			dir_list_file(backup_files_list, parray_get(external_dirs, i),
						  false, true, false, i+1, FIO_DB_HOST);

	/* Sanity check for backup_files_list, thank you, Windows:
	 * https://github.com/postgrespro/pg_probackup/issues/48
	 */

	if (parray_num(backup_files_list) < 100)
		elog(ERROR, "PGDATA is almost empty. Either it was concurrently deleted or "
			"pg_probackup do not possess sufficient permissions to list PGDATA content");

	/*
	 * Sort pathname ascending. It is necessary to create intermediate
	 * directories sequentially.
	 *
	 * For example:
	 * 1 - create 'base'
	 * 2 - create 'base/1'
	 *
	 * Sorted array is used at least in parse_filelist_filenames(),
	 * extractPageMap(), make_pagemap_from_ptrack().
	 */
	parray_qsort(backup_files_list, pgFileComparePath);

	/* Extract information about files in backup_list parsing their names:*/
	parse_filelist_filenames(backup_files_list, instance_config.pgdata);

	if (current.backup_mode != BACKUP_MODE_FULL)
	{
		elog(LOG, "current_tli:%X", current.tli);
		elog(LOG, "prev_backup->start_lsn: %X/%X",
			 (uint32) (prev_backup->start_lsn >> 32), (uint32) (prev_backup->start_lsn));
		elog(LOG, "current.start_lsn: %X/%X",
			 (uint32) (current.start_lsn >> 32), (uint32) (current.start_lsn));
	}

	/*
	 * Build page mapping in incremental mode.
	 */
	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE)
	{
		/*
		 * Build the page map. Obtain information about changed pages
		 * reading WAL segments present in archives up to the point
		 * where this backup has started.
		 */
		extractPageMap(arclog_path, current.tli, instance_config.xlog_seg_size,
					   prev_backup->start_lsn, current.start_lsn);
	}
	else if (current.backup_mode == BACKUP_MODE_DIFF_PTRACK)
	{
		/*
		 * Build the page map from ptrack information.
		 */
		make_pagemap_from_ptrack(backup_files_list, backup_conn);
	}

	/*
	 * Make directories before backup and setup threads at the same time
	 */
	for (i = 0; i < parray_num(backup_files_list); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(backup_files_list, i);

		/* if the entry was a directory, create it in the backup */
		if (S_ISDIR(file->mode))
		{
			char		dirpath[MAXPGPATH];
			char	   *dir_name;

			if (file->external_dir_num)
				dir_name = GetRelativePath(file->path,
								parray_get(external_dirs,
											file->external_dir_num - 1));
			else
				dir_name = GetRelativePath(file->path, instance_config.pgdata);

			elog(VERBOSE, "Create directory \"%s\"", dir_name);

			if (file->external_dir_num)
			{
				char		temp[MAXPGPATH];
				snprintf(temp, MAXPGPATH, "%s%d", external_prefix,
						 file->external_dir_num);
				join_path_components(dirpath, temp, dir_name);
			}
			else
				join_path_components(dirpath, database_path, dir_name);
			fio_mkdir(dirpath, DIR_PERMISSION, FIO_BACKUP_HOST);
		}

		/* setup threads */
		pg_atomic_clear_flag(&file->lock);
	}

	/* Sort by size for load balancing */
	parray_qsort(backup_files_list, pgFileCompareSize);
	/* Sort the array for binary search */
	if (prev_backup_filelist)
		parray_qsort(prev_backup_filelist, pgFileComparePathWithExternal);

	/* write initial backup_content.control file and update backup.control  */
	write_backup_filelist(&current, backup_files_list,
						  instance_config.pgdata, external_dirs);
	write_backup(&current);

	/* init thread args with own file lists */
	threads = (pthread_t *) palloc(sizeof(pthread_t) * num_threads);
	threads_args = (backup_files_arg *) palloc(sizeof(backup_files_arg)*num_threads);

	for (i = 0; i < num_threads; i++)
	{
		backup_files_arg *arg = &(threads_args[i]);

		arg->from_root = instance_config.pgdata;
		arg->to_root = database_path;
		arg->external_prefix = external_prefix;
		arg->external_dirs = external_dirs;
		arg->files_list = backup_files_list;
		arg->prev_filelist = prev_backup_filelist;
		arg->prev_start_lsn = prev_backup_start_lsn;
		arg->conn_arg.conn = NULL;
		arg->conn_arg.cancel_conn = NULL;
		arg->thread_num = i+1;
		/* By default there are some error */
		arg->ret = 1;
	}

	/* Run threads */
	thread_interrupted = false;
	elog(INFO, "Start transfering data files");
	for (i = 0; i < num_threads; i++)
	{
		backup_files_arg *arg = &(threads_args[i]);

		elog(VERBOSE, "Start thread num: %i", i);
		pthread_create(&threads[i], NULL, backup_files, arg);
	}

	/* Wait threads */
	for (i = 0; i < num_threads; i++)
	{
		pthread_join(threads[i], NULL);
		if (threads_args[i].ret == 1)
			backup_isok = false;
	}
	if (backup_isok)
		elog(INFO, "Data files are transfered");
	else
		elog(ERROR, "Data files transferring failed");

	/* Remove disappeared during backup files from backup_list */
	for (i = 0; i < parray_num(backup_files_list); i++)
	{
		pgFile	   *tmp_file = (pgFile *) parray_get(backup_files_list, i);

		if (tmp_file->write_size == FILE_NOT_FOUND)
		{
			pgFileFree(tmp_file);
			parray_remove(backup_files_list, i);
			i--;
		}
	}

	/* clean previous backup file list */
	if (prev_backup_filelist)
	{
		parray_walk(prev_backup_filelist, pgFileFree);
		parray_free(prev_backup_filelist);
	}

	/* In case of backup from replica >= 9.6 we must fix minRecPoint,
	 * First we must find pg_control in backup_files_list.
	 */
	if (current.from_replica && !exclusive_backup)
	{
		char		pg_control_path[MAXPGPATH];

		snprintf(pg_control_path, sizeof(pg_control_path), "%s/%s",
				 instance_config.pgdata, "global/pg_control");

		for (i = 0; i < parray_num(backup_files_list); i++)
		{
			pgFile	   *tmp_file = (pgFile *) parray_get(backup_files_list, i);

			if (strcmp(tmp_file->path, pg_control_path) == 0)
			{
				pg_control = tmp_file;
				break;
			}
		}
	}

	/* Notify end of backup */
	pg_stop_backup(&current, pg_startbackup_conn);

	if (current.from_replica && !exclusive_backup)
		set_min_recovery_point(pg_control, database_path, current.stop_lsn);

	/* Add archived xlog files into the list of files of this backup */
	if (stream_wal)
	{
		parray     *xlog_files_list;
		char		pg_xlog_path[MAXPGPATH];

		/* Scan backup PG_XLOG_DIR */
		xlog_files_list = parray_new();
		join_path_components(pg_xlog_path, database_path, PG_XLOG_DIR);
		dir_list_file(xlog_files_list, pg_xlog_path, false, true, false, 0,
					  FIO_BACKUP_HOST);

		for (i = 0; i < parray_num(xlog_files_list); i++)
		{
			pgFile	   *file = (pgFile *) parray_get(xlog_files_list, i);
			if (S_ISREG(file->mode))
			{
				file->crc = pgFileGetCRC(file->path, true, false,
										 &file->read_size, FIO_BACKUP_HOST);
				file->write_size = file->read_size;
			}
			/* Remove file path root prefix*/
			if (strstr(file->path, database_path) == file->path)
			{
				char	   *ptr = file->path;

				file->path = pstrdup(GetRelativePath(ptr, database_path));
				free(ptr);
			}
		}
		/* Add xlog files into the list of backed up files */
		parray_concat(backup_files_list, xlog_files_list);
		parray_free(xlog_files_list);
	}

	/* Print the list of files to backup catalog */
	write_backup_filelist(&current, backup_files_list, instance_config.pgdata,
						  external_dirs);
	/* update backup control file to update size info */
	write_backup(&current);

	/* clean external directories list */
	if (external_dirs)
		free_dir_list(external_dirs);

	/* Cleanup */
	if (backup_list)
	{
		parray_walk(backup_list, pgBackupFree);
		parray_free(backup_list);
	}

	parray_walk(backup_files_list, pgFileFree);
	parray_free(backup_files_list);
	backup_files_list = NULL;
}

/*
 * Common code for CHECKDB and BACKUP commands.
 * Ensure that we're able to connect to the instance
 * check compatibility and fill basic info.
 * For checkdb launched in amcheck mode with pgdata validation
 * do not check system ID, it gives user an opportunity to
 * check remote PostgreSQL instance.
 * Also checking system ID in this case serves no purpose, because
 * all work is done by server.
 * 
 * Returns established connection
 */
PGconn *
pgdata_basic_setup(ConnectionOptions conn_opt, PGNodeInfo *nodeInfo)
{
	PGconn *cur_conn;

	/* Create connection for PostgreSQL */
	cur_conn = pgut_connect(conn_opt.pghost, conn_opt.pgport,
							   conn_opt.pgdatabase,
							   conn_opt.pguser);

	current.primary_conninfo = pgut_get_conninfo_string(cur_conn);

	/* Confirm data block size and xlog block size are compatible */
	confirm_block_size(cur_conn, "block_size", BLCKSZ);
	confirm_block_size(cur_conn, "wal_block_size", XLOG_BLCKSZ);
	nodeInfo->block_size = BLCKSZ;
	nodeInfo->wal_block_size = XLOG_BLCKSZ;

	current.from_replica = pg_is_in_recovery(cur_conn);

	/* Confirm that this server version is supported */
	check_server_version(cur_conn);

	if (pg_checksum_enable(cur_conn))
		current.checksum_version = 1;
	else
		current.checksum_version = 0;

	nodeInfo->checksum_version = current.checksum_version;

	if (current.checksum_version)
		elog(LOG, "This PostgreSQL instance was initialized with data block checksums. "
					"Data block corruption will be detected");
	else
		elog(WARNING, "This PostgreSQL instance was initialized without data block checksums. "
						"pg_probackup have no way to detect data block corruption without them. "
						"Reinitialize PGDATA with option '--data-checksums'.");

	StrNCpy(current.server_version, server_version_str,
			sizeof(current.server_version));

	StrNCpy(nodeInfo->server_version, server_version_str,
			sizeof(nodeInfo->server_version));

	return cur_conn;
}

/*
 * Entry point of pg_probackup BACKUP subcommand.
 */
int
do_backup(time_t start_time, bool no_validate)
{
	PGconn *backup_conn = NULL;

	if (!instance_config.pgdata)
		elog(ERROR, "required parameter not specified: PGDATA "
						 "(-D, --pgdata)");
	/*
	 * setup backup_conn, do some compatibility checks and
	 * fill basic info about instance
	 */
	backup_conn = pgdata_basic_setup(instance_config.conn_opt,
										 &(current.nodeInfo));
	/*
	 * Ensure that backup directory was initialized for the same PostgreSQL
	 * instance we opened connection to. And that target backup database PGDATA
	 * belogns to the same instance.
	 */
	check_system_identifiers(backup_conn, instance_config.pgdata);

	/* below perform checks specific for backup command */
#if PG_VERSION_NUM >= 110000
	if (!RetrieveWalSegSize(backup_conn))
		elog(ERROR, "Failed to retreive wal_segment_size");
#endif

	current.compress_alg = instance_config.compress_alg;
	current.compress_level = instance_config.compress_level;

	current.stream = stream_wal;

	is_ptrack_support = pg_ptrack_support(backup_conn);
	if (is_ptrack_support)
	{
		is_ptrack_enable = pg_ptrack_enable(backup_conn);
	}

	if (current.backup_mode == BACKUP_MODE_DIFF_PTRACK)
	{
		if (!is_ptrack_support)
			elog(ERROR, "This PostgreSQL instance does not support ptrack");
		else
		{
			if(!is_ptrack_enable)
				elog(ERROR, "Ptrack is disabled");
		}
	}

	if (current.from_replica && exclusive_backup)
		/* Check master connection options */
		if (instance_config.master_conn_opt.pghost == NULL)
			elog(ERROR, "Options for connection to master must be provided to perform backup from replica");

	/* Start backup. Update backup status. */
	current.status = BACKUP_STATUS_RUNNING;
	current.start_time = start_time;
	StrNCpy(current.program_version, PROGRAM_VERSION,
			sizeof(current.program_version));

	/* Save list of external directories */
	if (instance_config.external_dir_str &&
		pg_strcasecmp(instance_config.external_dir_str, "none") != 0)
	{
		current.external_dir_str = instance_config.external_dir_str;
	}

	/* Create backup directory and BACKUP_CONTROL_FILE */
	if (pgBackupCreateDir(&current))
		elog(ERROR, "Cannot create backup directory");
	if (!lock_backup(&current))
		elog(ERROR, "Cannot lock backup %s directory",
			 base36enc(current.start_time));
	write_backup(&current);

	elog(LOG, "Backup destination is initialized");

	/* set the error processing function for the backup process */
	pgut_atexit_push(backup_cleanup, NULL);

	/* backup data */
	do_backup_instance(backup_conn);
	pgut_atexit_pop(backup_cleanup, NULL);

	/* compute size of wal files of this backup stored in the archive */
	if (!current.stream)
	{
		current.wal_bytes = instance_config.xlog_seg_size *
			(current.stop_lsn / instance_config.xlog_seg_size -
			 current.start_lsn / instance_config.xlog_seg_size + 1);
	}

	/* Backup is done. Update backup status */
	current.end_time = time(NULL);
	current.status = BACKUP_STATUS_DONE;
	write_backup(&current);

	//elog(LOG, "Backup completed. Total bytes : " INT64_FORMAT "",
	//		current.data_bytes);

	if (!no_validate)
		pgBackupValidate(&current);

	if (current.status == BACKUP_STATUS_OK ||
		current.status == BACKUP_STATUS_DONE)
		elog(INFO, "Backup %s completed", base36enc(current.start_time));
	else
		elog(ERROR, "Backup %s failed", base36enc(current.start_time));

	/*
	 * After successful backup completion remove backups
	 * which are expired according to retention policies
	 */
	if (delete_expired || merge_expired || delete_wal)
		do_retention();

	return 0;
}

/*
 * Confirm that this server version is supported
 */
static void
check_server_version(PGconn *conn)
{
	PGresult   *res;

	/* confirm server version */
	server_version = PQserverVersion(conn);

	if (server_version == 0)
		elog(ERROR, "Unknown server version %d", server_version);

	if (server_version < 100000)
		sprintf(server_version_str, "%d.%d",
				server_version / 10000,
				(server_version / 100) % 100);
	else
		sprintf(server_version_str, "%d",
				server_version / 10000);

	if (server_version < 90500)
		elog(ERROR,
			 "server version is %s, must be %s or higher",
			 server_version_str, "9.5");

	if (current.from_replica && server_version < 90600)
		elog(ERROR,
			 "server version is %s, must be %s or higher for backup from replica",
			 server_version_str, "9.6");

	res = pgut_execute_extended(conn, "SELECT pgpro_edition()",
								0, NULL, true, true);

	/*
	 * Check major version of connected PostgreSQL and major version of
	 * compiled PostgreSQL.
	 */
#ifdef PGPRO_VERSION
	if (PQresultStatus(res) == PGRES_FATAL_ERROR)
		/* It seems we connected to PostgreSQL (not Postgres Pro) */
		elog(ERROR, "%s was built with Postgres Pro %s %s, "
					"but connection is made with PostgreSQL %s",
			 PROGRAM_NAME, PG_MAJORVERSION, PGPRO_EDITION, server_version_str);
	else if (strcmp(server_version_str, PG_MAJORVERSION) != 0 &&
			 strcmp(PQgetvalue(res, 0, 0), PGPRO_EDITION) != 0)
		elog(ERROR, "%s was built with Postgres Pro %s %s, "
					"but connection is made with Postgres Pro %s %s",
			 PROGRAM_NAME, PG_MAJORVERSION, PGPRO_EDITION,
			 server_version_str, PQgetvalue(res, 0, 0));
#else
	if (PQresultStatus(res) != PGRES_FATAL_ERROR)
		/* It seems we connected to Postgres Pro (not PostgreSQL) */
		elog(ERROR, "%s was built with PostgreSQL %s, "
					"but connection is made with Postgres Pro %s %s",
			 PROGRAM_NAME, PG_MAJORVERSION,
			 server_version_str, PQgetvalue(res, 0, 0));
	else if (strcmp(server_version_str, PG_MAJORVERSION) != 0)
		elog(ERROR, "%s was built with PostgreSQL %s, but connection is made with %s",
			 PROGRAM_NAME, PG_MAJORVERSION, server_version_str);
#endif

	PQclear(res);

	/* Do exclusive backup only for PostgreSQL 9.5 */
	exclusive_backup = server_version < 90600 ||
		current.backup_mode == BACKUP_MODE_DIFF_PTRACK;
}

/*
 * Ensure that backup directory was initialized for the same PostgreSQL
 * instance we opened connection to. And that target backup database PGDATA
 * belogns to the same instance.
 * All system identifiers must be equal.
 */
void
check_system_identifiers(PGconn *conn, char *pgdata)
{
	uint64		system_id_conn;
	uint64		system_id_pgdata;

	system_id_pgdata = get_system_identifier(pgdata);
	system_id_conn = get_remote_system_identifier(conn);

	/* for checkdb check only system_id_pgdata and system_id_conn */
	if (current.backup_mode == BACKUP_MODE_INVALID)
	{
		if (system_id_conn != system_id_pgdata)
		{
			elog(ERROR, "Data directory initialized with system id " UINT64_FORMAT ", "
						"but connected instance system id is " UINT64_FORMAT,
				 system_id_pgdata, system_id_conn);
		}
		return;
	}

	if (system_id_conn != instance_config.system_identifier)
		elog(ERROR, "Backup data directory was initialized for system id " UINT64_FORMAT ", "
					"but connected instance system id is " UINT64_FORMAT,
			 instance_config.system_identifier, system_id_conn);
	if (system_id_pgdata != instance_config.system_identifier)
		elog(ERROR, "Backup data directory was initialized for system id " UINT64_FORMAT ", "
					"but target backup directory system id is " UINT64_FORMAT,
			 instance_config.system_identifier, system_id_pgdata);
}

/*
 * Ensure that target backup database is initialized with
 * compatible settings. Currently check BLCKSZ and XLOG_BLCKSZ.
 */
static void
confirm_block_size(PGconn *conn, const char *name, int blcksz)
{
	PGresult   *res;
	char	   *endp;
	int			block_size;

	res = pgut_execute(conn, "SELECT pg_catalog.current_setting($1)", 1, &name);
	if (PQntuples(res) != 1 || PQnfields(res) != 1)
		elog(ERROR, "cannot get %s: %s", name, PQerrorMessage(conn));

	block_size = strtol(PQgetvalue(res, 0, 0), &endp, 10);
	if ((endp && *endp) || block_size != blcksz)
		elog(ERROR,
			 "%s(%d) is not compatible(%d expected)",
			 name, block_size, blcksz);

	PQclear(res);
}

/*
 * Notify start of backup to PostgreSQL server.
 */
static void
pg_start_backup(const char *label, bool smooth, pgBackup *backup,
				PGconn *backup_conn, PGconn *pg_startbackup_conn)
{
	PGresult   *res;
	const char *params[2];
	uint32		lsn_hi;
	uint32		lsn_lo;
	PGconn	   *conn;

	params[0] = label;

	/* For 9.5 replica we call pg_start_backup() on master */
	conn = pg_startbackup_conn;

	/* 2nd argument is 'fast'*/
	params[1] = smooth ? "false" : "true";
	if (!exclusive_backup)
		res = pgut_execute(conn,
						   "SELECT pg_catalog.pg_start_backup($1, $2, false)",
						   2,
						   params);
	else
		res = pgut_execute(conn,
						   "SELECT pg_catalog.pg_start_backup($1, $2)",
						   2,
						   params);

	/*
	 * Set flag that pg_start_backup() was called. If an error will happen it
	 * is necessary to call pg_stop_backup() in backup_cleanup().
	 */
	backup_in_progress = true;
	pgut_atexit_push(backup_stopbackup_callback, pg_startbackup_conn);

	/* Extract timeline and LSN from results of pg_start_backup() */
	XLogDataFromLSN(PQgetvalue(res, 0, 0), &lsn_hi, &lsn_lo);
	/* Calculate LSN */
	backup->start_lsn = ((uint64) lsn_hi )<< 32 | lsn_lo;

	PQclear(res);

	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE &&
			(!(backup->from_replica && !exclusive_backup)))
		/*
		 * Switch to a new WAL segment. It is necessary to get archived WAL
		 * segment, which includes start LSN of current backup.
		 * Don`t do this for replica backups unless it`s PG 9.5
		 */
		pg_switch_wal(conn);

	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE)
		/* In PAGE mode wait for current segment... */
		wait_wal_lsn(backup->start_lsn, true, false);
	/*
	 * Do not wait start_lsn for stream backup.
	 * Because WAL streaming will start after pg_start_backup() in stream
	 * mode.
	 */
	else if (!stream_wal)
		/* ...for others wait for previous segment */
		wait_wal_lsn(backup->start_lsn, true, true);

	/* In case of backup from replica for PostgreSQL 9.5
	 * wait for start_lsn to be replayed by replica
	 */
	if (backup->from_replica && exclusive_backup)
		wait_replica_wal_lsn(backup->start_lsn, true, backup_conn);
}

/*
 * Switch to a new WAL segment. It should be called only for master.
 */
static void
pg_switch_wal(PGconn *conn)
{
	PGresult   *res;

	/* Remove annoying NOTICE messages generated by backend */
	res = pgut_execute(conn, "SET client_min_messages = warning;", 0, NULL);
	PQclear(res);

#if PG_VERSION_NUM >= 100000
	res = pgut_execute(conn, "SELECT * FROM pg_catalog.pg_switch_wal()", 0, NULL);
#else
	res = pgut_execute(conn, "SELECT * FROM pg_catalog.pg_switch_xlog()", 0, NULL);
#endif

	PQclear(res);
}

/*
 * Check if the instance supports ptrack
 * TODO Maybe we should rather check ptrack_version()?
 */
static bool
pg_ptrack_support(PGconn *backup_conn)
{
	PGresult   *res_db;

	res_db = pgut_execute(backup_conn,
						  "SELECT proname FROM pg_proc WHERE proname='ptrack_version'",
						  0, NULL);
	if (PQntuples(res_db) == 0)
	{
		PQclear(res_db);
		return false;
	}
	PQclear(res_db);

	res_db = pgut_execute(backup_conn,
						  "SELECT pg_catalog.ptrack_version()",
						  0, NULL);
	if (PQntuples(res_db) == 0)
	{
		PQclear(res_db);
		return false;
	}

	/* Now we support only ptrack versions upper than 1.5 */
	if (strcmp(PQgetvalue(res_db, 0, 0), "1.5") != 0 &&
		strcmp(PQgetvalue(res_db, 0, 0), "1.6") != 0 &&
		strcmp(PQgetvalue(res_db, 0, 0), "1.7") != 0)
	{
		elog(WARNING, "Update your ptrack to the version 1.5 or upper. Current version is %s", PQgetvalue(res_db, 0, 0));
		PQclear(res_db);
		return false;
	}

	PQclear(res_db);
	return true;
}

/* Check if ptrack is enabled in target instance */
static bool
pg_ptrack_enable(PGconn *backup_conn)
{
	PGresult   *res_db;

	res_db = pgut_execute(backup_conn, "SHOW ptrack_enable", 0, NULL);

	if (strcmp(PQgetvalue(res_db, 0, 0), "on") != 0)
	{
		PQclear(res_db);
		return false;
	}
	PQclear(res_db);
	return true;
}

/* Check if ptrack is enabled in target instance */
static bool
pg_checksum_enable(PGconn *conn)
{
	PGresult   *res_db;

	res_db = pgut_execute(conn, "SHOW data_checksums", 0, NULL);

	if (strcmp(PQgetvalue(res_db, 0, 0), "on") != 0)
	{
		PQclear(res_db);
		return false;
	}
	PQclear(res_db);
	return true;
}

/* Check if target instance is replica */
static bool
pg_is_in_recovery(PGconn *conn)
{
	PGresult   *res_db;

	res_db = pgut_execute(conn, "SELECT pg_catalog.pg_is_in_recovery()", 0, NULL);

	if (PQgetvalue(res_db, 0, 0)[0] == 't')
	{
		PQclear(res_db);
		return true;
	}
	PQclear(res_db);
	return false;
}

/* Clear ptrack files in all databases of the instance we connected to */
static void
pg_ptrack_clear(PGconn *backup_conn)
{
	PGresult   *res_db,
			   *res;
	const char *dbname;
	int			i;
	Oid dbOid, tblspcOid;
	char *params[2];

	params[0] = palloc(64);
	params[1] = palloc(64);
	res_db = pgut_execute(backup_conn, "SELECT datname, oid, dattablespace FROM pg_database",
						  0, NULL);

	for(i = 0; i < PQntuples(res_db); i++)
	{
		PGconn	   *tmp_conn;

		dbname = PQgetvalue(res_db, i, 0);
		if (strcmp(dbname, "template0") == 0)
			continue;

		dbOid = atoi(PQgetvalue(res_db, i, 1));
		tblspcOid = atoi(PQgetvalue(res_db, i, 2));

		tmp_conn = pgut_connect(instance_config.conn_opt.pghost, instance_config.conn_opt.pgport,
								dbname,
								instance_config.conn_opt.pguser);

		res = pgut_execute(tmp_conn, "SELECT pg_catalog.pg_ptrack_clear()",
						   0, NULL);
		PQclear(res);

		sprintf(params[0], "%i", dbOid);
		sprintf(params[1], "%i", tblspcOid);
		res = pgut_execute(tmp_conn, "SELECT pg_catalog.pg_ptrack_get_and_clear_db($1, $2)",
						   2, (const char **)params);
		PQclear(res);

		pgut_disconnect(tmp_conn);
	}

	pfree(params[0]);
	pfree(params[1]);
	PQclear(res_db);
}

static bool
pg_ptrack_get_and_clear_db(Oid dbOid, Oid tblspcOid, PGconn *backup_conn)
{
	char	   *params[2];
	char	   *dbname;
	PGresult   *res_db;
	PGresult   *res;
	bool		result;

	params[0] = palloc(64);
	params[1] = palloc(64);

	sprintf(params[0], "%i", dbOid);
	res_db = pgut_execute(backup_conn,
							"SELECT datname FROM pg_database WHERE oid=$1",
							1, (const char **) params);
	/*
	 * If database is not found, it's not an error.
	 * It could have been deleted since previous backup.
	 */
	if (PQntuples(res_db) != 1 || PQnfields(res_db) != 1)
		return false;

	dbname = PQgetvalue(res_db, 0, 0);

	/* Always backup all files from template0 database */
	if (strcmp(dbname, "template0") == 0)
	{
		PQclear(res_db);
		return true;
	}
	PQclear(res_db);

	sprintf(params[0], "%i", dbOid);
	sprintf(params[1], "%i", tblspcOid);
	res = pgut_execute(backup_conn, "SELECT pg_catalog.pg_ptrack_get_and_clear_db($1, $2)",
						2, (const char **)params);

	if (PQnfields(res) != 1)
		elog(ERROR, "cannot perform pg_ptrack_get_and_clear_db()");

	if (!parse_bool(PQgetvalue(res, 0, 0), &result))
		elog(ERROR,
			 "result of pg_ptrack_get_and_clear_db() is invalid: %s",
			 PQgetvalue(res, 0, 0));

	PQclear(res);
	pfree(params[0]);
	pfree(params[1]);

	return result;
}

/* Read and clear ptrack files of the target relation.
 * Result is a bytea ptrack map of all segments of the target relation.
 * case 1: we know a tablespace_oid, db_oid, and rel_filenode
 * case 2: we know db_oid and rel_filenode (no tablespace_oid, because file in pg_default)
 * case 3: we know only rel_filenode (because file in pg_global)
 */
static char *
pg_ptrack_get_and_clear(Oid tablespace_oid, Oid db_oid, Oid rel_filenode,
						size_t *result_size, PGconn *backup_conn)
{
	PGconn	   *tmp_conn;
	PGresult   *res_db,
			   *res;
	char	   *params[2];
	char	   *result;
	char	   *val;

	params[0] = palloc(64);
	params[1] = palloc(64);

	/* regular file (not in directory 'global') */
	if (db_oid != 0)
	{
		char	   *dbname;

		sprintf(params[0], "%i", db_oid);
		res_db = pgut_execute(backup_conn,
							  "SELECT datname FROM pg_database WHERE oid=$1",
							  1, (const char **) params);
		/*
		 * If database is not found, it's not an error.
		 * It could have been deleted since previous backup.
		 */
		if (PQntuples(res_db) != 1 || PQnfields(res_db) != 1)
			return NULL;

		dbname = PQgetvalue(res_db, 0, 0);

		if (strcmp(dbname, "template0") == 0)
		{
			PQclear(res_db);
			return NULL;
		}

		tmp_conn = pgut_connect(instance_config.conn_opt.pghost, instance_config.conn_opt.pgport,
								dbname,
								instance_config.conn_opt.pguser);
		sprintf(params[0], "%i", tablespace_oid);
		sprintf(params[1], "%i", rel_filenode);
		res = pgut_execute(tmp_conn, "SELECT pg_catalog.pg_ptrack_get_and_clear($1, $2)",
						   2, (const char **)params);

		if (PQnfields(res) != 1)
			elog(ERROR, "cannot get ptrack file from database \"%s\" by tablespace oid %u and relation oid %u",
				 dbname, tablespace_oid, rel_filenode);
		PQclear(res_db);
		pgut_disconnect(tmp_conn);
	}
	/* file in directory 'global' */
	else
	{
		/*
		 * execute ptrack_get_and_clear for relation in pg_global
		 * Use backup_conn, cause we can do it from any database.
		 */
		sprintf(params[0], "%i", tablespace_oid);
		sprintf(params[1], "%i", rel_filenode);
		res = pgut_execute(backup_conn, "SELECT pg_catalog.pg_ptrack_get_and_clear($1, $2)",
						   2, (const char **)params);

		if (PQnfields(res) != 1)
			elog(ERROR, "cannot get ptrack file from pg_global tablespace and relation oid %u",
			 rel_filenode);
	}

	val = PQgetvalue(res, 0, 0);

	/* TODO Now pg_ptrack_get_and_clear() returns bytea ending with \x.
	 * It should be fixed in future ptrack releases, but till then we
	 * can parse it.
	 */
	if (strcmp("x", val+1) == 0)
	{
		/* Ptrack file is missing */
		return NULL;
	}

	result = (char *) PQunescapeBytea((unsigned char *) PQgetvalue(res, 0, 0),
									  result_size);
	PQclear(res);
	pfree(params[0]);
	pfree(params[1]);

	return result;
}

/*
 * Wait for target 'lsn'.
 *
 * If current backup started in archive mode wait for 'lsn' to be archived in
 * archive 'wal' directory with WAL segment file.
 * If current backup started in stream mode wait for 'lsn' to be streamed in
 * 'pg_wal' directory.
 *
 * If 'is_start_lsn' is true and backup mode is PAGE then we wait for 'lsn' to
 * be archived in archive 'wal' directory regardless stream mode.
 *
 * If 'wait_prev_segment' wait for previous segment.
 *
 * Returns LSN of last valid record if wait_prev_segment is not true, otherwise
 * returns InvalidXLogRecPtr.
 */
static XLogRecPtr
wait_wal_lsn(XLogRecPtr lsn, bool is_start_lsn, bool wait_prev_segment)
{
	TimeLineID	tli;
	XLogSegNo	targetSegNo;
	char		pg_wal_dir[MAXPGPATH];
	char		wal_segment_path[MAXPGPATH],
			   *wal_segment_dir,
				wal_segment[MAXFNAMELEN];
	bool		file_exists = false;
	uint32		try_count = 0,
				timeout;

#ifdef HAVE_LIBZ
	char		gz_wal_segment_path[MAXPGPATH];
#endif

	tli = get_current_timeline(false);

	/* Compute the name of the WAL file containig requested LSN */
	GetXLogSegNo(lsn, targetSegNo, instance_config.xlog_seg_size);
	if (wait_prev_segment)
		targetSegNo--;
	GetXLogFileName(wal_segment, tli, targetSegNo,
					instance_config.xlog_seg_size);

	/*
	 * In pg_start_backup we wait for 'lsn' in 'pg_wal' directory if it is
	 * stream and non-page backup. Page backup needs archived WAL files, so we
	 * wait for 'lsn' in archive 'wal' directory for page backups.
	 *
	 * In pg_stop_backup it depends only on stream_wal.
	 */
	if (stream_wal &&
		(current.backup_mode != BACKUP_MODE_DIFF_PAGE || !is_start_lsn))
	{
		pgBackupGetPath2(&current, pg_wal_dir, lengthof(pg_wal_dir),
						 DATABASE_DIR, PG_XLOG_DIR);
		join_path_components(wal_segment_path, pg_wal_dir, wal_segment);
		wal_segment_dir = pg_wal_dir;
	}
	else
	{
		join_path_components(wal_segment_path, arclog_path, wal_segment);
		wal_segment_dir = arclog_path;
	}

	if (instance_config.archive_timeout > 0)
		timeout = instance_config.archive_timeout;
	else
		timeout = ARCHIVE_TIMEOUT_DEFAULT;

	if (wait_prev_segment)
		elog(LOG, "Looking for segment: %s", wal_segment);
	else
		elog(LOG, "Looking for LSN %X/%X in segment: %s",
			 (uint32) (lsn >> 32), (uint32) lsn, wal_segment);

#ifdef HAVE_LIBZ
	snprintf(gz_wal_segment_path, sizeof(gz_wal_segment_path), "%s.gz",
			 wal_segment_path);
#endif

	/* Wait until target LSN is archived or streamed */
	while (true)
	{
		if (!file_exists)
		{
			file_exists = fileExists(wal_segment_path, FIO_BACKUP_HOST);

			/* Try to find compressed WAL file */
			if (!file_exists)
			{
#ifdef HAVE_LIBZ
				file_exists = fileExists(gz_wal_segment_path, FIO_BACKUP_HOST);
				if (file_exists)
					elog(LOG, "Found compressed WAL segment: %s", wal_segment_path);
#endif
			}
			else
				elog(LOG, "Found WAL segment: %s", wal_segment_path);
		}

		if (file_exists)
		{
			/* Do not check LSN for previous WAL segment */
			if (wait_prev_segment)
				return InvalidXLogRecPtr;

			/*
			 * A WAL segment found. Check LSN on it.
			 */
			if (wal_contains_lsn(wal_segment_dir, lsn, tli,
								 instance_config.xlog_seg_size))
				/* Target LSN was found */
			{
				elog(LOG, "Found LSN: %X/%X", (uint32) (lsn >> 32), (uint32) lsn);
				return lsn;
			}

			/*
			 * If we failed to get LSN of valid record in a reasonable time, try
			 * to get LSN of last valid record prior to the target LSN. But only
			 * in case of a backup from a replica.
			 */
			if (!exclusive_backup && current.from_replica &&
				(try_count > timeout / 4))
			{
				XLogRecPtr	res;

				res = get_last_wal_lsn(wal_segment_dir, current.start_lsn,
									   lsn, tli, false,
									   instance_config.xlog_seg_size);
				if (!XLogRecPtrIsInvalid(res))
				{
					/* LSN of the prior record was found */
					elog(LOG, "Found prior LSN: %X/%X, it is used as stop LSN",
						 (uint32) (res >> 32), (uint32) res);
					return res;
				}
			}
		}

		sleep(1);
		if (interrupted)
			elog(ERROR, "Interrupted during waiting for WAL archiving");
		try_count++;

		/* Inform user if WAL segment is absent in first attempt */
		if (try_count == 1)
		{
			if (wait_prev_segment)
				elog(INFO, "Wait for WAL segment %s to be archived",
					 wal_segment_path);
			else
				elog(INFO, "Wait for LSN %X/%X in archived WAL segment %s",
					 (uint32) (lsn >> 32), (uint32) lsn, wal_segment_path);
		}

		if (!stream_wal && is_start_lsn && try_count == 30)
			elog(WARNING, "By default pg_probackup assume WAL delivery method to be ARCHIVE. "
				 "If continius archiving is not set up, use '--stream' option to make autonomous backup. "
				 "Otherwise check that continius archiving works correctly.");

		if (timeout > 0 && try_count > timeout)
		{
			if (file_exists)
				elog(ERROR, "WAL segment %s was archived, "
					 "but target LSN %X/%X could not be archived in %d seconds",
					 wal_segment, (uint32) (lsn >> 32), (uint32) lsn, timeout);
			/* If WAL segment doesn't exist or we wait for previous segment */
			else
				elog(ERROR,
					 "Switched WAL segment %s could not be archived in %d seconds",
					 wal_segment, timeout);
		}
	}
}

/*
 * Wait for target 'lsn' on replica instance from master.
 */
static void
wait_replica_wal_lsn(XLogRecPtr lsn, bool is_start_backup,
					 PGconn *backup_conn)
{
	uint32		try_count = 0;

	while (true)
	{
		XLogRecPtr	replica_lsn;

		/*
		 * For lsn from pg_start_backup() we need it to be replayed on replica's
		 * data.
		 */
		if (is_start_backup)
		{
			replica_lsn = get_checkpoint_location(backup_conn);
		}
		/*
		 * For lsn from pg_stop_backup() we need it only to be received by
		 * replica and fsync()'ed on WAL segment.
		 */
		else
		{
			PGresult   *res;
			uint32		lsn_hi;
			uint32		lsn_lo;

#if PG_VERSION_NUM >= 100000
			res = pgut_execute(backup_conn, "SELECT pg_catalog.pg_last_wal_receive_lsn()",
							   0, NULL);
#else
			res = pgut_execute(backup_conn, "SELECT pg_catalog.pg_last_xlog_receive_location()",
							   0, NULL);
#endif

			/* Extract LSN from result */
			XLogDataFromLSN(PQgetvalue(res, 0, 0), &lsn_hi, &lsn_lo);
			/* Calculate LSN */
			replica_lsn = ((uint64) lsn_hi) << 32 | lsn_lo;
			PQclear(res);
		}

		/* target lsn was replicated */
		if (replica_lsn >= lsn)
			break;

		sleep(1);
		if (interrupted)
			elog(ERROR, "Interrupted during waiting for target LSN");
		try_count++;

		/* Inform user if target lsn is absent in first attempt */
		if (try_count == 1)
			elog(INFO, "Wait for target LSN %X/%X to be received by replica",
				 (uint32) (lsn >> 32), (uint32) lsn);

		if (instance_config.replica_timeout > 0 &&
			try_count > instance_config.replica_timeout)
			elog(ERROR, "Target LSN %X/%X could not be recevied by replica "
				 "in %d seconds",
				 (uint32) (lsn >> 32), (uint32) lsn,
				 instance_config.replica_timeout);
	}
}

/*
 * Notify end of backup to PostgreSQL server.
 */
static void
pg_stop_backup(pgBackup *backup, PGconn *pg_startbackup_conn)
{
	PGconn		*conn;
	PGresult	*res;
	PGresult	*tablespace_map_content = NULL;
	uint32		lsn_hi;
	uint32		lsn_lo;
	//XLogRecPtr	restore_lsn = InvalidXLogRecPtr;
	int			pg_stop_backup_timeout = 0;
	char		path[MAXPGPATH];
	char		backup_label[MAXPGPATH];
	FILE		*fp;
	pgFile		*file;
	size_t		len;
	char	   *val = NULL;
	char	   *stop_backup_query = NULL;
	bool		stop_lsn_exists = false;

	/*
	 * We will use this values if there are no transactions between start_lsn
	 * and stop_lsn.
	 */
	time_t		recovery_time;
	TransactionId recovery_xid;

	if (!backup_in_progress)
		elog(ERROR, "backup is not in progress");

	conn = pg_startbackup_conn;

	/* Remove annoying NOTICE messages generated by backend */
	res = pgut_execute(conn, "SET client_min_messages = warning;",
					   0, NULL);
	PQclear(res);

	/* Create restore point
	 * only if it`s backup from master, or exclusive replica(wich connects to master)
	 */
	if (backup != NULL && (!current.from_replica || (current.from_replica && exclusive_backup)))
	{
		const char *params[1];
		char		name[1024];

		if (!current.from_replica)
			snprintf(name, lengthof(name), "pg_probackup, backup_id %s",
					 base36enc(backup->start_time));
		else
			snprintf(name, lengthof(name), "pg_probackup, backup_id %s. Replica Backup",
					 base36enc(backup->start_time));
		params[0] = name;

		res = pgut_execute(conn, "SELECT pg_catalog.pg_create_restore_point($1)",
						   1, params);
		/* Extract timeline and LSN from the result */
		XLogDataFromLSN(PQgetvalue(res, 0, 0), &lsn_hi, &lsn_lo);
		/* Calculate LSN */
		//restore_lsn = ((uint64) lsn_hi) << 32 | lsn_lo;
		PQclear(res);
	}

	/*
	 * send pg_stop_backup asynchronously because we could came
	 * here from backup_cleanup() after some error caused by
	 * postgres archive_command problem and in this case we will
	 * wait for pg_stop_backup() forever.
	 */

	if (!pg_stop_backup_is_sent)
	{
		bool		sent = false;

		if (!exclusive_backup)
		{
			/*
			 * Stop the non-exclusive backup. Besides stop_lsn it returns from
			 * pg_stop_backup(false) copy of the backup label and tablespace map
			 * so they can be written to disk by the caller.
			 * In case of backup from replica >= 9.6 we do not trust minRecPoint
			 * and stop_backup LSN, so we use latest replayed LSN as STOP LSN.
			 */
			if (current.from_replica)
				stop_backup_query = "SELECT"
									" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
									" current_timestamp(0)::timestamptz,"
#if PG_VERSION_NUM >= 100000
									" pg_catalog.pg_last_wal_replay_lsn(),"
#else
									" pg_catalog.pg_last_xlog_replay_location(),"
#endif
									" labelfile,"
									" spcmapfile"
#if PG_VERSION_NUM >= 100000
									" FROM pg_catalog.pg_stop_backup(false, false)";
#else
									" FROM pg_catalog.pg_stop_backup(false)";
#endif
			else
				stop_backup_query = "SELECT"
									" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
									" current_timestamp(0)::timestamptz,"
									" lsn,"
									" labelfile,"
									" spcmapfile"
#if PG_VERSION_NUM >= 100000
									" FROM pg_catalog.pg_stop_backup(false, false)";
#else
									" FROM pg_catalog.pg_stop_backup(false)";
#endif

		}
		else
		{
			stop_backup_query =	"SELECT"
								" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
								" current_timestamp(0)::timestamptz,"
								" pg_catalog.pg_stop_backup() as lsn";
		}

		sent = pgut_send(conn, stop_backup_query, 0, NULL, WARNING);
		pg_stop_backup_is_sent = true;
		if (!sent)
			elog(ERROR, "Failed to send pg_stop_backup query");
	}

	/* After we have sent pg_stop_backup, we don't need this callback anymore */
	pgut_atexit_pop(backup_stopbackup_callback, pg_startbackup_conn);

	/*
	 * Wait for the result of pg_stop_backup(), but no longer than
	 * archive_timeout seconds
	 */
	if (pg_stop_backup_is_sent && !in_cleanup)
	{
		res = NULL;

		while (1)
		{
			if (!PQconsumeInput(conn) || PQisBusy(conn))
			{
				pg_stop_backup_timeout++;
				sleep(1);

				if (interrupted)
				{
					pgut_cancel(conn);
					elog(ERROR, "interrupted during waiting for pg_stop_backup");
				}

				if (pg_stop_backup_timeout == 1)
					elog(INFO, "wait for pg_stop_backup()");

				/*
				 * If postgres haven't answered in archive_timeout seconds,
				 * send an interrupt.
				 */
				if (pg_stop_backup_timeout > instance_config.archive_timeout)
				{
					pgut_cancel(conn);
					elog(ERROR, "pg_stop_backup doesn't answer in %d seconds, cancel it",
						 instance_config.archive_timeout);
				}
			}
			else
			{
				res = PQgetResult(conn);
				break;
			}
		}

		/* Check successfull execution of pg_stop_backup() */
		if (!res)
			elog(ERROR, "pg_stop backup() failed");
		else
		{
			switch (PQresultStatus(res))
			{
				/*
				 * We should expect only PGRES_TUPLES_OK since pg_stop_backup
				 * returns tuples.
				 */
				case PGRES_TUPLES_OK:
					break;
				default:
					elog(ERROR, "query failed: %s query was: %s",
						 PQerrorMessage(conn), stop_backup_query);
			}
			elog(INFO, "pg_stop backup() successfully executed");
		}

		backup_in_progress = false;

		/* Extract timeline and LSN from results of pg_stop_backup() */
		XLogDataFromLSN(PQgetvalue(res, 0, 2), &lsn_hi, &lsn_lo);
		/* Calculate LSN */
		stop_backup_lsn = ((uint64) lsn_hi) << 32 | lsn_lo;

		if (!XRecOffIsValid(stop_backup_lsn))
		{
			if (XRecOffIsNull(stop_backup_lsn))
			{
				char	   *xlog_path,
							stream_xlog_path[MAXPGPATH];

				if (stream_wal)
				{
					pgBackupGetPath2(backup, stream_xlog_path,
									 lengthof(stream_xlog_path),
									 DATABASE_DIR, PG_XLOG_DIR);
					xlog_path = stream_xlog_path;
				}
				else
					xlog_path = arclog_path;

				wait_wal_lsn(stop_backup_lsn, false, true);
				stop_backup_lsn = get_last_wal_lsn(xlog_path, backup->start_lsn,
												   stop_backup_lsn, backup->tli,
												   true, instance_config.xlog_seg_size);
				/*
				 * Do not check existance of LSN again below using
				 * wait_wal_lsn().
				 */
				stop_lsn_exists = true;
			}
			else
				elog(ERROR, "Invalid stop_backup_lsn value %X/%X",
					 (uint32) (stop_backup_lsn >> 32), (uint32) (stop_backup_lsn));
		}

		/* Write backup_label and tablespace_map */
		if (!exclusive_backup)
		{
			Assert(PQnfields(res) >= 4);
			pgBackupGetPath(&current, path, lengthof(path), DATABASE_DIR);

			/* Write backup_label */
			join_path_components(backup_label, path, PG_BACKUP_LABEL_FILE);
			fp = fio_fopen(backup_label, PG_BINARY_W, FIO_BACKUP_HOST);
			if (fp == NULL)
				elog(ERROR, "can't open backup label file \"%s\": %s",
					 backup_label, strerror(errno));

			len = strlen(PQgetvalue(res, 0, 3));
			if (fio_fwrite(fp, PQgetvalue(res, 0, 3), len) != len ||
				fio_fflush(fp) != 0 ||
				fio_fclose(fp))
				elog(ERROR, "can't write backup label file \"%s\": %s",
					 backup_label, strerror(errno));

			/*
			 * It's vital to check if backup_files_list is initialized,
			 * because we could get here because the backup was interrupted
			 */
			if (backup_files_list)
			{
				file = pgFileNew(backup_label, PG_BACKUP_LABEL_FILE, true, 0,
								 FIO_BACKUP_HOST);
				file->crc = pgFileGetCRC(file->path, true, false,
										 &file->read_size, FIO_BACKUP_HOST);
				file->write_size = file->read_size;
				free(file->path);
				file->path = strdup(PG_BACKUP_LABEL_FILE);
				parray_append(backup_files_list, file);
			}
		}

		if (sscanf(PQgetvalue(res, 0, 0), XID_FMT, &recovery_xid) != 1)
			elog(ERROR,
				 "result of txid_snapshot_xmax() is invalid: %s",
				 PQgetvalue(res, 0, 0));
		if (!parse_time(PQgetvalue(res, 0, 1), &recovery_time, true))
			elog(ERROR,
				 "result of current_timestamp is invalid: %s",
				 PQgetvalue(res, 0, 1));

		/* Get content for tablespace_map from stop_backup results
		 * in case of non-exclusive backup
		 */
		if (!exclusive_backup)
			val = PQgetvalue(res, 0, 4);

		/* Write tablespace_map */
		if (!exclusive_backup && val && strlen(val) > 0)
		{
			char		tablespace_map[MAXPGPATH];

			join_path_components(tablespace_map, path, PG_TABLESPACE_MAP_FILE);
			fp = fio_fopen(tablespace_map, PG_BINARY_W, FIO_BACKUP_HOST);
			if (fp == NULL)
				elog(ERROR, "can't open tablespace map file \"%s\": %s",
					 tablespace_map, strerror(errno));

			len = strlen(val);
			if (fio_fwrite(fp, val, len) != len ||
				fio_fflush(fp) != 0 ||
				fio_fclose(fp))
				elog(ERROR, "can't write tablespace map file \"%s\": %s",
					 tablespace_map, strerror(errno));

			if (backup_files_list)
			{
				file = pgFileNew(tablespace_map, PG_TABLESPACE_MAP_FILE, true, 0,
								 FIO_BACKUP_HOST);
				if (S_ISREG(file->mode))
				{
					file->crc = pgFileGetCRC(file->path, true, false,
											 &file->read_size, FIO_BACKUP_HOST);
					file->write_size = file->read_size;
				}
				free(file->path);
				file->path = strdup(PG_TABLESPACE_MAP_FILE);
				parray_append(backup_files_list, file);
			}
		}

		if (tablespace_map_content)
			PQclear(tablespace_map_content);
		PQclear(res);

		if (stream_wal)
		{
			/* Wait for the completion of stream */
			pthread_join(stream_thread, NULL);
			if (stream_thread_arg.ret == 1)
				elog(ERROR, "WAL streaming failed");
		}
	}

	/* Fill in fields if that is the correct end of backup. */
	if (backup != NULL)
	{
		char	   *xlog_path,
					stream_xlog_path[MAXPGPATH];

		/* Wait for stop_lsn to be received by replica */
		/* XXX Do we need this? */
//		if (current.from_replica)
//			wait_replica_wal_lsn(stop_backup_lsn, false);
		/*
		 * Wait for stop_lsn to be archived or streamed.
		 * We wait for stop_lsn in stream mode just in case.
		 */
		if (!stop_lsn_exists)
			stop_backup_lsn = wait_wal_lsn(stop_backup_lsn, false, false);

		if (stream_wal)
		{
			pgBackupGetPath2(backup, stream_xlog_path,
							 lengthof(stream_xlog_path),
							 DATABASE_DIR, PG_XLOG_DIR);
			xlog_path = stream_xlog_path;
		}
		else
			xlog_path = arclog_path;

		backup->tli = get_current_timeline(false);
		backup->stop_lsn = stop_backup_lsn;

		elog(LOG, "Getting the Recovery Time from WAL");

		/* iterate over WAL from stop_backup lsn to start_backup lsn */
		if (!read_recovery_info(xlog_path, backup->tli,
								instance_config.xlog_seg_size,
								backup->start_lsn, backup->stop_lsn,
								&backup->recovery_time, &backup->recovery_xid))
		{
			elog(LOG, "Failed to find Recovery Time in WAL. Forced to trust current_timestamp");
			backup->recovery_time = recovery_time;
			backup->recovery_xid = recovery_xid;
		}
	}
}

/*
 * Retreive checkpoint_timeout GUC value in seconds.
 */
static int
checkpoint_timeout(PGconn *backup_conn)
{
	PGresult   *res;
	const char *val;
	const char *hintmsg;
	int			val_int;

	res = pgut_execute(backup_conn, "show checkpoint_timeout", 0, NULL);
	val = PQgetvalue(res, 0, 0);

	if (!parse_int(val, &val_int, OPTION_UNIT_S, &hintmsg))
	{
		PQclear(res);
		if (hintmsg)
			elog(ERROR, "Invalid value of checkout_timeout %s: %s", val,
				 hintmsg);
		else
			elog(ERROR, "Invalid value of checkout_timeout %s", val);
	}

	PQclear(res);

	return val_int;
}

/*
 * Notify end of backup to server when "backup_label" is in the root directory
 * of the DB cluster.
 * Also update backup status to ERROR when the backup is not finished.
 */
static void
backup_cleanup(bool fatal, void *userdata)
{
	/*
	 * Update status of backup in BACKUP_CONTROL_FILE to ERROR.
	 * end_time != 0 means backup finished
	 */
	if (current.status == BACKUP_STATUS_RUNNING && current.end_time == 0)
	{
		elog(WARNING, "Backup %s is running, setting its status to ERROR",
			 base36enc(current.start_time));
		current.end_time = time(NULL);
		current.status = BACKUP_STATUS_ERROR;
		write_backup(&current);
	}
}

/*
 * Take a backup of the PGDATA at a file level.
 * Copy all directories and files listed in backup_files_list.
 * If the file is 'datafile' (regular relation's main fork), read it page by page,
 * verify checksum and copy.
 * In incremental backup mode, copy only files or datafiles' pages changed after
 * previous backup.
 */
static void *
backup_files(void *arg)
{
	int			i;
	backup_files_arg *arguments = (backup_files_arg *) arg;
	int			n_backup_files_list = parray_num(arguments->files_list);
	static time_t prev_time;

	prev_time = current.start_time;

	/* backup a file */
	for (i = 0; i < n_backup_files_list; i++)
	{
		int			ret;
		struct stat	buf;
		pgFile	   *file = (pgFile *) parray_get(arguments->files_list, i);

		if (arguments->thread_num == 1)
		{
			/* update backup_content.control every 10 seconds */
			if ((difftime(time(NULL), prev_time)) > 10)
			{
				prev_time = time(NULL);

				write_backup_filelist(&current, arguments->files_list, arguments->from_root,
									  arguments->external_dirs);
				/* update backup control file to update size info */
				write_backup(&current);
			}
		}

		if (!pg_atomic_test_set_flag(&file->lock))
			continue;
		elog(VERBOSE, "Copying file:  \"%s\" ", file->path);

		/* check for interrupt */
		if (interrupted || thread_interrupted)
			elog(ERROR, "interrupted during backup");

		if (progress)
			elog(INFO, "Progress: (%d/%d). Process file \"%s\"",
				 i + 1, n_backup_files_list, file->path);

		/* stat file to check its current state */
		ret = fio_stat(file->path, &buf, true, FIO_DB_HOST);
		if (ret == -1)
		{
			if (errno == ENOENT)
			{
				/*
				 * If file is not found, this is not en error.
				 * It could have been deleted by concurrent postgres transaction.
				 */
				file->write_size = FILE_NOT_FOUND;
				elog(LOG, "File \"%s\" is not found", file->path);
				continue;
			}
			else
			{
				elog(ERROR,
					"can't stat file to backup \"%s\": %s",
					file->path, strerror(errno));
			}
		}

		/* We have already copied all directories */
		if (S_ISDIR(buf.st_mode))
			continue;

		if (S_ISREG(buf.st_mode))
		{
			pgFile	  **prev_file = NULL;
			char	   *external_path = NULL;

			if (file->external_dir_num)
				external_path = parray_get(arguments->external_dirs,
										file->external_dir_num - 1);

			/* Check that file exist in previous backup */
			if (current.backup_mode != BACKUP_MODE_FULL)
			{
				char	   *relative;
				pgFile		key;

				relative = GetRelativePath(file->path, file->external_dir_num ?
										   external_path : arguments->from_root);
				key.path = relative;
				key.external_dir_num = file->external_dir_num;

				prev_file = (pgFile **) parray_bsearch(arguments->prev_filelist,
											&key, pgFileComparePathWithExternal);
				if (prev_file)
					/* File exists in previous backup */
					file->exists_in_prev = true;
			}

			/* copy the file into backup */
			if (file->is_datafile && !file->is_cfs)
			{
				char		to_path[MAXPGPATH];

				join_path_components(to_path, arguments->to_root,
									 file->path + strlen(arguments->from_root) + 1);

				/* backup block by block if datafile AND not compressed by cfs*/
				if (!backup_data_file(arguments, to_path, file,
									  arguments->prev_start_lsn,
									  current.backup_mode,
									  instance_config.compress_alg,
									  instance_config.compress_level,
									  true))
				{
					/* disappeared file not to be confused with 'not changed' */
					if (file->write_size != FILE_NOT_FOUND)
						file->write_size = BYTES_INVALID;
					elog(VERBOSE, "File \"%s\" was not copied to backup", file->path);
					continue;
				}
			}
			else if (!file->external_dir_num &&
					 strcmp(file->name, "pg_control") == 0)
				copy_pgcontrol_file(arguments->from_root, FIO_DB_HOST,
									arguments->to_root, FIO_BACKUP_HOST,
									file);
			else
			{
				const char *dst;
				bool		skip = false;
				char		external_dst[MAXPGPATH];

				/* If non-data file has not changed since last backup... */
				if (prev_file && file->exists_in_prev &&
					buf.st_mtime < current.parent_backup)
				{
					file->crc = pgFileGetCRC(file->path, true, false,
											 &file->read_size, FIO_DB_HOST);
					file->write_size = file->read_size;
					/* ...and checksum is the same... */
					if (EQ_TRADITIONAL_CRC32(file->crc, (*prev_file)->crc))
						skip = true; /* ...skip copying file. */
				}
				/* Set file paths */
				if (file->external_dir_num)
				{
					makeExternalDirPathByNum(external_dst,
											 arguments->external_prefix,
											 file->external_dir_num);
					dst = external_dst;
				}
				else
					dst = arguments->to_root;
				if (skip ||
					!copy_file(FIO_DB_HOST, dst, FIO_BACKUP_HOST, file, true))
				{
					/* disappeared file not to be confused with 'not changed' */
					if (file->write_size != FILE_NOT_FOUND)
						file->write_size = BYTES_INVALID;
					elog(VERBOSE, "File \"%s\" was not copied to backup",
						 file->path);
					continue;
				}
			}

			elog(VERBOSE, "File \"%s\". Copied "INT64_FORMAT " bytes",
				 file->path, file->write_size);
		}
		else
			elog(WARNING, "unexpected file type %d", buf.st_mode);
	}

	/* Close connection */
	if (arguments->conn_arg.conn)
		pgut_disconnect(arguments->conn_arg.conn);

	/* Data files transferring is successful */
	arguments->ret = 0;

	return NULL;
}

/*
 * Extract information about files in backup_list parsing their names:
 * - remove temp tables from the list
 * - remove unlogged tables from the list (leave the _init fork)
 * - set flags for database directories
 * - set flags for datafiles
 */
void
parse_filelist_filenames(parray *files, const char *root)
{
	size_t		i = 0;
	Oid			unlogged_file_reloid = 0;

	while (i < parray_num(files))
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);
		char	   *relative;
		int 		sscanf_result;

		relative = GetRelativePath(file->path, root);

		if (S_ISREG(file->mode) &&
			path_is_prefix_of_path(PG_TBLSPC_DIR, relative))
		{
			/*
			 * Found file in pg_tblspc/tblsOid/TABLESPACE_VERSION_DIRECTORY
			 * Legal only in case of 'pg_compression'
			 */
			if (strcmp(file->name, "pg_compression") == 0)
			{
				Oid			tblspcOid;
				Oid			dbOid;
				char		tmp_rel_path[MAXPGPATH];
				/*
				 * Check that the file is located under
				 * TABLESPACE_VERSION_DIRECTORY
				 */
				sscanf_result = sscanf(relative, PG_TBLSPC_DIR "/%u/%s/%u",
									   &tblspcOid, tmp_rel_path, &dbOid);

				/* Yes, it is */
				if (sscanf_result == 2 &&
					strncmp(tmp_rel_path, TABLESPACE_VERSION_DIRECTORY,
							strlen(TABLESPACE_VERSION_DIRECTORY)) == 0)
					set_cfs_datafiles(files, root, relative, i);
			}
		}

		if (S_ISREG(file->mode) && file->tblspcOid != 0 &&
			file->name && file->name[0])
		{
			if (strcmp(file->forkName, "init") == 0)
			{
				/*
				 * Do not backup files of unlogged relations.
				 * scan filelist backward and exclude these files.
				 */
				int			unlogged_file_num = i - 1;
				pgFile	   *unlogged_file = (pgFile *) parray_get(files,
																  unlogged_file_num);

				unlogged_file_reloid = file->relOid;

				while (unlogged_file_num >= 0 &&
					   (unlogged_file_reloid != 0) &&
					   (unlogged_file->relOid == unlogged_file_reloid))
				{
					pgFileFree(unlogged_file);
					parray_remove(files, unlogged_file_num);

					unlogged_file_num--;
					i--;

					unlogged_file = (pgFile *) parray_get(files,
														  unlogged_file_num);
				}
			}
		}

		i++;
	}
}

/* If file is equal to pg_compression, then we consider this tablespace as
 * cfs-compressed and should mark every file in this tablespace as cfs-file
 * Setting is_cfs is done via going back through 'files' set every file
 * that contain cfs_tablespace in his path as 'is_cfs'
 * Goings back through array 'files' is valid option possible because of current
 * sort rules:
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/dboid
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/dboid/1
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/dboid/1.cfm
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/pg_compression
 */
static void
set_cfs_datafiles(parray *files, const char *root, char *relative, size_t i)
{
	int			len;
	int			p;
	pgFile	   *prev_file;
	char	   *cfs_tblspc_path;
	char	   *relative_prev_file;

	cfs_tblspc_path = strdup(relative);
	if(!cfs_tblspc_path)
		elog(ERROR, "Out of memory");
	len = strlen("/pg_compression");
	cfs_tblspc_path[strlen(cfs_tblspc_path) - len] = 0;
	elog(VERBOSE, "CFS DIRECTORY %s, pg_compression path: %s", cfs_tblspc_path, relative);

	for (p = (int) i; p >= 0; p--)
	{
		prev_file = (pgFile *) parray_get(files, (size_t) p);
		relative_prev_file = GetRelativePath(prev_file->path, root);

		elog(VERBOSE, "Checking file in cfs tablespace %s", relative_prev_file);

		if (strstr(relative_prev_file, cfs_tblspc_path) != NULL)
		{
			if (S_ISREG(prev_file->mode) && prev_file->is_datafile)
			{
				elog(VERBOSE, "Setting 'is_cfs' on file %s, name %s",
					relative_prev_file, prev_file->name);
				prev_file->is_cfs = true;
			}
		}
		else
		{
			elog(VERBOSE, "Breaking on %s", relative_prev_file);
			break;
		}
	}
	free(cfs_tblspc_path);
}

/*
 * Find pgfile by given rnode in the backup_files_list
 * and add given blkno to its pagemap.
 */
void
process_block_change(ForkNumber forknum, RelFileNode rnode, BlockNumber blkno)
{
	char	   *path;
	char	   *rel_path;
	BlockNumber blkno_inseg;
	int			segno;
	pgFile	  **file_item;
	pgFile		f;

	segno = blkno / RELSEG_SIZE;
	blkno_inseg = blkno % RELSEG_SIZE;

	rel_path = relpathperm(rnode, forknum);
	if (segno > 0)
		path = psprintf("%s/%s.%u", instance_config.pgdata, rel_path, segno);
	else
		path = psprintf("%s/%s", instance_config.pgdata, rel_path);

	pg_free(rel_path);

	f.path = path;
	/* backup_files_list should be sorted before */
	file_item = (pgFile **) parray_bsearch(backup_files_list, &f,
										   pgFileComparePath);

	/*
	 * If we don't have any record of this file in the file map, it means
	 * that it's a relation that did not have much activity since the last
	 * backup. We can safely ignore it. If it is a new relation file, the
	 * backup would simply copy it as-is.
	 */
	if (file_item)
	{
		/* We need critical section only we use more than one threads */
		if (num_threads > 1)
			pthread_lock(&backup_pagemap_mutex);

		datapagemap_add(&(*file_item)->pagemap, blkno_inseg);

		if (num_threads > 1)
			pthread_mutex_unlock(&backup_pagemap_mutex);
	}

	pg_free(path);
}

/*
 * Given a list of files in the instance to backup, build a pagemap for each
 * data file that has ptrack. Result is saved in the pagemap field of pgFile.
 * NOTE we rely on the fact that provided parray is sorted by file->path.
 */
static void
make_pagemap_from_ptrack(parray *files, PGconn *backup_conn)
{
	size_t		i;
	Oid dbOid_with_ptrack_init = 0;
	Oid tblspcOid_with_ptrack_init = 0;
	char	   *ptrack_nonparsed = NULL;
	size_t		ptrack_nonparsed_size = 0;

	elog(LOG, "Compiling pagemap");
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);
		size_t		start_addr;

		/*
		 * If there is a ptrack_init file in the database,
		 * we must backup all its files, ignoring ptrack files for relations.
		 */
		if (file->is_database)
		{
			char *filename = strrchr(file->path, '/');

			Assert(filename != NULL);
			filename++;

			/*
			 * The function pg_ptrack_get_and_clear_db returns true
			 * if there was a ptrack_init file.
			 * Also ignore ptrack files for global tablespace,
			 * to avoid any possible specific errors.
			 */
			if ((file->tblspcOid == GLOBALTABLESPACE_OID) ||
				pg_ptrack_get_and_clear_db(file->dbOid, file->tblspcOid, backup_conn))
			{
				dbOid_with_ptrack_init = file->dbOid;
				tblspcOid_with_ptrack_init = file->tblspcOid;
			}
		}

		if (file->is_datafile)
		{
			if (file->tblspcOid == tblspcOid_with_ptrack_init &&
				file->dbOid == dbOid_with_ptrack_init)
			{
				/* ignore ptrack if ptrack_init exists */
				elog(VERBOSE, "Ignoring ptrack because of ptrack_init for file: %s", file->path);
				file->pagemap_isabsent = true;
				continue;
			}

			/* get ptrack bitmap once for all segments of the file */
			if (file->segno == 0)
			{
				/* release previous value */
				pg_free(ptrack_nonparsed);
				ptrack_nonparsed_size = 0;

				ptrack_nonparsed = pg_ptrack_get_and_clear(file->tblspcOid, file->dbOid,
											   file->relOid, &ptrack_nonparsed_size, backup_conn);
			}

			if (ptrack_nonparsed != NULL)
			{
				/*
				 * pg_ptrack_get_and_clear() returns ptrack with VARHDR cutted out.
				 * Compute the beginning of the ptrack map related to this segment
				 *
				 * HEAPBLOCKS_PER_BYTE. Number of heap pages one ptrack byte can track: 8
				 * RELSEG_SIZE. Number of Pages per segment: 131072
				 * RELSEG_SIZE/HEAPBLOCKS_PER_BYTE. number of bytes in ptrack file needed
				 * to keep track on one relsegment: 16384
				 */
				start_addr = (RELSEG_SIZE/HEAPBLOCKS_PER_BYTE)*file->segno;

				/*
				 * If file segment was created after we have read ptrack,
				 * we won't have a bitmap for this segment.
				 */
				if (start_addr > ptrack_nonparsed_size)
				{
					elog(VERBOSE, "Ptrack is missing for file: %s", file->path);
					file->pagemap_isabsent = true;
				}
				else
				{

					if (start_addr + RELSEG_SIZE/HEAPBLOCKS_PER_BYTE > ptrack_nonparsed_size)
					{
						file->pagemap.bitmapsize = ptrack_nonparsed_size - start_addr;
						elog(VERBOSE, "pagemap size: %i", file->pagemap.bitmapsize);
					}
					else
					{
						file->pagemap.bitmapsize = RELSEG_SIZE/HEAPBLOCKS_PER_BYTE;
						elog(VERBOSE, "pagemap size: %i", file->pagemap.bitmapsize);
					}

					file->pagemap.bitmap = pg_malloc(file->pagemap.bitmapsize);
					memcpy(file->pagemap.bitmap, ptrack_nonparsed+start_addr, file->pagemap.bitmapsize);
				}
			}
			else
			{
				/*
				 * If ptrack file is missing, try to copy the entire file.
				 * It can happen in two cases:
				 * - files were created by commands that bypass buffer manager
				 * and, correspondingly, ptrack mechanism.
				 * i.e. CREATE DATABASE
				 * - target relation was deleted.
				 */
				elog(VERBOSE, "Ptrack is missing for file: %s", file->path);
				file->pagemap_isabsent = true;
			}
		}
	}
	elog(LOG, "Pagemap compiled");
}


/*
 * Stop WAL streaming if current 'xlogpos' exceeds 'stop_backup_lsn', which is
 * set by pg_stop_backup().
 */
static bool
stop_streaming(XLogRecPtr xlogpos, uint32 timeline, bool segment_finished)
{
	static uint32 prevtimeline = 0;
	static XLogRecPtr prevpos = InvalidXLogRecPtr;

	/* check for interrupt */
	if (interrupted || thread_interrupted)
		elog(ERROR, "Interrupted during backup stop_streaming");

	/* we assume that we get called once at the end of each segment */
	if (segment_finished)
		elog(VERBOSE, _("finished segment at %X/%X (timeline %u)"),
			 (uint32) (xlogpos >> 32), (uint32) xlogpos, timeline);

	/*
	 * Note that we report the previous, not current, position here. After a
	 * timeline switch, xlogpos points to the beginning of the segment because
	 * that's where we always begin streaming. Reporting the end of previous
	 * timeline isn't totally accurate, because the next timeline can begin
	 * slightly before the end of the WAL that we received on the previous
	 * timeline, but it's close enough for reporting purposes.
	 */
	if (prevtimeline != 0 && prevtimeline != timeline)
		elog(LOG, _("switched to timeline %u at %X/%X\n"),
			 timeline, (uint32) (prevpos >> 32), (uint32) prevpos);

	if (!XLogRecPtrIsInvalid(stop_backup_lsn))
	{
		if (xlogpos >= stop_backup_lsn)
		{
			stop_stream_lsn = xlogpos;
			return true;
		}

		/* pg_stop_backup() was executed, wait for the completion of stream */
		if (stream_stop_begin == 0)
		{
			elog(INFO, "Wait for LSN %X/%X to be streamed",
				 (uint32) (stop_backup_lsn >> 32), (uint32) stop_backup_lsn);

			stream_stop_begin = time(NULL);
		}

		if (time(NULL) - stream_stop_begin > stream_stop_timeout)
			elog(ERROR, "Target LSN %X/%X could not be streamed in %d seconds",
				 (uint32) (stop_backup_lsn >> 32), (uint32) stop_backup_lsn,
				 stream_stop_timeout);
	}

	prevtimeline = timeline;
	prevpos = xlogpos;

	return false;
}

/*
 * Start the log streaming
 */
static void *
StreamLog(void *arg)
{
	StreamThreadArg *stream_arg = (StreamThreadArg *) arg;

	/*
	 * Always start streaming at the beginning of a segment
	 */
	stream_arg->startpos -= stream_arg->startpos % instance_config.xlog_seg_size;

	/* Initialize timeout */
	stream_stop_timeout = 0;
	stream_stop_begin = 0;

#if PG_VERSION_NUM >= 100000
	/* if slot name was not provided for temp slot, use default slot name */
	if (!replication_slot && temp_slot)
		replication_slot = "pg_probackup_slot";
#endif


#if PG_VERSION_NUM >= 110000
	/* Create temp repslot */
	if (temp_slot)
		CreateReplicationSlot(stream_arg->conn, replication_slot,
			NULL, temp_slot, true, true, false);
#endif

	/*
	 * Start the replication
	 */
	elog(LOG, _("started streaming WAL at %X/%X (timeline %u)"),
		 (uint32) (stream_arg->startpos >> 32), (uint32) stream_arg->startpos,
		  stream_arg->starttli);

#if PG_VERSION_NUM >= 90600
	{
		StreamCtl	ctl;

		MemSet(&ctl, 0, sizeof(ctl));

		ctl.startpos = stream_arg->startpos;
		ctl.timeline = stream_arg->starttli;
		ctl.sysidentifier = NULL;

#if PG_VERSION_NUM >= 100000
		ctl.walmethod = CreateWalDirectoryMethod(stream_arg->basedir, 0, true);
		ctl.replication_slot = replication_slot;
		ctl.stop_socket = PGINVALID_SOCKET;
#if PG_VERSION_NUM >= 100000 && PG_VERSION_NUM < 110000
		ctl.temp_slot = temp_slot;
#endif
#else
		ctl.basedir = (char *) stream_arg->basedir;
#endif

		ctl.stream_stop = stop_streaming;
		ctl.standby_message_timeout = standby_message_timeout;
		ctl.partial_suffix = NULL;
		ctl.synchronous = false;
		ctl.mark_done = false;

		if(ReceiveXlogStream(stream_arg->conn, &ctl) == false)
			elog(ERROR, "Problem in receivexlog");

#if PG_VERSION_NUM >= 100000
		if (!ctl.walmethod->finish())
			elog(ERROR, "Could not finish writing WAL files: %s",
				 strerror(errno));
#endif
	}
#else
	if(ReceiveXlogStream(stream_arg->conn, stream_arg->startpos, stream_arg->starttli, NULL,
						 (char *) stream_arg->basedir, stop_streaming,
						 standby_message_timeout, NULL, false, false) == false)
		elog(ERROR, "Problem in receivexlog");
#endif

	elog(LOG, _("finished streaming WAL at %X/%X (timeline %u)"),
		 (uint32) (stop_stream_lsn >> 32), (uint32) stop_stream_lsn, stream_arg->starttli);
	stream_arg->ret = 0;

	PQfinish(stream_arg->conn);
	stream_arg->conn = NULL;

	return NULL;
}

/*
 * Get lsn of the moment when ptrack was enabled the last time.
 */
static XLogRecPtr
get_last_ptrack_lsn(PGconn *backup_conn)

{
	PGresult   *res;
	uint32		lsn_hi;
	uint32		lsn_lo;
	XLogRecPtr	lsn;

	res = pgut_execute(backup_conn, "select pg_catalog.pg_ptrack_control_lsn()",
					   0, NULL);

	/* Extract timeline and LSN from results of pg_start_backup() */
	XLogDataFromLSN(PQgetvalue(res, 0, 0), &lsn_hi, &lsn_lo);
	/* Calculate LSN */
	lsn = ((uint64) lsn_hi) << 32 | lsn_lo;

	PQclear(res);
	return lsn;
}

char *
pg_ptrack_get_block(ConnectionArgs *arguments,
					Oid dbOid,
					Oid tblsOid,
					Oid relOid,
					BlockNumber blknum,
					size_t *result_size)
{
	PGresult   *res;
	char	   *params[4];
	char	   *result;

	params[0] = palloc(64);
	params[1] = palloc(64);
	params[2] = palloc(64);
	params[3] = palloc(64);

	/*
	 * Use tmp_conn, since we may work in parallel threads.
	 * We can connect to any database.
	 */
	sprintf(params[0], "%i", tblsOid);
	sprintf(params[1], "%i", dbOid);
	sprintf(params[2], "%i", relOid);
	sprintf(params[3], "%u", blknum);

	if (arguments->conn == NULL)
	{
		arguments->conn = pgut_connect(instance_config.conn_opt.pghost,
											  instance_config.conn_opt.pgport,
											  instance_config.conn_opt.pgdatabase,
											  instance_config.conn_opt.pguser);
	}

	if (arguments->cancel_conn == NULL)
		arguments->cancel_conn = PQgetCancel(arguments->conn);

	//elog(LOG, "db %i pg_ptrack_get_block(%i, %i, %u)",dbOid, tblsOid, relOid, blknum);
	res = pgut_execute_parallel(arguments->conn,
								arguments->cancel_conn,
					"SELECT pg_catalog.pg_ptrack_get_block_2($1, $2, $3, $4)",
					4, (const char **)params, true, false, false);

	if (PQnfields(res) != 1)
	{
		elog(VERBOSE, "cannot get file block for relation oid %u",
					   relOid);
		return NULL;
	}

	if (PQgetisnull(res, 0, 0))
	{
		elog(VERBOSE, "cannot get file block for relation oid %u",
				   relOid);
		return NULL;
	}

	result = (char *) PQunescapeBytea((unsigned char *) PQgetvalue(res, 0, 0),
									  result_size);

	PQclear(res);

	pfree(params[0]);
	pfree(params[1]);
	pfree(params[2]);
	pfree(params[3]);

	return result;
}

static void
check_external_for_tablespaces(parray *external_list, PGconn *backup_conn)
{
	PGresult   *res;
	int			i = 0;
	int			j = 0;
	char	   *tablespace_path = NULL;
	char	   *query = "SELECT pg_catalog.pg_tablespace_location(oid) "
						"FROM pg_catalog.pg_tablespace "
						"WHERE pg_catalog.pg_tablespace_location(oid) <> '';";

	res = pgut_execute(backup_conn, query, 0, NULL);

	/* Check successfull execution of query */
	if (!res)
		elog(ERROR, "Failed to get list of tablespaces");

	for (i = 0; i < res->ntups; i++)
	{
		tablespace_path = PQgetvalue(res, i, 0);
		Assert (strlen(tablespace_path) > 0);

		canonicalize_path(tablespace_path);

		for (j = 0; j < parray_num(external_list); j++)
		{
			char *external_path = parray_get(external_list, j);

			if (path_is_prefix_of_path(external_path, tablespace_path))
				elog(ERROR, "External directory path (-E option) \"%s\" "
							"contains tablespace \"%s\"",
							external_path, tablespace_path);
			if (path_is_prefix_of_path(tablespace_path, external_path))
				elog(WARNING, "External directory path (-E option) \"%s\" "
							  "is in tablespace directory \"%s\"",
							  tablespace_path, external_path);
		}
	}
	PQclear(res);

	/* Check that external directories do not overlap */
	if (parray_num(external_list) < 2)
		return;

	for (i = 0; i < parray_num(external_list); i++)
	{
		char *external_path = parray_get(external_list, i);

		for (j = 0; j < parray_num(external_list); j++)
		{
			char *tmp_external_path = parray_get(external_list, j);

			/* skip yourself */
			if (j == i)
				continue;

			if (path_is_prefix_of_path(external_path, tmp_external_path))
				elog(ERROR, "External directory path (-E option) \"%s\" "
							"contain another external directory \"%s\"",
							external_path, tmp_external_path);

		}
	}
}
