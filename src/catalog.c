/*-------------------------------------------------------------------------
 *
 * catalog.c: backup catalog operation
 *
 * Portions Copyright (c) 2009-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/file.h"
#include "utils/configuration.h"

static const char *backupModes[] = {"", "PAGE", "PTRACK", "DELTA", "FULL"};
static pgBackup *readBackupControlFile(const char *path);

static bool exit_hook_registered = false;
static parray *lock_files = NULL;

static void
unlink_lock_atexit(void)
{
	int			i;

	if (lock_files == NULL)
		return;

	for (i = 0; i < parray_num(lock_files); i++)
	{
		char	   *lock_file = (char *) parray_get(lock_files, i);
		int			res;

		res = fio_unlink(lock_file, FIO_BACKUP_HOST);
		if (res != 0 && errno != ENOENT)
			elog(WARNING, "%s: %s", lock_file, strerror(errno));
	}

	parray_walk(lock_files, pfree);
	parray_free(lock_files);
	lock_files = NULL;
}

/*
 * Read backup meta information from BACKUP_CONTROL_FILE.
 * If no backup matches, return NULL.
 */
pgBackup *
read_backup(time_t timestamp)
{
	pgBackup	tmp;
	char		conf_path[MAXPGPATH];

	tmp.start_time = timestamp;
	pgBackupGetPath(&tmp, conf_path, lengthof(conf_path), BACKUP_CONTROL_FILE);

	return readBackupControlFile(conf_path);
}

/*
 * Save the backup status into BACKUP_CONTROL_FILE.
 *
 * We need to reread the backup using its ID and save it changing only its
 * status.
 */
void
write_backup_status(pgBackup *backup, BackupStatus status)
{
	pgBackup   *tmp;

	tmp = read_backup(backup->start_time);
	if (!tmp)
	{
		/*
		 * Silently exit the function, since read_backup already logged the
		 * warning message.
		 */
		return;
	}

	backup->status = status;
	tmp->status = backup->status;
	write_backup(tmp);

	pgBackupFree(tmp);
}

/*
 * Create exclusive lockfile in the backup's directory.
 */
bool
lock_backup(pgBackup *backup)
{
	char		lock_file[MAXPGPATH];
	int			fd;
	char		buffer[MAXPGPATH * 2 + 256];
	int			ntries;
	int			len;
	int			encoded_pid;
	pid_t		my_pid,
				my_p_pid;

	pgBackupGetPath(backup, lock_file, lengthof(lock_file), BACKUP_CATALOG_PID);

	/*
	 * If the PID in the lockfile is our own PID or our parent's or
	 * grandparent's PID, then the file must be stale (probably left over from
	 * a previous system boot cycle).  We need to check this because of the
	 * likelihood that a reboot will assign exactly the same PID as we had in
	 * the previous reboot, or one that's only one or two counts larger and
	 * hence the lockfile's PID now refers to an ancestor shell process.  We
	 * allow pg_ctl to pass down its parent shell PID (our grandparent PID)
	 * via the environment variable PG_GRANDPARENT_PID; this is so that
	 * launching the postmaster via pg_ctl can be just as reliable as
	 * launching it directly.  There is no provision for detecting
	 * further-removed ancestor processes, but if the init script is written
	 * carefully then all but the immediate parent shell will be root-owned
	 * processes and so the kill test will fail with EPERM.  Note that we
	 * cannot get a false negative this way, because an existing postmaster
	 * would surely never launch a competing postmaster or pg_ctl process
	 * directly.
	 */
	my_pid = getpid();
#ifndef WIN32
	my_p_pid = getppid();
#else

	/*
	 * Windows hasn't got getppid(), but doesn't need it since it's not using
	 * real kill() either...
	 */
	my_p_pid = 0;
#endif

	/*
	 * We need a loop here because of race conditions.  But don't loop forever
	 * (for example, a non-writable $backup_instance_path directory might cause a failure
	 * that won't go away).  100 tries seems like plenty.
	 */
	for (ntries = 0;; ntries++)
	{
		/*
		 * Try to create the lock file --- O_EXCL makes this atomic.
		 *
		 * Think not to make the file protection weaker than 0600.  See
		 * comments below.
		 */
		fd = fio_open(lock_file, O_RDWR | O_CREAT | O_EXCL, FIO_BACKUP_HOST);
		if (fd >= 0)
			break;				/* Success; exit the retry loop */

		/*
		 * Couldn't create the pid file. Probably it already exists.
		 */
		if ((errno != EEXIST && errno != EACCES) || ntries > 100)
			elog(ERROR, "Could not create lock file \"%s\": %s",
				 lock_file, strerror(errno));

		/*
		 * Read the file to get the old owner's PID.  Note race condition
		 * here: file might have been deleted since we tried to create it.
		 */
		fd = fio_open(lock_file, O_RDONLY, FIO_BACKUP_HOST);
		if (fd < 0)
		{
			if (errno == ENOENT)
				continue;		/* race condition; try again */
			elog(ERROR, "Could not open lock file \"%s\": %s",
				 lock_file, strerror(errno));
		}
		if ((len = fio_read(fd, buffer, sizeof(buffer) - 1)) < 0)
			elog(ERROR, "Could not read lock file \"%s\": %s",
				 lock_file, strerror(errno));
		fio_close(fd);

		if (len == 0)
			elog(ERROR, "Lock file \"%s\" is empty", lock_file);

		buffer[len] = '\0';
		encoded_pid = atoi(buffer);

		if (encoded_pid <= 0)
			elog(ERROR, "Bogus data in lock file \"%s\": \"%s\"",
				 lock_file, buffer);

		/*
		 * Check to see if the other process still exists
		 *
		 * Per discussion above, my_pid, my_p_pid can be
		 * ignored as false matches.
		 *
		 * Normally kill() will fail with ESRCH if the given PID doesn't
		 * exist.
		 */
		if (encoded_pid != my_pid && encoded_pid != my_p_pid)
		{
			if (kill(encoded_pid, 0) == 0)
			{
				elog(WARNING, "Process %d is using backup %s and still is running",
					 encoded_pid, base36enc(backup->start_time));
				return false;
			}
			else
			{
				if (errno == ESRCH)
					elog(WARNING, "Process %d which used backup %s no longer exists",
						 encoded_pid, base36enc(backup->start_time));
				else
					elog(ERROR, "Failed to send signal 0 to a process %d: %s",
						encoded_pid, strerror(errno));
			}
		}

		/*
		 * Looks like nobody's home.  Unlink the file and try again to create
		 * it.  Need a loop because of possible race condition against other
		 * would-be creators.
		 */
		if (fio_unlink(lock_file, FIO_BACKUP_HOST) < 0)
			elog(ERROR, "Could not remove old lock file \"%s\": %s",
				 lock_file, strerror(errno));
	}

	/*
	 * Successfully created the file, now fill it.
	 */
	snprintf(buffer, sizeof(buffer), "%d\n", my_pid);

	errno = 0;
	if (fio_write(fd, buffer, strlen(buffer)) != strlen(buffer))
	{
		int			save_errno = errno;

		fio_close(fd);
		fio_unlink(lock_file, FIO_BACKUP_HOST);
		/* if write didn't set errno, assume problem is no disk space */
		errno = save_errno ? save_errno : ENOSPC;
		elog(ERROR, "Could not write lock file \"%s\": %s",
			 lock_file, strerror(errno));
	}
	if (fio_flush(fd) != 0)
	{
		int			save_errno = errno;

		fio_close(fd);
		fio_unlink(lock_file, FIO_BACKUP_HOST);
		errno = save_errno;
		elog(ERROR, "Could not write lock file \"%s\": %s",
			 lock_file, strerror(errno));
	}
	if (fio_close(fd) != 0)
	{
		int			save_errno = errno;

		fio_unlink(lock_file, FIO_BACKUP_HOST);
		errno = save_errno;
		elog(ERROR, "Culd not write lock file \"%s\": %s",
			 lock_file, strerror(errno));
	}

	/*
	 * Arrange to unlink the lock file(s) at proc_exit.
	 */
	if (!exit_hook_registered)
	{
		atexit(unlink_lock_atexit);
		exit_hook_registered = true;
	}

	/* Use parray so that the lock files are unlinked in a loop */
	if (lock_files == NULL)
		lock_files = parray_new();
	parray_append(lock_files, pgut_strdup(lock_file));

	return true;
}

/*
 * Get backup_mode in string representation.
 */
const char *
pgBackupGetBackupMode(pgBackup *backup)
{
	return backupModes[backup->backup_mode];
}

static bool
IsDir(const char *dirpath, const char *entry, fio_location location)
{
	char		path[MAXPGPATH];
	struct stat	st;

	snprintf(path, MAXPGPATH, "%s/%s", dirpath, entry);

	return fio_stat(path, &st, false, location) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Create list of backups.
 * If 'requested_backup_id' is INVALID_BACKUP_ID, return list of all backups.
 * The list is sorted in order of descending start time.
 * If valid backup id is passed only matching backup will be added to the list.
 */
parray *
catalog_get_backup_list(time_t requested_backup_id)
{
	DIR		   *data_dir = NULL;
	struct dirent *data_ent = NULL;
	parray	   *backups = NULL;
	int			i;

	/* open backup instance backups directory */
	data_dir = fio_opendir(backup_instance_path, FIO_BACKUP_HOST);
	if (data_dir == NULL)
	{
		elog(WARNING, "cannot open directory \"%s\": %s", backup_instance_path,
			strerror(errno));
		goto err_proc;
	}

	/* scan the directory and list backups */
	backups = parray_new();
	for (; (data_ent = fio_readdir(data_dir)) != NULL; errno = 0)
	{
		char		backup_conf_path[MAXPGPATH];
		char		data_path[MAXPGPATH];
		pgBackup   *backup = NULL;

		/* skip not-directory entries and hidden entries */
		if (!IsDir(backup_instance_path, data_ent->d_name, FIO_BACKUP_HOST)
			|| data_ent->d_name[0] == '.')
			continue;

		/* open subdirectory of specific backup */
		join_path_components(data_path, backup_instance_path, data_ent->d_name);

		/* read backup information from BACKUP_CONTROL_FILE */
		snprintf(backup_conf_path, MAXPGPATH, "%s/%s", data_path, BACKUP_CONTROL_FILE);
		backup = readBackupControlFile(backup_conf_path);

		if (!backup)
		{
			backup = pgut_new(pgBackup);
			pgBackupInit(backup);
			backup->start_time = base36dec(data_ent->d_name);
		}
		else if (strcmp(base36enc(backup->start_time), data_ent->d_name) != 0)
		{
			elog(WARNING, "backup ID in control file \"%s\" doesn't match name of the backup folder \"%s\"",
				 base36enc(backup->start_time), backup_conf_path);
		}

		backup->backup_id = backup->start_time;
		if (requested_backup_id != INVALID_BACKUP_ID
			&& requested_backup_id != backup->start_time)
		{
			pgBackupFree(backup);
			continue;
		}
		parray_append(backups, backup);

		if (errno && errno != ENOENT)
		{
			elog(WARNING, "cannot read data directory \"%s\": %s",
				 data_ent->d_name, strerror(errno));
			goto err_proc;
		}
	}
	if (errno)
	{
		elog(WARNING, "cannot read backup root directory \"%s\": %s",
			backup_instance_path, strerror(errno));
		goto err_proc;
	}

	fio_closedir(data_dir);
	data_dir = NULL;

	parray_qsort(backups, pgBackupCompareIdDesc);

	/* Link incremental backups with their ancestors.*/
	for (i = 0; i < parray_num(backups); i++)
	{
		pgBackup   *curr = parray_get(backups, i);
		pgBackup  **ancestor;
		pgBackup	key;

		if (curr->backup_mode == BACKUP_MODE_FULL)
			continue;

		key.start_time = curr->parent_backup;
		ancestor = (pgBackup **) parray_bsearch(backups, &key,
												pgBackupCompareIdDesc);
		if (ancestor)
			curr->parent_backup_link = *ancestor;
	}

	return backups;

err_proc:
	if (data_dir)
		fio_closedir(data_dir);
	if (backups)
		parray_walk(backups, pgBackupFree);
	parray_free(backups);

	elog(ERROR, "Failed to get backup list");

	return NULL;
}

/*
 * Lock list of backups. Function goes in backward direction.
 */
void
catalog_lock_backup_list(parray *backup_list, int from_idx, int to_idx)
{
	int			start_idx,
				end_idx;
	int			i;

	if (parray_num(backup_list) == 0)
		return;

	start_idx = Max(from_idx, to_idx);
	end_idx = Min(from_idx, to_idx);

	for (i = start_idx; i >= end_idx; i--)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);
		if (!lock_backup(backup))
			elog(ERROR, "Cannot lock backup %s directory",
				 base36enc(backup->start_time));
	}
}

/*
 * Find the last completed backup on given timeline
 */
pgBackup *
catalog_get_last_data_backup(parray *backup_list, TimeLineID tli)
{
	int			i;
	pgBackup   *backup = NULL;

	/* backup_list is sorted in order of descending ID */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		backup = (pgBackup *) parray_get(backup_list, (size_t) i);

		if ((backup->status == BACKUP_STATUS_OK ||
			backup->status == BACKUP_STATUS_DONE) && backup->tli == tli)
			return backup;
	}

	return NULL;
}

/* create backup directory in $BACKUP_PATH */
int
pgBackupCreateDir(pgBackup *backup)
{
	int		i;
	char	path[MAXPGPATH];
	parray *subdirs = parray_new();

	parray_append(subdirs, pg_strdup(DATABASE_DIR));

	/* Add external dirs containers */
	if (backup->external_dir_str)
	{
		parray *external_list;

		external_list = make_external_directory_list(backup->external_dir_str,
													 false);
		for (i = 0; i < parray_num(external_list); i++)
		{
			char		temp[MAXPGPATH];
			/* Numeration of externaldirs starts with 1 */
			makeExternalDirPathByNum(temp, EXTERNAL_DIR, i+1);
			parray_append(subdirs, pg_strdup(temp));
		}
		free_dir_list(external_list);
	}

	pgBackupGetPath(backup, path, lengthof(path), NULL);

	if (!dir_is_empty(path, FIO_BACKUP_HOST))
		elog(ERROR, "backup destination is not empty \"%s\"", path);

	fio_mkdir(path, DIR_PERMISSION, FIO_BACKUP_HOST);

	/* create directories for actual backup files */
	for (i = 0; i < parray_num(subdirs); i++)
	{
		pgBackupGetPath(backup, path, lengthof(path), parray_get(subdirs, i));
		fio_mkdir(path, DIR_PERMISSION, FIO_BACKUP_HOST);
	}

	free_dir_list(subdirs);
	return 0;
}

/*
 * Write information about backup.in to stream "out".
 */
void
pgBackupWriteControl(FILE *out, pgBackup *backup)
{
	char		timestamp[100];

	fio_fprintf(out, "#Configuration\n");
	fio_fprintf(out, "backup-mode = %s\n", pgBackupGetBackupMode(backup));
	fio_fprintf(out, "stream = %s\n", backup->stream ? "true" : "false");
	fio_fprintf(out, "compress-alg = %s\n",
			deparse_compress_alg(backup->compress_alg));
	fio_fprintf(out, "compress-level = %d\n", backup->compress_level);
	fio_fprintf(out, "from-replica = %s\n", backup->from_replica ? "true" : "false");

	fio_fprintf(out, "\n#Compatibility\n");
	fio_fprintf(out, "block-size = %u\n", backup->block_size);
	fio_fprintf(out, "xlog-block-size = %u\n", backup->wal_block_size);
	fio_fprintf(out, "checksum-version = %u\n", backup->checksum_version);
	if (backup->program_version[0] != '\0')
		fio_fprintf(out, "program-version = %s\n", backup->program_version);
	if (backup->server_version[0] != '\0')
		fio_fprintf(out, "server-version = %s\n", backup->server_version);

	fio_fprintf(out, "\n#Result backup info\n");
	fio_fprintf(out, "timelineid = %d\n", backup->tli);
	/* LSN returned by pg_start_backup */
	fio_fprintf(out, "start-lsn = %X/%X\n",
			(uint32) (backup->start_lsn >> 32),
			(uint32) backup->start_lsn);
	/* LSN returned by pg_stop_backup */
	fio_fprintf(out, "stop-lsn = %X/%X\n",
			(uint32) (backup->stop_lsn >> 32),
			(uint32) backup->stop_lsn);

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	fio_fprintf(out, "start-time = '%s'\n", timestamp);
	if (backup->merge_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->merge_time);
		fio_fprintf(out, "merge-time = '%s'\n", timestamp);
	}
	if (backup->end_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->end_time);
		fio_fprintf(out, "end-time = '%s'\n", timestamp);
	}
	fio_fprintf(out, "recovery-xid = " XID_FMT "\n", backup->recovery_xid);
	if (backup->recovery_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->recovery_time);
		fio_fprintf(out, "recovery-time = '%s'\n", timestamp);
	}

	/*
	 * Size of PGDATA directory. The size does not include size of related
	 * WAL segments in archive 'wal' directory.
	 */
	if (backup->data_bytes != BYTES_INVALID)
		fio_fprintf(out, "data-bytes = " INT64_FORMAT "\n", backup->data_bytes);

	if (backup->wal_bytes != BYTES_INVALID)
		fio_fprintf(out, "wal-bytes = " INT64_FORMAT "\n", backup->wal_bytes);

	fio_fprintf(out, "status = %s\n", status2str(backup->status));

	/* 'parent_backup' is set if it is incremental backup */
	if (backup->parent_backup != 0)
		fio_fprintf(out, "parent-backup-id = '%s'\n", base36enc(backup->parent_backup));

	/* print connection info except password */
	if (backup->primary_conninfo)
		fio_fprintf(out, "primary_conninfo = '%s'\n", backup->primary_conninfo);

	/* print external directories list */
	if (backup->external_dir_str)
		fio_fprintf(out, "external-dirs = '%s'\n", backup->external_dir_str);
}

/*
 * Save the backup content into BACKUP_CONTROL_FILE.
 */
void
write_backup(pgBackup *backup)
{
	FILE	   *fp = NULL;
	char		path[MAXPGPATH];
	char		path_temp[MAXPGPATH];
	int			errno_temp;

	pgBackupGetPath(backup, path, lengthof(path), BACKUP_CONTROL_FILE);
	snprintf(path_temp, sizeof(path_temp), "%s.tmp", path);

	fp = fio_fopen(path_temp, PG_BINARY_W, FIO_BACKUP_HOST);
	if (fp == NULL)
		elog(ERROR, "Cannot open configuration file \"%s\": %s",
			 path_temp, strerror(errno));

	pgBackupWriteControl(fp, backup);

	if (fio_fflush(fp) || fio_fclose(fp))
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot write configuration file \"%s\": %s",
			 path_temp, strerror(errno_temp));
	}

	if (fio_rename(path_temp, path, FIO_BACKUP_HOST) < 0)
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot rename configuration file \"%s\" to \"%s\": %s",
			 path_temp, path, strerror(errno_temp));
	}
}

/*
 * Output the list of files to backup catalog DATABASE_FILE_LIST
 */
void
write_backup_filelist(pgBackup *backup, parray *files, const char *root,
					  parray *external_list)
{
	FILE	   *out;
	char		path[MAXPGPATH];
	char		path_temp[MAXPGPATH];
	int			errno_temp;
	size_t		i = 0;
	#define BUFFERSZ BLCKSZ*500
	char		buf[BUFFERSZ];
	size_t		write_len = 0;
	int64 		backup_size_on_disk = 0;

	pgBackupGetPath(backup, path, lengthof(path), DATABASE_FILE_LIST);
	snprintf(path_temp, sizeof(path_temp), "%s.tmp", path);

	out = fio_fopen(path_temp, PG_BINARY_W, FIO_BACKUP_HOST);
	if (out == NULL)
		elog(ERROR, "Cannot open file list \"%s\": %s", path_temp,
			 strerror(errno));

	/* print each file in the list */
	while(i < parray_num(files))
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);
		char	   *path = file->path; /* for streamed WAL files */
		char	line[BLCKSZ];
		int 	len = 0;

		i++;

		if (S_ISDIR(file->mode))
			backup_size_on_disk += 4096;

		/* Count the amount of the data actually copied */
		if (S_ISREG(file->mode) && file->write_size > 0)
			backup_size_on_disk += file->write_size;

		/* for files from PGDATA and external files use rel_path
		 * streamed WAL files has rel_path relative not to "database/"
		 * but to "database/pg_wal", so for them use path.
		 */
		if ((root && strstr(path, root) == path) ||
			(file->external_dir_num && external_list))
				path = file->rel_path;

		len = sprintf(line, "{\"path\":\"%s\", \"size\":\"" INT64_FORMAT "\", "
					 "\"mode\":\"%u\", \"is_datafile\":\"%u\", "
					 "\"is_cfs\":\"%u\", \"crc\":\"%u\", "
					 "\"compress_alg\":\"%s\", \"external_dir_num\":\"%d\"",
					path, file->write_size, file->mode,
					file->is_datafile ? 1 : 0,
					file->is_cfs ? 1 : 0,
					file->crc,
					deparse_compress_alg(file->compress_alg),
					file->external_dir_num);

		if (file->is_datafile)
			len += sprintf(line+len, ",\"segno\":\"%d\"", file->segno);

		if (file->linked)
			len += sprintf(line+len, ",\"linked\":\"%s\"", file->linked);

		if (file->n_blocks != BLOCKNUM_INVALID)
			len += sprintf(line+len, ",\"n_blocks\":\"%i\"", file->n_blocks);

		len += sprintf(line+len, "}\n");

		if (write_len + len <= BUFFERSZ)
		{
			memcpy(buf+write_len, line, len);
			write_len += len;
		}
		else
		{
			/* write buffer to file */
			if (fio_fwrite(out, buf, write_len) != write_len)
			{
				errno_temp = errno;
				fio_unlink(path_temp, FIO_BACKUP_HOST);
				elog(ERROR, "Cannot write file list \"%s\": %s",
					path_temp, strerror(errno));
			}
			/* reset write_len */
			write_len = 0;
		}
	}

	/* write what is left in the buffer to file */
	if (write_len > 0)
		if (fio_fwrite(out, buf, write_len) != write_len)
		{
			errno_temp = errno;
			fio_unlink(path_temp, FIO_BACKUP_HOST);
			elog(ERROR, "Cannot write file list \"%s\": %s",
				path_temp, strerror(errno));
		}

	if (fio_fflush(out) || fio_fclose(out))
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot write file list \"%s\": %s",
			 path_temp, strerror(errno));
	}

	if (fio_rename(path_temp, path, FIO_BACKUP_HOST) < 0)
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot rename configuration file \"%s\" to \"%s\": %s",
			 path_temp, path, strerror(errno_temp));
	}

	/* use extra variable to avoid reset of previous data_bytes value in case of error */
	backup->data_bytes = backup_size_on_disk;
}

/*
 * Read BACKUP_CONTROL_FILE and create pgBackup.
 *  - Comment starts with ';'.
 *  - Do not care section.
 */
static pgBackup *
readBackupControlFile(const char *path)
{
	pgBackup   *backup = pgut_new(pgBackup);
	char	   *backup_mode = NULL;
	char	   *start_lsn = NULL;
	char	   *stop_lsn = NULL;
	char	   *status = NULL;
	char	   *parent_backup = NULL;
	char	   *program_version = NULL;
	char	   *server_version = NULL;
	char	   *compress_alg = NULL;
	int			parsed_options;

	ConfigOption options[] =
	{
		{'s', 0, "backup-mode",			&backup_mode, SOURCE_FILE_STRICT},
		{'u', 0, "timelineid",			&backup->tli, SOURCE_FILE_STRICT},
		{'s', 0, "start-lsn",			&start_lsn, SOURCE_FILE_STRICT},
		{'s', 0, "stop-lsn",			&stop_lsn, SOURCE_FILE_STRICT},
		{'t', 0, "start-time",			&backup->start_time, SOURCE_FILE_STRICT},
		{'t', 0, "merge-time",			&backup->merge_time, SOURCE_FILE_STRICT},
		{'t', 0, "end-time",			&backup->end_time, SOURCE_FILE_STRICT},
		{'U', 0, "recovery-xid",		&backup->recovery_xid, SOURCE_FILE_STRICT},
		{'t', 0, "recovery-time",		&backup->recovery_time, SOURCE_FILE_STRICT},
		{'I', 0, "data-bytes",			&backup->data_bytes, SOURCE_FILE_STRICT},
		{'I', 0, "wal-bytes",			&backup->wal_bytes, SOURCE_FILE_STRICT},
		{'u', 0, "block-size",			&backup->block_size, SOURCE_FILE_STRICT},
		{'u', 0, "xlog-block-size",		&backup->wal_block_size, SOURCE_FILE_STRICT},
		{'u', 0, "checksum-version",	&backup->checksum_version, SOURCE_FILE_STRICT},
		{'s', 0, "program-version",		&program_version, SOURCE_FILE_STRICT},
		{'s', 0, "server-version",		&server_version, SOURCE_FILE_STRICT},
		{'b', 0, "stream",				&backup->stream, SOURCE_FILE_STRICT},
		{'s', 0, "status",				&status, SOURCE_FILE_STRICT},
		{'s', 0, "parent-backup-id",	&parent_backup, SOURCE_FILE_STRICT},
		{'s', 0, "compress-alg",		&compress_alg, SOURCE_FILE_STRICT},
		{'u', 0, "compress-level",		&backup->compress_level, SOURCE_FILE_STRICT},
		{'b', 0, "from-replica",		&backup->from_replica, SOURCE_FILE_STRICT},
		{'s', 0, "primary-conninfo",	&backup->primary_conninfo, SOURCE_FILE_STRICT},
		{'s', 0, "external-dirs",		&backup->external_dir_str, SOURCE_FILE_STRICT},
		{0}
	};

	pgBackupInit(backup);
	if (fio_access(path, F_OK, FIO_BACKUP_HOST) != 0)
	{
		elog(WARNING, "Control file \"%s\" doesn't exist", path);
		pgBackupFree(backup);
		return NULL;
	}

	parsed_options = config_read_opt(path, options, WARNING, true, true);

	if (parsed_options == 0)
	{
		elog(WARNING, "Control file \"%s\" is empty", path);
		pgBackupFree(backup);
		return NULL;
	}

	if (backup->start_time == 0)
	{
		elog(WARNING, "Invalid ID/start-time, control file \"%s\" is corrupted", path);
		pgBackupFree(backup);
		return NULL;
	}

	if (backup_mode)
	{
		backup->backup_mode = parse_backup_mode(backup_mode);
		free(backup_mode);
	}

	if (start_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(start_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->start_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "Invalid START_LSN \"%s\"", start_lsn);
		free(start_lsn);
	}

	if (stop_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(stop_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->stop_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "Invalid STOP_LSN \"%s\"", stop_lsn);
		free(stop_lsn);
	}

	if (status)
	{
		if (strcmp(status, "OK") == 0)
			backup->status = BACKUP_STATUS_OK;
		else if (strcmp(status, "ERROR") == 0)
			backup->status = BACKUP_STATUS_ERROR;
		else if (strcmp(status, "RUNNING") == 0)
			backup->status = BACKUP_STATUS_RUNNING;
		else if (strcmp(status, "MERGING") == 0)
			backup->status = BACKUP_STATUS_MERGING;
		else if (strcmp(status, "DELETING") == 0)
			backup->status = BACKUP_STATUS_DELETING;
		else if (strcmp(status, "DELETED") == 0)
			backup->status = BACKUP_STATUS_DELETED;
		else if (strcmp(status, "DONE") == 0)
			backup->status = BACKUP_STATUS_DONE;
		else if (strcmp(status, "ORPHAN") == 0)
			backup->status = BACKUP_STATUS_ORPHAN;
		else if (strcmp(status, "CORRUPT") == 0)
			backup->status = BACKUP_STATUS_CORRUPT;
		else
			elog(WARNING, "Invalid STATUS \"%s\"", status);
		free(status);
	}

	if (parent_backup)
	{
		backup->parent_backup = base36dec(parent_backup);
		free(parent_backup);
	}

	if (program_version)
	{
		StrNCpy(backup->program_version, program_version,
				sizeof(backup->program_version));
		pfree(program_version);
	}

	if (server_version)
	{
		StrNCpy(backup->server_version, server_version,
				sizeof(backup->server_version));
		pfree(server_version);
	}

	if (compress_alg)
		backup->compress_alg = parse_compress_alg(compress_alg);

	return backup;
}

BackupMode
parse_backup_mode(const char *value)
{
	const char *v = value;
	size_t		len;

	/* Skip all spaces detected */
	while (IsSpace(*v))
		v++;
	len = strlen(v);

	if (len > 0 && pg_strncasecmp("full", v, len) == 0)
		return BACKUP_MODE_FULL;
	else if (len > 0 && pg_strncasecmp("page", v, len) == 0)
		return BACKUP_MODE_DIFF_PAGE;
	else if (len > 0 && pg_strncasecmp("ptrack", v, len) == 0)
		return BACKUP_MODE_DIFF_PTRACK;
	else if (len > 0 && pg_strncasecmp("delta", v, len) == 0)
		return BACKUP_MODE_DIFF_DELTA;

	/* Backup mode is invalid, so leave with an error */
	elog(ERROR, "invalid backup-mode \"%s\"", value);
	return BACKUP_MODE_INVALID;
}

const char *
deparse_backup_mode(BackupMode mode)
{
	switch (mode)
	{
		case BACKUP_MODE_FULL:
			return "full";
		case BACKUP_MODE_DIFF_PAGE:
			return "page";
		case BACKUP_MODE_DIFF_PTRACK:
			return "ptrack";
		case BACKUP_MODE_DIFF_DELTA:
			return "delta";
		case BACKUP_MODE_INVALID:
			return "invalid";
	}

	return NULL;
}

CompressAlg
parse_compress_alg(const char *arg)
{
	size_t		len;

	/* Skip all spaces detected */
	while (isspace((unsigned char)*arg))
		arg++;
	len = strlen(arg);

	if (len == 0)
		elog(ERROR, "compress algrorithm is empty");

	if (pg_strncasecmp("zlib", arg, len) == 0)
		return ZLIB_COMPRESS;
	else if (pg_strncasecmp("pglz", arg, len) == 0)
		return PGLZ_COMPRESS;
	else if (pg_strncasecmp("none", arg, len) == 0)
		return NONE_COMPRESS;
	else
		elog(ERROR, "invalid compress algorithm value \"%s\"", arg);

	return NOT_DEFINED_COMPRESS;
}

const char*
deparse_compress_alg(int alg)
{
	switch (alg)
	{
		case NONE_COMPRESS:
		case NOT_DEFINED_COMPRESS:
			return "none";
		case ZLIB_COMPRESS:
			return "zlib";
		case PGLZ_COMPRESS:
			return "pglz";
	}

	return NULL;
}

/*
 * Fill pgBackup struct with default values.
 */
void
pgBackupInit(pgBackup *backup)
{
	backup->backup_id = INVALID_BACKUP_ID;
	backup->backup_mode = BACKUP_MODE_INVALID;
	backup->status = BACKUP_STATUS_INVALID;
	backup->tli = 0;
	backup->start_lsn = 0;
	backup->stop_lsn = 0;
	backup->start_time = (time_t) 0;
	backup->merge_time = (time_t) 0;
	backup->end_time = (time_t) 0;
	backup->recovery_xid = 0;
	backup->recovery_time = (time_t) 0;

	backup->data_bytes = BYTES_INVALID;
	backup->wal_bytes = BYTES_INVALID;

	backup->compress_alg = COMPRESS_ALG_DEFAULT;
	backup->compress_level = COMPRESS_LEVEL_DEFAULT;

	backup->block_size = BLCKSZ;
	backup->wal_block_size = XLOG_BLCKSZ;
	backup->checksum_version = 0;

	backup->stream = false;
	backup->from_replica = false;
	backup->parent_backup = INVALID_BACKUP_ID;
	backup->parent_backup_link = NULL;
	backup->primary_conninfo = NULL;
	backup->program_version[0] = '\0';
	backup->server_version[0] = '\0';
	backup->external_dir_str = NULL;
}

/* free pgBackup object */
void
pgBackupFree(void *backup)
{
	pgBackup *b = (pgBackup *) backup;

	pfree(b->primary_conninfo);
	pfree(b->external_dir_str);
	pfree(backup);
}

/* Compare two pgBackup with their IDs (start time) in ascending order */
int
pgBackupCompareId(const void *l, const void *r)
{
	pgBackup *lp = *(pgBackup **)l;
	pgBackup *rp = *(pgBackup **)r;

	if (lp->start_time > rp->start_time)
		return 1;
	else if (lp->start_time < rp->start_time)
		return -1;
	else
		return 0;
}

/* Compare two pgBackup with their IDs in descending order */
int
pgBackupCompareIdDesc(const void *l, const void *r)
{
	return -pgBackupCompareId(l, r);
}

/*
 * Construct absolute path of the backup directory.
 * If subdir is not NULL, it will be appended after the path.
 */
void
pgBackupGetPath(const pgBackup *backup, char *path, size_t len, const char *subdir)
{
	pgBackupGetPath2(backup, path, len, subdir, NULL);
}

/*
 * Construct absolute path of the backup directory.
 * Append "subdir1" and "subdir2" to the backup directory.
 */
void
pgBackupGetPath2(const pgBackup *backup, char *path, size_t len,
				 const char *subdir1, const char *subdir2)
{
	/* If "subdir1" is NULL do not check "subdir2" */
	if (!subdir1)
		snprintf(path, len, "%s/%s", backup_instance_path,
				 base36enc(backup->start_time));
	else if (!subdir2)
		snprintf(path, len, "%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1);
	/* "subdir1" and "subdir2" is not NULL */
	else
		snprintf(path, len, "%s/%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1, subdir2);
}

/*
 * Check if multiple backups consider target backup to be their direct parent
 */
bool
is_prolific(parray *backup_list, pgBackup *target_backup)
{
	int i;
	int child_counter = 0;

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *tmp_backup = (pgBackup *) parray_get(backup_list, i);

		/* consider only OK and DONE backups */
		if (tmp_backup->parent_backup == target_backup->start_time &&
			(tmp_backup->status == BACKUP_STATUS_OK ||
			 tmp_backup->status == BACKUP_STATUS_DONE))
		{
			child_counter++;
			if (child_counter > 1)
				return true;
		}
	}

	return false;
}

/*
 * Find parent base FULL backup for current backup using parent_backup_link
 */
pgBackup*
find_parent_full_backup(pgBackup *current_backup)
{
	pgBackup   *base_full_backup = NULL;
	base_full_backup = current_backup;

	/* sanity */
	if (!current_backup)
		elog(ERROR, "Target backup cannot be NULL");

	while (base_full_backup->parent_backup_link != NULL)
	{
		base_full_backup = base_full_backup->parent_backup_link;
	}

	if (base_full_backup->backup_mode != BACKUP_MODE_FULL)
	{
		if (base_full_backup->parent_backup)
			elog(WARNING, "Backup %s is missing",
				 base36enc(base_full_backup->parent_backup));
		else
			elog(WARNING, "Failed to find parent FULL backup for %s",
				 base36enc(current_backup->start_time));
		return NULL;
	}

	return base_full_backup;
}

/*
 * Interate over parent chain and look for any problems.
 * Return 0 if chain is broken.
 *  result_backup must contain oldest existing backup after missing backup.
 *  we have no way to know if there are multiple missing backups.
 * Return 1 if chain is intact, but at least one backup is !OK.
 *  result_backup must contain oldest !OK backup.
 * Return 2 if chain is intact and all backups are OK.
 *	result_backup must contain FULL backup on which chain is based.
 */
int
scan_parent_chain(pgBackup *current_backup, pgBackup **result_backup)
{
	pgBackup   *target_backup = NULL;
	pgBackup   *invalid_backup = NULL;

	if (!current_backup)
		elog(ERROR, "Target backup cannot be NULL");

	target_backup = current_backup;

	while (target_backup->parent_backup_link)
	{
		if (target_backup->status != BACKUP_STATUS_OK &&
			  target_backup->status != BACKUP_STATUS_DONE)
			/* oldest invalid backup in parent chain */
			invalid_backup = target_backup;


		target_backup = target_backup->parent_backup_link;
	}

	/* Prevous loop will skip FULL backup because his parent_backup_link is NULL */
	if (target_backup->backup_mode == BACKUP_MODE_FULL &&
		(target_backup->status != BACKUP_STATUS_OK &&
		target_backup->status != BACKUP_STATUS_DONE))
	{
		invalid_backup = target_backup;
	}

	/* found chain end and oldest backup is not FULL */
	if (target_backup->backup_mode != BACKUP_MODE_FULL)
	{
		/* Set oldest child backup in chain */
		*result_backup = target_backup;
		return 0;
	}

	/* chain is ok, but some backups are invalid */
	if (invalid_backup)
	{
		*result_backup = invalid_backup;
		return 1;
	}

	*result_backup = target_backup;
	return 2;
}

/*
 * Determine if child_backup descend from parent_backup
 * This check DO NOT(!!!) guarantee that parent chain is intact,
 * because parent_backup can be missing.
 * If inclusive is true, then child_backup counts as a child of himself
 * if parent_backup_time is start_time of child_backup.
 */
bool
is_parent(time_t parent_backup_time, pgBackup *child_backup, bool inclusive)
{
	if (!child_backup)
		elog(ERROR, "Target backup cannot be NULL");

	if (inclusive && child_backup->start_time == parent_backup_time)
		return true;

	while (child_backup->parent_backup_link &&
			child_backup->parent_backup != parent_backup_time)
	{
		child_backup = child_backup->parent_backup_link;
	}

	if (child_backup->parent_backup == parent_backup_time)
		return true;

	//if (inclusive && child_backup->start_time == parent_backup_time)
	//	return true;

	return false;
}

/*
 * Return backup index number.
 * Note: this index number holds true until new sorting of backup list
 */
int
get_backup_index_number(parray *backup_list, pgBackup *backup)
{
	int i;

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *tmp_backup = (pgBackup *) parray_get(backup_list, i);

		if (tmp_backup->start_time == backup->start_time)
			return i;
	}
	elog(WARNING, "Failed to find backup %s", base36enc(backup->start_time));
	return -1;
}
