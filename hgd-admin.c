/*
 * Copyright (c) 2011, Edd Barrett <vext01@gmail.com>
 * Copyright (c) 2011, Martin Ellis <ellism88@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE	/* linux */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>

#include <sqlite3.h>
#include <openssl/rand.h>

#include "hgd.h"
#include "db.h"

uint8_t				 purge_finished_db = 1;
uint8_t				 purge_finished_fs = 1;
uint8_t				 clear_playlist_on_start = 0;

/*
 * clean up, exit. if exit_ok = 0, an error (signal/error)
 */
void
hgd_exit_nicely()
{
	if (!exit_ok)
		DPRINTF(HGD_D_ERROR, "hgd-playd was interrupted or crashed\n");

	if (db)
		sqlite3_close(db);
	if (hgd_dir)
		free(hgd_dir);
	if (db_path)
		free (db_path);
	if (filestore_path)
		free(filestore_path);

	exit (!exit_ok);
}
/* NOTE! -c is reserved for 'config file path' */
void
hgd_usage()
{
        printf("Usage: hgdc [opts] command [args]\n\n");
        printf("  Commands include:\n");
        printf("    user-add username [password]\tAdd a user.\n");
        printf("    user-del username\t\t\tDelete a user.\n");
        printf("    user-list\t\t\t\tList users.\n");
	/*
        printf("    user-disable username\tDisable a user account");
        printf("    user-chpw username\t\t\tChange a users password\n");
        printf("    user-enable username\t\t\Re-enable a user\n\n");
	*/
        printf("\n  Options include:\n");
        printf("    -d\t\t\tLocation of state directory\n");
        printf("    -h\t\t\tShow this message and exit\n");
        printf("    -x level\t\tSet debug level (0-3)\n");
        printf("    -v\t\t\tShow version and exit\n");
}

int
hgd_acmd_user_add(char **args)
{
	unsigned char		 salt[HGD_SHA_SALT_SZ];
	char			*salt_hex, *hash_hex;
	char			*user = args[0], *pass = args[1];

	DPRINTF(HGD_D_INFO, "Adding user '%s'", user);

	memset(salt, 0, HGD_SHA_SALT_SZ);
	if (RAND_bytes(salt, HGD_SHA_SALT_SZ) != 1) {
		DPRINTF(HGD_D_ERROR, "can not generate salt");
		return (HGD_FAIL);
	}

	salt_hex = hgd_bytes_to_hex(salt, HGD_SHA_SALT_SZ);
	DPRINTF(HGD_D_DEBUG, "new user's salt '%s'", salt_hex);

	hash_hex = hgd_sha1(pass, salt_hex);
	memset(pass, 0, strlen(pass));
	DPRINTF(HGD_D_DEBUG, "new_user's hash '%s'", hash_hex);

	hgd_add_user(args[0], salt_hex, hash_hex);

	free(salt_hex);
	free(hash_hex);

	return (HGD_OK);
}

int
hgd_acmd_user_add_prompt(char **args)
{
	char			 pass[HGD_MAX_PASS_SZ];
	char			*new_args[2];

	if (hgd_readpassphrase_confirmed(pass) != HGD_OK)
		return (HGD_FAIL);

	new_args[0] = args[0];
	new_args[1] = pass;

	return (hgd_acmd_user_add(new_args));
}

int
hgd_acmd_user_del(char **args)
{
	if (hgd_delete_user(args[0]) != HGD_OK)
		return (HGD_FAIL);

	return (HGD_OK);
}

int
hgd_acmd_user_list(char **args)
{
	struct hgd_user_list	*list = hgd_get_all_users();
	int			 i;

	/* sssh */
	args = args;

	for (i = 0; i < list->n_users; i++)
		printf("%s\n", list->users[i]->name);

	hgd_free_user_list(list);
	free(list);

	return (HGD_OK);

}

struct hgd_admin_cmd admin_cmds[] = {
	{ "user-add", 2, hgd_acmd_user_add },
	{ "user-add", 1, hgd_acmd_user_add_prompt },
	{ "user-del", 1, hgd_acmd_user_del },
	{ "user-list", 0, hgd_acmd_user_list },
#if 0
	{ "user-disable", 1, hgd_acmd_user_disable },
	{ "user-chpw", 1, hgd_acmd_user_chpw },
	{ "user-enable", 1, hgd_acmd_user_enable },
#endif
	{ 0, 0, NULL }
};

int
hgd_parse_command(int argc, char **argv)
{
	struct hgd_admin_cmd	*acmd, *correct_acmd = NULL;

	DPRINTF(HGD_D_DEBUG, "Looking for command handler for '%s'", argv[0]);

	for (acmd = admin_cmds; acmd->cmd != 0; acmd++) {
		if ((acmd->num_args == argc -1) &&
		    (strcmp(acmd->cmd, argv[0]) == 0))
			correct_acmd = acmd;
	}

	if (correct_acmd == NULL) {
		DPRINTF(HGD_D_WARN, "Incorrect usage: '%s' with %d args",
		    argv[0], argc - 1);
		return (HGD_FAIL);
	}

	correct_acmd->handler(++argv);

	return (HGD_OK);
}

int
main(int argc, char **argv)
{
	char			 ch;

	hgd_register_sig_handlers();
	hgd_dir = xstrdup(HGD_DFL_DIR);

	DPRINTF(HGD_D_DEBUG, "Parsing options");
	while ((ch = getopt(argc, argv, "d:hvx:")) != -1) {
		switch (ch) {
		case 'd':
			free(hgd_dir);
			hgd_dir = xstrdup(optarg);
			DPRINTF(HGD_D_DEBUG, "set hgd dir to '%s'", hgd_dir);
			break;
		case 'v':
			hgd_print_version();
			exit_ok = 1;
			hgd_exit_nicely();
			break;
		case 'x':
			hgd_debug = atoi(optarg);
			if (hgd_debug > 3)
				hgd_debug = 3;
			DPRINTF(HGD_D_DEBUG,
			    "set debug level to %d", hgd_debug);
			break;
		case 'h':
		default:
			hgd_usage();
			exit_ok = 1;
			hgd_exit_nicely();
			break;
		};
	}

	argc -= optind;
	argv += optind;

	xasprintf(&db_path, "%s/%s", hgd_dir, HGD_DB_NAME);
	xasprintf(&filestore_path, "%s/%s", hgd_dir, HGD_FILESTORE_NAME);

	umask(~S_IRWXU);
	hgd_mk_state_dir();

	db = hgd_open_db(db_path);
	if (db == NULL)
		hgd_exit_nicely();

	if (hgd_parse_command(argc, argv) == -1)
		hgd_usage();

	exit_ok = 1;
	hgd_exit_nicely();
	_exit (EXIT_SUCCESS); /* NOREACH */
}
