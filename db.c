/*
 * Copyright (c) 2011, Edd Barrett <vext01@gmail.com>
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
#include <stdarg.h>
#include <string.h>
#include <err.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <sqlite3.h>

#include "hgd.h"
#include "db.h"

sqlite3				*db = NULL;
char				*db_path = NULL;

/* Open, create and initialise database */
sqlite3 *
hgd_open_db(char *db_path)
{
	int			sql_res;
	char			*sql_err;
	sqlite3			*db;

	/* open the database */
	DPRINTF(HGD_D_DEBUG, "opening database");
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		DPRINTF(HGD_D_ERROR, "Can't open db: %s", sqlite3_errmsg(db));
		return NULL;
	}

	DPRINTF(HGD_D_DEBUG, "Setting database timeout");
	sql_res = sqlite3_busy_timeout(db, 2000);


	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_ERROR, "Can't set busy timout on db: %s",
		    sqlite3_errmsg(db));
		sqlite3_close(db);
		sqlite3_free(sql_err);
		return NULL;
	}

	DPRINTF(HGD_D_DEBUG, "Making playlist table (if needed)");
	sql_res = sqlite3_exec(db,
	    "CREATE TABLE IF NOT EXISTS playlist ("
	    "id INTEGER PRIMARY KEY,"
	    "filename VARCHAR(" HGD_DBS_FILENAME_LEN " ),"
	    "user VARCHAR(" HGD_DBS_USERNAME_LEN "),"
	    "playing INTEGER,"
	    "finished INTEGER)",
	    NULL, NULL, &sql_err);

	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_ERROR, "Can't initialise db: %s",
		    sqlite3_errmsg(db));
		sqlite3_close(db);
		sqlite3_free(sql_err);
		return NULL;
	}

	DPRINTF(HGD_D_DEBUG, "making votes table (if needed)");
	sql_res = sqlite3_exec(db,
	    "CREATE TABLE IF NOT EXISTS votes ("
	    "user VARCHAR(" HGD_DBS_USERNAME_LEN ") PRIMARY KEY)",
	    NULL, NULL, &sql_err);

	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_ERROR, "Can't initialise db: %s",
		    sqlite3_errmsg(db));
		sqlite3_close(db);
		sqlite3_free(sql_err);
		return NULL;
	}

	return db;
}

int
hgd_get_playing_item_cb(void *arg, int argc, char **data, char **names)
{
	struct hgd_playlist_item	*t;

	DPRINTF(HGD_D_DEBUG, "A track is playing");

	/* silence compiler */
	argc = argc;
	names = names;

	t = (struct hgd_playlist_item *) arg;

	/* populate a struct that we pick up later */
	t->id = atoi(data[0]);
	t->filename = strdup(data[1]);
	t->user = strdup(data[2]);

	return SQLITE_OK;
}

int
hgd_get_playing_item(struct hgd_playlist_item **playing)
{
	int				 sql_res;
	char				*sql_err;

	*playing = hgd_new_playlist_item();

	sql_res = sqlite3_exec(db,
	    "SELECT id, filename, user "
	    "FROM playlist WHERE playing=1 LIMIT 1",
	    hgd_get_playing_item_cb, *playing, &sql_err);

	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_ERROR, "Can't get playing track: %s",
		    sqlite3_errmsg(db));
		sqlite3_free(sql_err);
		hgd_free_playlist_item(*playing);
		return (-1);
	}

	return (0);
}

int
hgd_get_num_votes_cb(void *arg, int argc, char **data, char **names)
{
	int			*num = (int *) arg;

	/* quiet */
	argc = argc;
	names = names;

	*num = atoi(data[0]);
	return (0);
}

int
hgd_get_num_votes()
{
	int			sql_res, num = -1;
	char			*sql, *sql_err;

	xasprintf(&sql, "SELECT COUNT (*) FROM votes;");
	sql_res = sqlite3_exec(db, sql, hgd_get_num_votes_cb, &num, &sql_err);
	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_ERROR, "Can't get votes: %s",
		    sqlite3_errmsg(db));
		sqlite3_free(sql_err);
		free(sql);
		return (-1);
	}
	free(sql);

	DPRINTF(HGD_D_DEBUG, "%d votes so far", num);
	return num;
}


int
hgd_insert_track(char *filename, char *user)
{
	int			 ret = -1;
	int			 sql_res;
	sqlite3_stmt		*stmt;
	char			*sql = "INSERT INTO playlist "
	    "(filename, user, playing, finished) VALUES (?, ?, 0, 0)";

	sql_res = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_WARN, "Can't prepare sql: %s",
		    sqlite3_errmsg(db));
		goto clean;
	}

	/* bind params */
	sql_res = sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_TRANSIENT);
	sql_res &= sqlite3_bind_text(stmt, 2, user, -1, SQLITE_TRANSIENT);
	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_WARN, "Can't bind sql: %s", sqlite3_errmsg(db));
		goto clean;
	}

	sql_res = sqlite3_step(stmt);
	if (sql_res != SQLITE_DONE) {
		DPRINTF(HGD_D_WARN, "Can't step sql: %s", sqlite3_errmsg(db));
		goto clean;
	}

	ret = 0; /* everything went ok */
clean:
	sqlite3_finalize(stmt);
	return (ret);
}

int
hgd_insert_vote(char *user)
{
	int			 ret = -1;
	int			 sql_res;
	sqlite3_stmt		*stmt;
	char			*sql = "INSERT INTO votes (user) VALUES (?)";

	sql_res = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_WARN, "Can't prepare sql: %s",
		    sqlite3_errmsg(db));
		goto clean;
	}

	/* bind params */
	sql_res = sqlite3_bind_text(stmt, 1, user, -1, SQLITE_TRANSIENT);
	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_WARN, "Can't bind sql: %s", sqlite3_errmsg(db));
		goto clean;
	}

	sql_res = sqlite3_step(stmt);
	if (sql_res == SQLITE_CONSTRAINT) {
		ret = 1; /* indicates duplicat vote */
		goto clean;
	} else if (sql_res != SQLITE_DONE) {
		DPRINTF(HGD_D_WARN, "Can't step sql: %s", sqlite3_errmsg(db));
		goto clean;
	}

	ret = 0; /* everything went ok */
clean:
	sqlite3_finalize(stmt);
	return (ret);
}

int
hgd_get_playlist_cb(void *arg, int argc, char **data, char **names)
{
	struct hgd_playlist		*list;
	struct hgd_playlist_item	*item;

	/* shaddap gcc */
	argc = argc;
	names = names;

	list = (struct hgd_playlist *) arg;

	item = xmalloc(sizeof(struct hgd_playlist_item));

	item->id = atoi(data[0]);
	item->filename = strdup(data[1]);
	item->user = strdup(data[2]);
	item->playing = 0;	/* don't need */
	item->finished = 0;	/* don't need */

	/* remove unique string from filename, only playd uses that */
	item->filename[strlen(item->filename) - 9] = 0;

	list->items = xrealloc(list->items,
	    sizeof(struct hgd_playlist_item *) * (list->n_items + 1));
	list->items[list->n_items] = item;

	list->n_items++;

	return (SQLITE_OK);
}

/*
 * report back items in the playlist
 */
int
hgd_get_playlist(struct hgd_playlist *list)
{
	int			sql_res;
	char			*sql_err;

	list->n_items = 0;
	list->items = NULL;

	DPRINTF(HGD_D_DEBUG, "Playlist request");

	sql_res = sqlite3_exec(db,
	    "SELECT id, filename, user FROM playlist WHERE finished=0",
	    hgd_get_playlist_cb, list, &sql_err);

	if (sql_res != SQLITE_OK) {
		DPRINTF(HGD_D_ERROR, "Can't get playing track: %s",
		    sqlite3_errmsg(db));
		sqlite3_free(sql_err);
		return (-1);
	}

	return (0);
}