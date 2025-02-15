/*-------------------------------------------------------------------------
 *
 * help.c
 *
 * Copyright (c) 2017-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

static void help_init(void);
static void help_backup(void);
static void help_restore(void);
static void help_validate(void);
static void help_show(void);
static void help_delete(void);
static void help_merge(void);
static void help_set_config(void);
static void help_show_config(void);
static void help_add_instance(void);
static void help_del_instance(void);
static void help_archive_push(void);
static void help_archive_get(void);
static void help_checkdb(void);

void
help_command(char *command)
{
	if (strcmp(command, "init") == 0)
		help_init();
	else if (strcmp(command, "backup") == 0)
		help_backup();
	else if (strcmp(command, "restore") == 0)
		help_restore();
	else if (strcmp(command, "validate") == 0)
		help_validate();
	else if (strcmp(command, "show") == 0)
		help_show();
	else if (strcmp(command, "delete") == 0)
		help_delete();
	else if (strcmp(command, "merge") == 0)
		help_merge();
	else if (strcmp(command, "set-config") == 0)
		help_set_config();
	else if (strcmp(command, "show-config") == 0)
		help_show_config();
	else if (strcmp(command, "add-instance") == 0)
		help_add_instance();
	else if (strcmp(command, "del-instance") == 0)
		help_del_instance();
	else if (strcmp(command, "archive-push") == 0)
		help_archive_push();
	else if (strcmp(command, "archive-get") == 0)
		help_archive_get();
	else if (strcmp(command, "checkdb") == 0)
		help_checkdb();
	else if (strcmp(command, "--help") == 0
			 || strcmp(command, "help") == 0
			 || strcmp(command, "-?") == 0
			 || strcmp(command, "--version") == 0
			 || strcmp(command, "version") == 0
			 || strcmp(command, "-V") == 0)
		printf(_("No help page for \"%s\" command. Try pg_probackup help\n"), command);
	else
		printf(_("Unknown command \"%s\". Try pg_probackup help\n"), command);
	exit(0);
}

void
help_pg_probackup(void)
{
	printf(_("\n%s - utility to manage backup/recovery of PostgreSQL database.\n\n"), PROGRAM_NAME);

	printf(_("  %s help [COMMAND]\n"), PROGRAM_NAME);

	printf(_("\n  %s version\n"), PROGRAM_NAME);

	printf(_("\n  %s init -B backup-path\n"), PROGRAM_NAME);

	printf(_("\n  %s set-config -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path]\n"));
	printf(_("                 [--external-dirs=external-directories-paths]\n"));
	printf(_("                 [--log-level-console=log-level-console]\n"));
	printf(_("                 [--log-level-file=log-level-file]\n"));
	printf(_("                 [--log-filename=log-filename]\n"));
	printf(_("                 [--error-log-filename=error-log-filename]\n"));
	printf(_("                 [--log-directory=log-directory]\n"));
	printf(_("                 [--log-rotation-size=log-rotation-size]\n"));
	printf(_("                 [--log-rotation-age=log-rotation-age]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]\n"));
	printf(_("                 [--retention-window=retention-window]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [--archive-timeout=timeout]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s show-config -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [--format=format]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s backup -B backup-path -b backup-mode --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path] [-C]\n"));
	printf(_("                 [--stream [-S slot-name]] [--temp-slot]\n"));
	printf(_("                 [--backup-pg-log] [-j num-threads] [--progress]\n"));
	printf(_("                 [--no-validate] [--skip-block-validation]\n"));
	printf(_("                 [--external-dirs=external-directories-paths]\n"));
	printf(_("                 [--log-level-console=log-level-console]\n"));
	printf(_("                 [--log-level-file=log-level-file]\n"));
	printf(_("                 [--log-filename=log-filename]\n"));
	printf(_("                 [--error-log-filename=error-log-filename]\n"));
	printf(_("                 [--log-directory=log-directory]\n"));
	printf(_("                 [--log-rotation-size=log-rotation-size]\n"));
	printf(_("                 [--log-rotation-age=log-rotation-age]\n"));
	printf(_("                 [--delete-expired] [--delete-wal] [--merge-expired]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]\n"));
	printf(_("                 [--retention-window=retention-window]\n"));
	printf(_("                 [--compress]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [--archive-timeout=archive-timeout]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [-w --no-password] [-W --password]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s restore -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path] [-i backup-id] [-j num-threads]\n"));
	printf(_("                 [--recovery-target-time=time|--recovery-target-xid=xid\n"));
	printf(_("                  |--recovery-target-lsn=lsn [--recovery-target-inclusive=boolean]]\n"));
	printf(_("                 [--recovery-target-timeline=timeline]\n"));
	printf(_("                 [--recovery-target=immediate|latest]\n"));
	printf(_("                 [--recovery-target-name=target-name]\n"));
	printf(_("                 [--recovery-target-action=pause|promote|shutdown]\n"));
	printf(_("                 [--restore-as-replica]\n"));
	printf(_("                 [--no-validate] [--skip-block-validation]\n"));
	printf(_("                 [-T OLDDIR=NEWDIR] [--progress]\n"));
	printf(_("                 [--external-mapping=OLDDIR=NEWDIR]\n"));
	printf(_("                 [--skip-external-dirs]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s validate -B backup-path [--instance=instance_name]\n"), PROGRAM_NAME);
	printf(_("                 [-i backup-id] [--progress] [-j num-threads]\n"));
	printf(_("                 [--recovery-target-time=time|--recovery-target-xid=xid\n"));
	printf(_("                  |--recovery-target-lsn=lsn [--recovery-target-inclusive=boolean]]\n"));
	printf(_("                 [--recovery-target-timeline=timeline]\n"));
	printf(_("                 [--recovery-target-name=target-name]\n"));
	printf(_("                 [--skip-block-validation]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s checkdb [-B backup-path] [--instance=instance_name]\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path] [--progress] [-j num-threads]\n"));
	printf(_("                 [--amcheck] [--skip-block-validation]\n"));
	printf(_("                 [--heapallindexed]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s show -B backup-path\n"), PROGRAM_NAME);
	printf(_("                 [--instance=instance_name [-i backup-id]]\n"));
	printf(_("                 [--format=format]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s delete -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [--wal] [-i backup-id | --expired | --merge-expired]\n"));
	printf(_("                 [--dry-run]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s merge -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 -i backup-id [--progress] [-j num-threads]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s add-instance -B backup-path -D pgdata-path\n"), PROGRAM_NAME);
	printf(_("                 --instance=instance_name\n"));
	printf(_("                 [--external-dirs=external-directories-paths]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s del-instance -B backup-path\n"), PROGRAM_NAME);
	printf(_("                 --instance=instance_name\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s archive-push -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 --wal-file-path=wal-file-path\n"));
	printf(_("                 --wal-file-name=wal-file-name\n"));
	printf(_("                 [--overwrite]\n"));
	printf(_("                 [--compress]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n"));
	printf(_("                 [--help]\n"));

	printf(_("\n  %s archive-get -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 --wal-file-path=wal-file-path\n"));
	printf(_("                 --wal-file-name=wal-file-name\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n"));
	printf(_("                 [--help]\n"));

	if ((PROGRAM_URL || PROGRAM_EMAIL))
	{
		printf("\n");
		if (PROGRAM_URL)
			printf("Read the website for details. <%s>\n", PROGRAM_URL);
		if (PROGRAM_EMAIL)
			printf("Report bugs to <%s>.\n", PROGRAM_EMAIL);
	}
	exit(0);
}

static void
help_init(void)
{
	printf(_("\n%s init -B backup-path\n\n"), PROGRAM_NAME);
	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n\n"));
}

static void
help_backup(void)
{
	printf(_("\n%s backup -B backup-path -b backup-mode --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path] [-C]\n"));
	printf(_("                 [--stream [-S slot-name] [--temp-slot]\n"));
	printf(_("                 [--backup-pg-log] [-j num-threads] [--progress]\n"));
	printf(_("                 [--no-validate] [--skip-block-validation]\n"));
	printf(_("                 [-E external-directories-paths]\n"));
	printf(_("                 [--log-level-console=log-level-console]\n"));
	printf(_("                 [--log-level-file=log-level-file]\n"));
	printf(_("                 [--log-filename=log-filename]\n"));
	printf(_("                 [--error-log-filename=error-log-filename]\n"));
	printf(_("                 [--log-directory=log-directory]\n"));
	printf(_("                 [--log-rotation-size=log-rotation-size]\n"));
	printf(_("                 [--log-rotation-age=log-rotation-age]\n"));
	printf(_("                 [--delete-expired] [--delete-wal] [--merge-expired]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]\n"));
	printf(_("                 [--retention-window=retention-window]\n"));
	printf(_("                 [--compress]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [--archive-timeout=archive-timeout]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [-w --no-password] [-W --password]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -b, --backup-mode=backup-mode    backup mode=FULL|PAGE|DELTA|PTRACK\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -D, --pgdata=pgdata-path         location of the database storage area\n"));
	printf(_("  -C, --smooth-checkpoint          do smooth checkpoint before backup\n"));
	printf(_("      --stream                     stream the transaction log and include it in the backup\n"));
	printf(_("  -S, --slot=SLOTNAME              replication slot to use\n"));
	printf(_("      --temp-slot                  use temporary replication slot\n"));
	printf(_("      --backup-pg-log              backup of '%s' directory\n"), PG_LOG_DIR);
	printf(_("  -j, --threads=NUM                number of parallel threads\n"));
	printf(_("      --progress                   show progress\n"));
	printf(_("      --no-validate                disable validation after backup\n"));
	printf(_("      --skip-block-validation      set to validate only file-level checksum\n"));
	printf(_("  -E  --external-dirs=external-directories-paths\n"));
	printf(_("                                   backup some directories not from pgdata \n"));
	printf(_("                                   (example: --external-dirs=/tmp/dir1:/tmp/dir2)\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level-console=log-level-console\n"));
	printf(_("                                   level for console logging (default: info)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-level-file=log-level-file\n"));
	printf(_("                                   level for file logging (default: off)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-filename=log-filename\n"));
	printf(_("                                   filename for file logging (default: 'pg_probackup.log')\n"));
	printf(_("                                   support strftime format (example: pg_probackup-%%Y-%%m-%%d_%%H%%M%%S.log)\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   filename for error logging (default: none)\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory for file logging (default: BACKUP_PATH/log)\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   rotate logfile if its size exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'kB', 'MB', 'GB', 'TB' (default: kB)\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   rotate logfile if its age exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'ms', 's', 'min', 'h', 'd' (default: min)\n"));

	printf(_("\n  Retention options:\n"));
	printf(_("      --delete-expired             delete backups expired according to current\n"));
	printf(_("                                   retention policy after successful backup completion\n"));
	printf(_("      --merge-expired              merge backups expired according to current\n"));
	printf(_("                                   retention policy after successful backup completion\n"));
	printf(_("      --delete-wal                 remove redundant archived wal files\n"));
	printf(_("      --retention-redundancy=retention-redundancy\n"));
	printf(_("                                   number of full backups to keep; 0 disables; (default: 0)\n"));
	printf(_("      --retention-window=retention-window\n"));
	printf(_("                                   number of days of recoverability; 0 disables; (default: 0)\n"));
	printf(_("      --dry-run                    perform a trial run without any changes\n"));

	printf(_("\n  Compression options:\n"));
	printf(_("      --compress                   alias for --compress-algorithm='zlib' and --compress-level=1\n"));
	printf(_("      --compress-algorithm=compress-algorithm\n"));
	printf(_("                                   available options: 'zlib', 'pglz', 'none' (default: none)\n"));
	printf(_("      --compress-level=compress-level\n"));
	printf(_("                                   level of compression [0-9] (default: 1)\n"));

	printf(_("\n  Archive options:\n"));
	printf(_("      --archive-timeout=timeout   wait timeout for WAL segment archiving (default: 5min)\n"));

	printf(_("\n  Connection options:\n"));
	printf(_("  -U, --username=USERNAME          user name to connect as (default: current local user)\n"));
	printf(_("  -d, --dbname=DBNAME              database to connect (default: username)\n"));
	printf(_("  -h, --host=HOSTNAME              database server host or socket directory(default: 'local socket')\n"));
	printf(_("  -p, --port=PORT                  database server port (default: 5432)\n"));
	printf(_("  -w, --no-password                never prompt for password\n"));
	printf(_("  -W, --password                   force password prompt\n"));

	printf(_("\n  Remote options:\n"));
	printf(_("      --remote-proto=protocol      remote protocol to use\n"));
	printf(_("                                   available options: 'ssh', 'none' (default: ssh)\n"));
	printf(_("      --remote-host=hostname       remote host address or hostname\n"));
	printf(_("      --remote-port=port           remote host port (default: 22)\n"));
	printf(_("      --remote-path=path           path to directory with pg_probackup binary on remote host\n"));
	printf(_("                                   (default: current binary path)\n"));
	printf(_("      --remote-user=username       user name for ssh connection (default: current user)\n"));
	printf(_("      --ssh-options=ssh_options    additional ssh options (default: none)\n"));
	printf(_("                                   (example: --ssh-options='-c cipher_spec -F configfile')\n"));

	printf(_("\n  Replica options:\n"));
	printf(_("      --master-user=user_name      user name to connect to master (deprecated)\n"));
	printf(_("      --master-db=db_name          database to connect to master (deprecated)\n"));
	printf(_("      --master-host=host_name      database server host of master (deprecated)\n"));
	printf(_("      --master-port=port           database server port of master (deprecated)\n"));
	printf(_("      --replica-timeout=timeout    wait timeout for WAL segment streaming through replication (deprecated)\n\n"));
}

static void
help_restore(void)
{
	printf(_("\n%s restore -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path] [-i backup-id] [-j num-threads]\n"));
	printf(_("                 [--recovery-target-time=time|--recovery-target-xid=xid\n"));
	printf(_("                  |--recovery-target-lsn=lsn [--recovery-target-inclusive=boolean]]\n"));
	printf(_("                 [--recovery-target-timeline=timeline]\n"));
	printf(_("                 [--recovery-target=immediate|latest]\n"));
	printf(_("                 [--recovery-target-name=target-name]\n"));
	printf(_("                 [--recovery-target-action=pause|promote|shutdown]\n"));
	printf(_("                 [--restore-as-replica]\n"));
	printf(_("                 [--no-validate] [--skip-block-validation]\n"));
	printf(_("                 [-T OLDDIR=NEWDIR] [--progress]\n"));
	printf(_("                 [--external-mapping=OLDDIR=NEWDIR]\n"));
	printf(_("                 [--skip-external-dirs]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));

	printf(_("  -D, --pgdata=pgdata-path         location of the database storage area\n"));
	printf(_("  -i, --backup-id=backup-id        backup to restore\n"));
	printf(_("  -j, --threads=NUM                number of parallel threads\n"));

	printf(_("      --progress                   show progress\n"));
	printf(_("      --recovery-target-time=time  time stamp up to which recovery will proceed\n"));
	printf(_("      --recovery-target-xid=xid    transaction ID up to which recovery will proceed\n"));
	printf(_("      --recovery-target-lsn=lsn    LSN of the write-ahead log location up to which recovery will proceed\n"));
	printf(_("      --recovery-target-inclusive=boolean\n"));
	printf(_("                                   whether we stop just after the recovery target\n"));
	printf(_("      --recovery-target-timeline=timeline\n"));
	printf(_("                                   recovering into a particular timeline\n"));
	printf(_("      --recovery-target=immediate|latest\n"));
	printf(_("                                   end recovery as soon as a consistent state is reached or as late as possible\n"));
	printf(_("      --recovery-target-name=target-name\n"));
	printf(_("                                   the named restore point to which recovery will proceed\n"));
	printf(_("      --recovery-target-action=pause|promote|shutdown\n"));
	printf(_("                                   action the server should take once the recovery target is reached\n"));
	printf(_("                                   (default: pause)\n"));

	printf(_("  -R, --restore-as-replica         write a minimal recovery.conf in the output directory\n"));
	printf(_("                                   to ease setting up a standby server\n"));
	printf(_("      --no-validate                disable backup validation during restore\n"));
	printf(_("      --skip-block-validation      set to validate only file-level checksum\n"));

	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"));
	printf(_("                                   relocate the tablespace from directory OLDDIR to NEWDIR\n"));
	printf(_("      --external-mapping=OLDDIR=NEWDIR\n"));
	printf(_("                                   relocate the external directory from OLDDIR to NEWDIR\n"));
	printf(_("      --skip-external-dirs         do not restore all external directories\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level-console=log-level-console\n"));
	printf(_("                                   level for console logging (default: info)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-level-file=log-level-file\n"));
	printf(_("                                   level for file logging (default: off)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-filename=log-filename\n"));
	printf(_("                                   filename for file logging (default: 'pg_probackup.log')\n"));
	printf(_("                                   support strftime format (example: pg_probackup-%%Y-%%m-%%d_%%H%%M%%S.log)\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   filename for error logging (default: none)\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory for file logging (default: BACKUP_PATH/log)\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   rotate logfile if its size exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'kB', 'MB', 'GB', 'TB' (default: kB)\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   rotate logfile if its age exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'ms', 's', 'min', 'h', 'd' (default: min)\n"));

	printf(_("\n  Remote options:\n"));
	printf(_("      --remote-proto=protocol      remote protocol to use\n"));
	printf(_("                                   available options: 'ssh', 'none' (default: ssh)\n"));
	printf(_("      --remote-host=hostname       remote host address or hostname\n"));
	printf(_("      --remote-port=port           remote host port (default: 22)\n"));
	printf(_("      --remote-path=path           path to directory with pg_probackup binary on remote host\n"));
	printf(_("                                   (default: current binary path)\n"));
	printf(_("      --remote-user=username       user name for ssh connection (default: current user)\n"));
	printf(_("      --ssh-options=ssh_options    additional ssh options (default: none)\n"));
	printf(_("                                   (example: --ssh-options='-c cipher_spec -F configfile')\n\n"));
}

static void
help_validate(void)
{
	printf(_("\n%s validate -B backup-path [--instance=instance_name]\n"), PROGRAM_NAME);
	printf(_("                 [-i backup-id] [--progress] [-j num-threads]\n"));
	printf(_("                 [--recovery-target-time=time|--recovery-target-xid=xid\n"));
	printf(_("                  |--recovery-target-lsn=lsn [--recovery-target-inclusive=boolean]]\n"));
	printf(_("                 [--recovery-target-timeline=timeline]\n"));
	printf(_("                 [--recovery-target-name=target-name]\n"));
	printf(_("                 [--skip-block-validation]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -i, --backup-id=backup-id        backup to validate\n"));

	printf(_("      --progress                   show progress\n"));
	printf(_("  -j, --threads=NUM                number of parallel threads\n"));
	printf(_("      --recovery-target-time=time  time stamp up to which recovery will proceed\n"));
	printf(_("      --recovery-target-xid=xid    transaction ID up to which recovery will proceed\n"));
	printf(_("      --recovery-target-lsn=lsn    LSN of the write-ahead log location up to which recovery will proceed\n"));
	printf(_("      --recovery-target-inclusive=boolean\n"));
	printf(_("                                   whether we stop just after the recovery target\n"));
	printf(_("      --recovery-target-timeline=timeline\n"));
	printf(_("                                   recovering into a particular timeline\n"));
	printf(_("      --recovery-target-name=target-name\n"));
	printf(_("                                   the named restore point to which recovery will proceed\n"));
	printf(_("      --skip-block-validation      set to validate only file-level checksum\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level-console=log-level-console\n"));
	printf(_("                                   level for console logging (default: info)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-level-file=log-level-file\n"));
	printf(_("                                   level for file logging (default: off)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-filename=log-filename\n"));
	printf(_("                                   filename for file logging (default: 'pg_probackup.log')\n"));
	printf(_("                                   support strftime format (example: pg_probackup-%%Y-%%m-%%d_%%H%%M%%S.log)\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   filename for error logging (default: none)\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory for file logging (default: BACKUP_PATH/log)\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   rotate logfile if its size exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'kB', 'MB', 'GB', 'TB' (default: kB)\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   rotate logfile if its age exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'ms', 's', 'min', 'h', 'd' (default: min)\n\n"));
}

static void
help_checkdb(void)
{
	printf(_("\n%s checkdb [-B backup-path] [--instance=instance_name]\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path] [-j num-threads] [--progress]\n"));
	printf(_("                 [--amcheck] [--skip-block-validation]\n"));
	printf(_("                 [--heapallindexed]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -D, --pgdata=pgdata-path         location of the database storage area\n"));

	printf(_("      --progress                   show progress\n"));
	printf(_("  -j, --threads=NUM                number of parallel threads\n"));
	printf(_("      --skip-block-validation      skip file-level checking\n"));
	printf(_("                                   can be used only with '--amcheck' option\n"));
	printf(_("      --amcheck                    in addition to file-level block checking\n"));
	printf(_("                                   check btree indexes via function 'bt_index_check()'\n"));
	printf(_("                                   using 'amcheck' or 'amcheck_next' extensions\n"));
	printf(_("      --heapallindexed             also check that heap is indexed\n"));
	printf(_("                                   can be used only with '--amcheck' option\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level-console=log-level-console\n"));
	printf(_("                                   level for console logging (default: info)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-level-file=log-level-file\n"));
	printf(_("                                   level for file logging (default: off)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-filename=log-filename\n"));
	printf(_("                                   filename for file logging (default: 'pg_probackup.log')\n"));
	printf(_("                                   support strftime format (example: pg_probackup-%%Y-%%m-%%d_%%H%%M%%S.log\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   filename for error logging (default: none)\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory for file logging (default: BACKUP_PATH/log)\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   rotate logfile if its size exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'kB', 'MB', 'GB', 'TB' (default: kB)\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   rotate logfile if its age exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'ms', 's', 'min', 'h', 'd' (default: min)\n"));

	printf(_("\n  Connection options:\n"));
	printf(_("  -U, --username=USERNAME          user name to connect as (default: current local user)\n"));
	printf(_("  -d, --dbname=DBNAME              database to connect (default: username)\n"));
	printf(_("  -h, --host=HOSTNAME              database server host or socket directory(default: 'local socket')\n"));
	printf(_("  -p, --port=PORT                  database server port (default: 5432)\n"));
	printf(_("  -w, --no-password                never prompt for password\n"));
	printf(_("  -W, --password                   force password prompt\n\n"));
}

static void
help_show(void)
{
	printf(_("\n%s show -B backup-path\n"), PROGRAM_NAME);
	printf(_("                 [--instance=instance_name [-i backup-id]]\n"));
	printf(_("                 [--format=format]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     show info about specific intstance\n"));
	printf(_("  -i, --backup-id=backup-id        show info about specific backups\n"));
	printf(_("      --format=format              show format=PLAIN|JSON\n\n"));
}

static void
help_delete(void)
{
	printf(_("\n%s delete -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-i backup-id | --expired | --merge-expired] [--wal]\n"));
	printf(_("                 [-j num-threads] [--dry-run]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -i, --backup-id=backup-id        backup to delete\n"));
	printf(_("      --expired                    delete backups expired according to current\n"));
	printf(_("                                   retention policy\n"));
	printf(_("      --merge-expired              merge backups expired according to current\n"));
	printf(_("                                   retention policy\n"));
	printf(_("      --wal                        remove unnecessary wal files in WAL ARCHIVE\n"));
	printf(_("  -j, --threads=NUM                number of parallel threads\n"));
	printf(_("      --dry-run                    perform a trial run without any changes\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level-console=log-level-console\n"));
	printf(_("                                   level for console logging (default: info)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-level-file=log-level-file\n"));
	printf(_("                                   level for file logging (default: off)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-filename=log-filename\n"));
	printf(_("                                   filename for file logging (default: 'pg_probackup.log')\n"));
	printf(_("                                   support strftime format (example: pg_probackup-%%Y-%%m-%%d_%%H%%M%%S.log)\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   filename for error logging (default: none)\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory for file logging (default: BACKUP_PATH/log)\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   rotate logfile if its size exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'kB', 'MB', 'GB', 'TB' (default: kB)\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   rotate logfile if its age exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'ms', 's', 'min', 'h', 'd' (default: min)\n\n"));
}

static void
help_merge(void)
{
	printf(_("\n%s merge -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 -i backup-id [-j num-threads] [--progress]\n"));
	printf(_("                 [--log-level-console=log-level-console]\n"));
	printf(_("                 [--log-level-file=log-level-file]\n"));
	printf(_("                 [--log-filename=log-filename]\n"));
	printf(_("                 [--error-log-filename=error-log-filename]\n"));
	printf(_("                 [--log-directory=log-directory]\n"));
	printf(_("                 [--log-rotation-size=log-rotation-size]\n"));
	printf(_("                 [--log-rotation-age=log-rotation-age]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -i, --backup-id=backup-id        backup to merge\n"));

	printf(_("  -j, --threads=NUM                number of parallel threads\n"));
	printf(_("      --progress                   show progress\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level-console=log-level-console\n"));
	printf(_("                                   level for console logging (default: info)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-level-file=log-level-file\n"));
	printf(_("                                   level for file logging (default: off)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-filename=log-filename\n"));
	printf(_("                                   filename for file logging (default: 'pg_probackup.log')\n"));
	printf(_("                                   support strftime format (example: pg_probackup-%%Y-%%m-%%d_%%H%%M%%S.log)\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   filename for error logging (default: none)\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory for file logging (default: BACKUP_PATH/log)\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   rotate logfile if its size exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'kB', 'MB', 'GB', 'TB' (default: kB)\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   rotate logfile if its age exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'ms', 's', 'min', 'h', 'd' (default: min)\n\n"));
}

static void
help_set_config(void)
{
	printf(_("\n%s set-config -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-path]\n"));
	printf(_("                 [-E external-directories-paths]\n"));
	printf(_("                 [--log-level-console=log-level-console]\n"));
	printf(_("                 [--log-level-file=log-level-file]\n"));
	printf(_("                 [--log-filename=log-filename]\n"));
	printf(_("                 [--error-log-filename=error-log-filename]\n"));
	printf(_("                 [--log-directory=log-directory]\n"));
	printf(_("                 [--log-rotation-size=log-rotation-size]\n"));
	printf(_("                 [--log-rotation-age=log-rotation-age]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]\n"));
	printf(_("                 [--retention-window=retention-window]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [--archive-timeout=timeout]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -D, --pgdata=pgdata-path         location of the database storage area\n"));
	printf(_("  -E  --external-dirs=external-directories-paths\n"));
	printf(_("                                   backup some directories not from pgdata \n"));
	printf(_("                                   (example: --external-dirs=/tmp/dir1:/tmp/dir2)\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level-console=log-level-console\n"));
	printf(_("                                   level for console logging (default: info)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-level-file=log-level-file\n"));
	printf(_("                                   level for file logging (default: off)\n"));
	printf(_("                                   available options: 'off', 'error', 'warning', 'info', 'log', 'verbose'\n"));
	printf(_("      --log-filename=log-filename\n"));
	printf(_("                                   filename for file logging (default: 'pg_probackup.log')\n"));
	printf(_("                                   support strftime format (example: pg_probackup-%%Y-%%m-%%d_%%H%%M%%S.log)\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   filename for error logging (default: none)\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory for file logging (default: BACKUP_PATH/log)\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   rotate logfile if its size exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'kB', 'MB', 'GB', 'TB' (default: kB)\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   rotate logfile if its age exceeds this value; 0 disables; (default: 0)\n"));
	printf(_("                                   available units: 'ms', 's', 'min', 'h', 'd' (default: min)\n"));

	printf(_("\n  Retention options:\n"));
	printf(_("      --retention-redundancy=retention-redundancy\n"));
	printf(_("                                   number of full backups to keep; 0 disables; (default: 0)\n"));
	printf(_("      --retention-window=retention-window\n"));
	printf(_("                                   number of days of recoverability; 0 disables; (default: 0)\n"));

	printf(_("\n  Compression options:\n"));
	printf(_("      --compress                   alias for --compress-algorithm='zlib' and --compress-level=1\n"));
	printf(_("      --compress-algorithm=compress-algorithm\n"));
	printf(_("                                   available options: 'zlib','pglz','none' (default: 'none')\n"));
	printf(_("      --compress-level=compress-level\n"));
	printf(_("                                   level of compression [0-9] (default: 1)\n"));

	printf(_("\n  Archive options:\n"));
	printf(_("      --archive-timeout=timeout   wait timeout for WAL segment archiving (default: 5min)\n"));

	printf(_("\n  Connection options:\n"));
	printf(_("  -U, --username=USERNAME          user name to connect as (default: current local user)\n"));
	printf(_("  -d, --dbname=DBNAME              database to connect (default: username)\n"));
	printf(_("  -h, --host=HOSTNAME              database server host or socket directory(default: 'local socket')\n"));
	printf(_("  -p, --port=PORT                  database server port (default: 5432)\n"));

	printf(_("\n  Remote options:\n"));
	printf(_("      --remote-proto=protocol      remote protocol to use\n"));
	printf(_("                                   available options: 'ssh', 'none' (default: ssh)\n"));
	printf(_("      --remote-host=hostname       remote host address or hostname\n"));
	printf(_("      --remote-port=port           remote host port (default: 22)\n"));
	printf(_("      --remote-path=path           path to directory with pg_probackup binary on remote host\n"));
	printf(_("                                   (default: current binary path)\n"));
	printf(_("      --remote-user=username       user name for ssh connection (default: current user)\n"));
	printf(_("      --ssh-options=ssh_options    additional ssh options (default: none)\n"));
	printf(_("                                   (example: --ssh-options='-c cipher_spec -F configfile')\n"));

	printf(_("\n  Replica options:\n"));
	printf(_("      --master-user=user_name      user name to connect to master (deprecated)\n"));
	printf(_("      --master-db=db_name          database to connect to master (deprecated)\n"));
	printf(_("      --master-host=host_name      database server host of master (deprecated)\n"));
	printf(_("      --master-port=port           database server port of master (deprecated)\n"));
	printf(_("      --replica-timeout=timeout    wait timeout for WAL segment streaming through replication (deprecated)\n\n"));
}

static void
help_show_config(void)
{
	printf(_("\n%s show-config -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [--format=format]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("      --format=format              show format=PLAIN|JSON\n\n"));
}

static void
help_add_instance(void)
{
	printf(_("\n%s add-instance -B backup-path -D pgdata-path\n"), PROGRAM_NAME);
	printf(_("                 --instance=instance_name\n"));
	printf(_("                 [-E external-directory-path]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -D, --pgdata=pgdata-path         location of the database storage area\n"));
	printf(_("      --instance=instance_name     name of the new instance\n"));

	printf(_("  -E  --external-dirs=external-directories-paths\n"));
	printf(_("                                   backup some directories not from pgdata \n"));
	printf(_("                                   (example: --external-dirs=/tmp/dir1:/tmp/dir2)\n"));
	printf(_("\n  Remote options:\n"));
	printf(_("      --remote-proto=protocol      remote protocol to use\n"));
	printf(_("                                   available options: 'ssh', 'none' (default: ssh)\n"));
	printf(_("      --remote-host=hostname       remote host address or hostname\n"));
	printf(_("      --remote-port=port           remote host port (default: 22)\n"));
	printf(_("      --remote-path=path           path to directory with pg_probackup binary on remote host\n"));
	printf(_("                                   (default: current binary path)\n"));
	printf(_("      --remote-user=username       user name for ssh connection (default: current user)\n"));
	printf(_("      --ssh-options=ssh_options    additional ssh options (default: none)\n"));
	printf(_("                                   (example: --ssh-options='-c cipher_spec -F configfile')\n\n"));
}

static void
help_del_instance(void)
{
	printf(_("\n%s del-instance -B backup-path --instance=instance_name\n"), PROGRAM_NAME);

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance to delete\n\n"));
}

static void
help_archive_push(void)
{
	printf(_("\n%s archive-push -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 --wal-file-path=wal-file-path\n"));
	printf(_("                 --wal-file-name=wal-file-name\n"));
	printf(_("                 [--overwrite]\n"));
	printf(_("                 [--compress]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance to delete\n"));
	printf(_("      --wal-file-path=wal-file-path\n"));
	printf(_("                                   relative path name of the WAL file on the server\n"));
	printf(_("      --wal-file-name=wal-file-name\n"));
	printf(_("                                   name of the WAL file to retrieve from the server\n"));
	printf(_("      --overwrite                  overwrite archived WAL file\n"));

	printf(_("\n  Compression options:\n"));
	printf(_("      --compress                   alias for --compress-algorithm='zlib' and --compress-level=1\n"));
	printf(_("      --compress-algorithm=compress-algorithm\n"));
	printf(_("                                   available options: 'zlib','pglz','none' (default: 'none')\n"));
	printf(_("      --compress-level=compress-level\n"));
	printf(_("                                   level of compression [0-9] (default: 1)\n"));

	printf(_("\n  Remote options:\n"));
	printf(_("      --remote-proto=protocol      remote protocol to use\n"));
	printf(_("                                   available options: 'ssh', 'none' (default: ssh)\n"));
	printf(_("      --remote-host=hostname       remote host address or hostname\n"));
	printf(_("      --remote-port=port           remote host port (default: 22)\n"));
	printf(_("      --remote-path=path           path to directory with pg_probackup binary on remote host\n"));
	printf(_("                                   (default: current binary path)\n"));
	printf(_("      --remote-user=username       user name for ssh connection (default: current user)\n"));
	printf(_("      --ssh-options=ssh_options    additional ssh options (default: none)\n"));
	printf(_("                                   (example: --ssh-options='-c cipher_spec -F configfile')\n\n"));
}

static void
help_archive_get(void)
{
	printf(_("\n%s archive-get -B backup-path --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 --wal-file-path=wal-file-path\n"));
	printf(_("                 --wal-file-name=wal-file-name\n"));
	printf(_("                 [--remote-proto] [--remote-host]\n"));
	printf(_("                 [--remote-port] [--remote-path] [--remote-user]\n"));
	printf(_("                 [--ssh-options]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance to delete\n"));
	printf(_("      --wal-file-path=wal-file-path\n"));
	printf(_("                                   relative destination path name of the WAL file on the server\n"));
	printf(_("      --wal-file-name=wal-file-name\n"));
	printf(_("                                   name of the WAL file to retrieve from the archive\n"));

	printf(_("\n  Remote options:\n"));
	printf(_("      --remote-proto=protocol      remote protocol to use\n"));
	printf(_("                                   available options: 'ssh', 'none' (default: ssh)\n"));
	printf(_("      --remote-host=hostname       remote host address or hostname\n"));
	printf(_("      --remote-port=port           remote host port (default: 22)\n"));
	printf(_("      --remote-path=path           path to directory with pg_probackup binary on remote host\n"));
	printf(_("                                   (default: current binary path)\n"));
	printf(_("      --remote-user=username       user name for ssh connection (default: current user)\n"));
	printf(_("      --ssh-options=ssh_options    additional ssh options (default: none)\n"));
	printf(_("                                   (example: --ssh-options='-c cipher_spec -F configfile')\n\n"));
}
