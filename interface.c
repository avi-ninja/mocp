/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Contributors:
 *  - Kamil Tarkowski <kamilt@interia.pl> - "back" command, sec_to_min_plist(),
 *  		fixes.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#include <locale.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif

#include "log.h"
#include "interface_elements.h"
#include "interface.h"
#include "main.h"
#include "playlist.h"
#include "protocol.h"
#include "keys.h"
#include "options.h"
#include "files.h"

#define INTERFACE_LOG	"mocp_client_log"

/* Socket of the server connection. */
static int srv_sock = -1;

static struct plist *playlist = NULL; /* our playlist */
static struct plist *dir_plist = NULL; /* content of the current directory */

/* Queue for events comming from the server. */
static struct event_queue events;

/* Current working directory (the directory we show). */
static char cwd[PATH_MAX] = "";

/* If the user presses quit, or we receive a termination signal. */
static volatile int want_quit = 0;

/* If user presses CTRL-C, set this to 1. This should interrupt long operations
 * that blocks the interface. */
static volatile int wants_interrupt = 0;

#ifdef SIGWINCH
/* If we get SIGWINCH. */
static volatile int want_resize = 0;
#endif

static void sig_quit (int sig ATTR_UNUSED)
{
	want_quit = 1;
}

static void sig_interrupt (int sig)
{
	logit ("Got signal %d: interrupt the operation", sig);
	wants_interrupt = 1;
}

#ifdef SIGWINCH
static void sig_winch (int sig ATTR_UNUSED)
{
	want_resize = 1;
}
#endif

int user_wants_interrupt ()
{
	return wants_interrupt;
}

static void clear_interrupt ()
{
	wants_interrupt = 0;
}

static void send_int_to_srv (const int num)
{
	if (!send_int(srv_sock, num))
		fatal ("Can't send() int to the server.");
}

static void send_str_to_srv (const char *str)
{
	if (!send_str(srv_sock, str))
		fatal ("Can't send() string to the server.");
}

static int get_int_from_srv ()
{
	int num;
	
	if (!get_int(srv_sock, &num))
		fatal ("Can't receive value from the server.");

	return num;
}

/* Returned memory is malloc()ed. */
static char *get_str_from_srv ()
{
	char *str = get_str (srv_sock);
	
	if (!str)
		fatal ("Can't receive string from the server.");

	return str;
}

/* Noblocking version of get_int_from_srv(): return 0 if there are no data. */
static int get_int_from_srv_noblock (int *num)
{
	enum noblock_io_status st;
	
	if ((st = get_int_noblock(srv_sock, num)) == NB_IO_ERR)
		interface_fatal ("Can't receive value from the server.");

	return st == NB_IO_OK ? 1 : 0;
}

static struct plist_item *recv_item_from_srv ()
{
	struct plist_item *item;

	if (!(item = recv_item(srv_sock)))
		fatal ("Can't receive item from the server.");

	return item;
}

static struct tag_ev_response *recv_tags_data_from_srv ()
{
	struct tag_ev_response *r;
	
	r = (struct tag_ev_response *)xmalloc (sizeof(struct tag_ev_response));

	r->file = get_str_from_srv ();
	if (!(r->tags = recv_tags(srv_sock)))
		fatal ("Can't receive tags event's data from the server.");

	return r;
}

/* Receive data for the given type of event and return them. Return NULL if
 * there is no data for the event. */
static void *get_event_data (const int type)
{
	switch (type) {
		case EV_PLIST_ADD:
			return recv_item_from_srv ();
		case EV_PLIST_DEL:
		case EV_STATUS_MSG:
			return get_str_from_srv ();
		case EV_FILE_TAGS:
			return recv_tags_data_from_srv ();
	}

	return NULL;
}

/* Wait for EV_DATA handling other events. */
static void wait_for_data ()
{
	int event;
	
	do {
		event = get_int_from_srv ();
		
		if (event != EV_DATA)
			event_push (&events, event, get_event_data(event));
	 } while (event != EV_DATA);
}

/* Get an integer value from the server that will arrive after EV_DATA. */
static int get_data_int ()
{
	wait_for_data ();
	return get_int_from_srv ();
}

/* Get a string value from the server that will arrive after EV_DATA. */
static char *get_data_str ()
{
	wait_for_data ();
	return get_str_from_srv ();
}

static void send_tags_request (const char *file, const int tags_sel)
{
	assert (file != NULL);
	assert (tags_sel != 0);

	send_int_to_srv (CMD_GET_FILE_TAGS);
	send_str_to_srv (file);
	send_int_to_srv (tags_sel);
}

static void init_playlists ()
{
	dir_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (dir_plist);
	playlist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (playlist);

	/* set serial numbers for the playlist */
	send_int_to_srv (CMD_GET_SERIAL);
	plist_set_serial (playlist, get_data_int());
}

/* Get an integer option from the server (like shuffle) and set it. */
static void sync_int_option (const char *name)
{
	int value;
	
	send_int_to_srv (CMD_GET_OPTION);
	send_str_to_srv (name);
	value = get_data_int ();
	option_set_int (name, value);
	iface_set_option_state (name, value);
}

/* Get the server options and set our options like them. */
static void get_server_options ()
{
	sync_int_option ("Shuffle");
	sync_int_option ("Repeat");
	sync_int_option ("AutoNext");
}

static void update_mixer_name ()
{
	char *name;
	
	send_int_to_srv (CMD_GET_MIXER_CHANNEL_NAME);
	name = get_data_str ();

	assert (strlen(name) <= 14);

	iface_set_mixer_name (name);
	free (name);
}

/* Make new cwd path from CWD and this path */
static void set_cwd (char *path)
{
	if (path[0] == '/')
		strcpy (cwd, "/"); /* for absolute path */
	else if (!cwd[0]) {
		if (!getcwd(cwd, sizeof(cwd)))
			fatal ("Can't get CWD: %s", strerror(errno));
	}

	resolve_path (cwd, sizeof(cwd), path);
}

/* Try to find the directory we can start and set cwd to it. */
static void set_start_dir ()
{
	if (!getcwd(cwd, sizeof(cwd))) {
		if (errno == ERANGE)
			fatal ("CWD is larger than PATH_MAX!");
		else if (!getenv("HOME"))
			fatal ("$HOME is not set.");
		strncpy (cwd, getenv("HOME"), sizeof(cwd));
		if (cwd[sizeof(cwd)-1])
			fatal ("$HOME is larger than PATH_MAX!");
	}
}

/* Set cwd to last directory written to a file, return 1 on success. */
static int read_last_dir ()
{
	FILE *dir_file;
	int res = 1;
	int read;

	if (!(dir_file = fopen(create_file_name("last_directory"), "r")))
		return 0;

	if ((read = fread(cwd, sizeof(char), sizeof(cwd)-1, dir_file)) == 0)
		res = 0;
	else
		cwd[read] = 0;

	fclose (dir_file);
	return res;
}

/* Check if dir2 is in dir1 */
static int is_subdir (const char *dir1, const char *dir2)
{
	return !strncmp(dir1, dir2, strlen(dir1)) ? 1 : 0;
}

static int qsort_strcmp_func (const void *a, const void *b)
{
	return strcmp (*(char **)a, *(char **)b);
}

static int qsort_dirs_func (const void *a, const void *b)
{
	char *sa = *(char **)a;
	char *sb = *(char **)b;
	
	/* '../' is always first */
	if (!strcmp(sa, "../"))
		return -1;
	if (!strcmp(sb, "../"))
		return 1;
	
	return strcmp (sa, sb);
}

/* Send requests for the given tags for every file on the playlist. */
static void ask_for_tags (const struct plist *plist, const int tags_sel)
{
	int i;

	assert (plist != NULL);
	assert (tags_sel != 0);
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			char *file = plist_get_file (plist, i);
			
			send_tags_request (file, tags_sel);
			free (file);
		}
}

/* Update tags (and titles) for the given item on the playlist with new tags. */
static void update_item_tags (struct plist *plist, const int num,
		const struct file_tags *tags)
{
	if (plist->items[num].tags)
		tags_free (plist->items[num].tags);
	plist->items[num].tags = tags_dup (tags);

	if (options_get_int("ReadTags")) {
		make_tags_title (plist, num);
		if (plist->items[num].title_tags)
			plist->items[num].title = plist->items[num].title_tags;
		else {
			if (!plist->items[num].title_file)
				make_file_title (plist, num, options_get_int(
							"HideFileExtension"));
			plist->items[num].title = plist->items[num].title_file;
		}
	}
}

/* Handle EV_FILE_TAGS. */
static void ev_file_tags (const struct tag_ev_response *data)
{
	int n;
	int found;

	assert (data != NULL);
	assert (data->file != NULL);
	assert (data->tags != NULL);

	if ((n = plist_find_fname(dir_plist, data->file)) != -1) {
		update_item_tags (dir_plist, n, data->tags);
		iface_update_item (dir_plist, n);
		found = 1;
	}
	else
		found = 0;
	
	if ((n = plist_find_fname(playlist, data->file)) != -1) {
		update_item_tags (playlist, n, data->tags);
		if (!found) /* don't do it twice */
			iface_update_item (dir_plist, n);
	}
}

/* Handle server event. */
static void server_event (const int event, void *data)
{
	logit ("EVENT: 0x%02x", event);

	switch (event) {
		case EV_BUSY:
			fatal ("The server is busy, another client is "
					"connected.");
			break;
/*		case EV_CTIME:
			update_ctime ();
			break;
		case EV_STATE:
			update_state ();
			break;*/
		case EV_EXIT:
			fatal ("The server exited.");
			break;
/*		case EV_BITRATE:
			update_bitrate ();
			break;
		case EV_RATE:
			update_rate ();
			break;
		case EV_CHANNELS:
			update_channels ();
			break;
		case EV_SRV_ERROR:
			update_error ();
			break;
		case EV_OPTIONS:
			get_server_options ();
			update_info_win ();
			wrefresh (info_win);
			break;
		case EV_SEND_PLIST:
			forward_playlist ();
			break;
		case EV_PLIST_ADD:
			if (options_get_int("SyncPlaylist")) {
				event_plist_add ((struct plist_item *)data);
				if (curr_menu == playlist_menu)
					update_menu ();
			}
			plist_free_item_fields (data);
			free (data);
			break;
		case EV_PLIST_CLEAR:
			if (options_get_int("SyncPlaylist")) {
				clear_playlist ();
				update_menu ();
			}
			break;
		case EV_PLIST_DEL:
			if (options_get_int("SyncPlaylist")) {
				event_plist_del ((char *)data);
				update_menu ();
			}
			free (data);
			break;
		case EV_TAGS:
			update_curr_tags (data);
			break;
		case EV_STATUS_MSG:
			set_iface_status_ref (data);
			free (data);
			break;
		case EV_MIXER_CHANGE:
			ev_mixer_change ();
			break;*/
		case EV_FILE_TAGS:
			ev_file_tags ((struct tag_ev_response *)data);
			free_tag_ev_data ((struct tag_ev_response *)data);
			break;
		default:
			fatal ("Unknown event: 0x%02x", event);
	}
}

/* Load the directory content into dir_plist and switch the menu to it.
 * If dir is NULL, go to the cwd.
 * Return 1 on success, 0 on error. */
static int go_to_dir (const char *dir)
{
	struct plist *old_dir_plist;
	char last_dir[PATH_MAX];
	const char *new_dir = dir ? dir : cwd;
	int going_up = 0;
	struct file_list *dirs, *playlists;

	iface_set_status ("reading directory...");

	if (dir && is_subdir(dir, cwd)) {
		strcpy (last_dir, strrchr(cwd, '/') + 1);
		strcat (last_dir, "/");
		going_up = 1;
	}

	old_dir_plist = dir_plist;
	dir_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (dir_plist);
	dirs = file_list_new ();
	playlists = file_list_new ();

	if (!read_directory(new_dir, dirs, playlists, dir_plist)) {
		iface_set_status ("");
		plist_free (dir_plist);
		file_list_free (dirs);
		file_list_free (playlists);
		free (dir_plist);
		dir_plist = old_dir_plist;
		return 0;
	}

	/* TODO: use CMD_ABORT_TAGS_REQUESTS (what if we requested tags for the
	 playlist?) */

	plist_free (old_dir_plist);
	free (old_dir_plist);

	if (dir) /* if dir is NULL, we went to cwd */
		strcpy (cwd, dir);

	switch_titles_file (dir_plist);


	plist_sort_fname (dir_plist);
	qsort (dirs->items, dirs->num, sizeof(char *), qsort_dirs_func);
	qsort (playlists->items, playlists->num, sizeof(char *),
			qsort_strcmp_func);
	
	if (options_get_int("ReadTags")) {
		int tags = TAGS_COMMENTS;

		if (!strcasecmp(options_get_str("ShowTime"), "yes"))
			tags |= TAGS_TIME;

		ask_for_tags (dir_plist, tags);
	}

	iface_set_dir_content (dir_plist, dirs, playlists);
	file_list_free (dirs);
	file_list_free (playlists);
	if (going_up)
		iface_set_curr_item_title (last_dir);
	
	iface_set_dir_title (cwd);
	//update_state ();

	return 1;
}

/* Enter to the initial directory or toggle to the initial playlist (only
 * if the function has not been called yet). */
static void enter_first_dir ()
{
	static int first_run = 1;
	
#if 0
	if (options_get_int("StartInMusicDir")) {
		char *music_dir;

		if ((music_dir = options_get_str("MusicDir"))) {
			set_cwd (music_dir);
			if (first_run && file_type(cwd) == F_PLAYLIST
					&& plist_count(playlist) == 0
					&& plist_load(playlist, cwd, NULL)) {
				toggle_plist ();
				cwd[0] = 0;
				first_run = 0;
				return;
			}
			else if (file_type(cwd) == F_DIR && go_to_dir(NULL)) {
				first_run = 0;
				return;
			}
		}
		else
			error ("MusicDir is not set");
	}
#endif
	if (!(read_last_dir() && go_to_dir(NULL))) {
		set_start_dir ();
		if (!go_to_dir(NULL))
			fatal ("Can't enter any directory.");
	}

	first_run = 0;
}

void init_interface (const int sock, const int logging, char **args,
		const int arg_num, const int recursively)
{
	srv_sock = sock;

	if (logging) {
		FILE *logfp;

		if (!(logfp = fopen(INTERFACE_LOG, "a")))
			fatal ("Can't open log file for the interface");
		log_init_stream (logfp);
	}

	logit ("Starting MOC interface...");

	/* set locale acording to the environment variables */
	if (!setlocale(LC_CTYPE, ""))
		logit ("Could not net locate!");

	init_playlists ();
	event_queue_init (&events);
	windows_init ();
	keys_init ();
	get_server_options ();
	update_mixer_name ();

	signal (SIGQUIT, sig_quit);
	/*signal (SIGTERM, sig_quit);*/
	signal (SIGINT, sig_interrupt);
	
#ifdef SIGWINCH
	signal (SIGWINCH, sig_winch);
#endif
	
#if 0
	if (arg_num) {
		process_args (args, arg_num, recursively);
	
		if (plist_count(playlist) == 0) {
			if (!options_get_int("SyncPlaylist")
					|| !get_server_playlist(playlist))
				load_playlist ();
		}
		else if (options_get_int("SyncPlaylist")) {
			struct plist tmp_plist;
			
			/* We have made the playlist from command line. */
			
			/* the playlist should be now clear, but this will give
			 * us the serial number of the playlist used by other
			 * clients. */
			plist_init (&tmp_plist);
			get_server_playlist (&tmp_plist);

			send_int_to_srv (CMD_LOCK);
			send_int_to_srv (CMD_CLI_PLIST_CLEAR);

			plist_set_serial (playlist,
					plist_get_serial(&tmp_plist));
			plist_free (&tmp_plist);

			change_srv_plist_serial ();
		
			set_iface_status_ref ("Notifying clients...");
			send_all_items (playlist);
			set_iface_status_ref (NULL);
			send_int_to_srv (CMD_UNLOCK);

			/* Now enter_first_dir() should not go to the music
			 * directory. */
			option_set_int ("StartInMusicDir", 0);
		}
	}
	else {
		if (!options_get_int("SyncPlaylist")
				|| !get_server_playlist(playlist))
			load_playlist ();
		enter_first_dir ();
	}
#else
	enter_first_dir ();
#endif
	
	send_int_to_srv (CMD_SEND_EVENTS);
}

#ifdef SIGWINCH
/* Initialize the screen again after resizeing xterm */
static void do_resize ()
{
	/* TODO */

#if 0
	endwin ();
	refresh ();
	keypad (main_win, TRUE);
	wresize (main_win, LINES - 4, COLS);
	wresize (info_win, 4, COLS);
	mvwin (info_win, LINES - 4, 0);
	werase (main_win);
	
	entry.width = COLS - strlen(entry.title) - 4;
	entry_end ();

	if (curr_plist_menu)
		menu_update_size (curr_plist_menu, main_win);
	if (playlist_menu)
		menu_update_size (playlist_menu, main_win);

	if (main_win_mode == WIN_MENU) {
		main_border ();
		
		menu_draw (curr_menu);
		update_info_win ();	
		wrefresh (main_win);
	}
	else
		print_help_screen ();

	wrefresh (info_win);
#endif
	logit ("resize");
	want_resize = 0;
}
#endif

static void go_dir_up ()
{
	char *dir;
	char *slash;

	dir = xstrdup (cwd);
	slash = strrchr (dir, '/');
	assert (slash != NULL);
	if (slash == dir)
		*(slash + 1) = 0;
	else
		*slash = 0;

	go_to_dir (dir);
	free (dir);
}

/* Get (generate) a playlist serial from the server and make sure it's not
 * the same as our playlists' serial. */
static int get_safe_serial ()
{
	int serial;

	do {
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
	} while (serial == plist_get_serial(playlist)); /* check only the
							   playlist, because
							   dir_plist has serial
							   -1 */

	return serial;
}

/* Send the playlist to the server. If clear != 0, clear the server's playlist
 * before sending. */
static void send_playlist (struct plist *plist, const int clear)
{
	int i;
	
	if (clear)
		send_int_to_srv (CMD_LIST_CLEAR);
	
	for (i = 0; i < plist->num; i++) {
		if (!plist_deleted(plist, i)) {
			send_int_to_srv (CMD_LIST_ADD);
			send_str_to_srv (plist->items[i].file);
		}
	}
}

/* Send the playlist to the server if necessary and request playing this
 * item. */
static void play_it (const char *file)
{
	int serial; /* serial number of the playlist */
	struct plist *curr_plist;
	
	assert (file != NULL);

	if (iface_in_dir_menu())
		curr_plist = dir_plist;
	else
		curr_plist = playlist;
	
	send_int_to_srv (CMD_LOCK);

	send_int_to_srv (CMD_PLIST_GET_SERIAL);
	serial = get_data_int ();
	
	if (plist_get_serial(curr_plist) == -1
			|| serial != plist_get_serial(curr_plist)) {

		logit ("The server has different playlist");

		serial = get_safe_serial();
		plist_set_serial (curr_plist, serial);
		send_int_to_srv (CMD_PLIST_SET_SERIAL);
		send_int_to_srv (serial);
	
		send_playlist (curr_plist, 1);
	}
	else
		logit ("The server already has my playlist");
	
	send_int_to_srv (CMD_PLAY);
	send_str_to_srv (file);

	send_int_to_srv (CMD_UNLOCK);
}

/* Action when the user selected a file. */
static void go_file ()
{
	enum file_type type = iface_curritem_get_type ();
	char *file = iface_get_curr_file ();

	if (type == F_SOUND || type == F_URL)
		play_it (file);
	else if (type == F_DIR && iface_in_dir_menu()) {
		if (!strcmp(file, ".."))
			go_dir_up ();
		else 
			go_to_dir (file);
	}
#if 0
	else if (type == F_DIR && visible_plist == playlist)
		
		/* the only item on the playlist of type F_DIR is '..' */
		toggle_plist ();
	else if (type == F_PLAYLIST)
		go_to_playlist(menu_item_get_file(curr_menu, selected));
#endif

	free (file);
}

/* Handle key */
static void menu_key (const int ch)
{
	if (iface_in_help()) {
#if 0
		int help_lines;

		get_keys_help (&help_lines);

		if (ch == KEY_DOWN || ch == KEY_NPAGE || ch == '\n') {
			if (help_screen_top + LINES - 5 <= help_lines) {
				help_screen_top++;
				print_help_screen ();
			}
		}
		else if (ch == KEY_UP || ch == KEY_PPAGE) {
			if (help_screen_top > 0) {
				help_screen_top--;
				print_help_screen ();
			}
		}
		else if (ch != KEY_RESIZE) {
			
			/* Switch to menu */
			werase (main_win);
			main_border ();
			menu_draw (curr_menu);
			update_info_win ();
			wrefresh (main_win);
			wrefresh (info_win);
		
			main_win_mode = WIN_MENU;
		}
#endif
	}
#if 0
	else if (entry.type != ENTRY_DISABLED)
		entry_key (ch);
#endif
	else if (!iface_key_is_resize(ch)) {
		enum key_cmd cmd = get_key_cmd (CON_MENU, ch);
		
		switch (cmd) {
			case KEY_CMD_QUIT_CLIENT:
				want_quit = 1;
				break;
			case KEY_CMD_GO:
				go_file ();
				break;
			case KEY_CMD_MENU_DOWN:
			case KEY_CMD_MENU_UP:
			case KEY_CMD_MENU_NPAGE:
			case KEY_CMD_MENU_PPAGE:
			case KEY_CMD_MENU_FIRST:
			case KEY_CMD_MENU_LAST:
				iface_menu_key (cmd);
				break;
#if 0
			case KEY_CMD_QUIT:
				send_int_to_srv (CMD_QUIT);
				want_quit = 1;
				break;
			case KEY_CMD_STOP:
				send_int_to_srv (CMD_STOP);
				break;
			case KEY_CMD_NEXT:
				send_int_to_srv (CMD_NEXT);
				break;
			case KEY_CMD_PREVIOUS:
				send_int_to_srv (CMD_PREV);
				break;
			case KEY_CMD_PAUSE:
				switch_pause ();
				break;
			case KEY_CMD_TOGGLE_READ_TAGS:
				switch_read_tags ();
				do_update_menu = 1;
				break;
			case KEY_CMD_TOGGLE_SHUFFLE:
				toggle_option ("Shuffle");
				break;
			case KEY_CMD_TOGGLE_REPEAT:
				toggle_option ("Repeat");
				break;
			case KEY_CMD_TOGGLE_AUTO_NEXT:
				toggle_option ("AutoNext");
				break;
			case KEY_CMD_TOGGLE_PLAYLIST:
				toggle_plist ();
				do_update_menu = 1;
				break;
			case KEY_CMD_PLIST_ADD_FILE:
				add_file_plist ();
				break;
			case KEY_CMD_PLIST_CLEAR:
				cmd_clear_playlist ();
				do_update_menu = 1;
				break;
			case KEY_CMD_PLIST_ADD_DIR:
				add_dir_plist ();
				break;
			case KEY_CMD_MIXED_DEC_1:
				adjust_mixer (-1);
				break;
			case KEY_CMD_MIXER_DEC_5:
				adjust_mixer (-5);
				break;
			case KEY_CMD_MIXER_INC_5:
				adjust_mixer (+5);
				break;
			case KEY_CMD_MIXER_INC_1:
				adjust_mixer (+1);
				break;
			case KEY_CMD_SEEK_BACKWARD:
				seek (-options_get_int("SeekTime"));
				break;
			case KEY_CMD_SEEK_FORWARD:
				seek (options_get_int("SeekTime"));
				break;
			case KEY_CMD_HELP:
				help_screen ();
				break;
			case KEY_CMD_HIDE_MESSAGE:
				interface_message (NULL);
				update_info_win ();
				wrefresh (info_win);
				break;
			case KEY_CMD_REFRESH:
				wclear (info_win);
				update_info_win ();
				wrefresh (info_win);
				wclear (main_win);
				do_update_menu = 1;
				break;
			case KEY_CMD_RELOAD:
				if (visible_plist == curr_plist) {
					reread_dir (1);
					do_update_menu = 1;
				}
				break;
			case KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES:
				option_set_int ("ShowHiddenFiles",
						!options_get_int(
							"ShowHiddenFiles"));
				if (visible_plist == curr_plist) {
					reread_dir (1);
					do_update_menu = 1;
				}
				break;
			case KEY_CMD_GO_MUSIC_DIR:
				go_to_music_dir ();
				do_update_menu = 1;
				break;
			case KEY_CMD_PLIST_DEL:
				delete_item ();
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_SEARCH:
				make_entry (ENTRY_SEARCH, "SEARCH");
				break;
			case KEY_CMD_PLIST_SAVE:
				if (plist_count(playlist))
					make_entry (ENTRY_PLIST_SAVE,
							"SAVE PLAYLIST");
				else
					error ("The playlist is "
							"empty.");
				break;
			case KEY_CMD_TOGGLE_SHOW_TIME:
				toggle_show_time ();
				do_update_menu = 1;
				break;
			case KEY_CMD_TOGGLE_SHOW_FORMAT:
				toggle_show_format ();
				do_update_menu = 1;
				break;
			case KEY_CMD_GO_TO_PLAYING_FILE:
				go_to_file_dir ();
				do_update_menu = 1;
				break;
			case KEY_CMD_GO_DIR:
				make_entry (ENTRY_GO_DIR, "GO");
				break;
			case KEY_CMD_GO_URL:
				make_entry (ENTRY_GO_URL, "URL");
				break;
			case KEY_CMD_GO_DIR_UP:
				go_dir_up ();
				do_update_menu = 1;
				break;
			case KEY_CMD_WRONG:
				error ("Bad command");
				break;
			case KEY_CMD_SEEK_FORWARD_5:
				seek_silent (5);
				break;
			case KEY_CMD_SEEK_BACKWARD_5:
				seek_silent (-5);
				break;
			case KEY_CMD_VOLUME_10:
				set_mixer (10);
				break;
			case KEY_CMD_VOLUME_20:
				set_mixer (20);
				break;
			case KEY_CMD_VOLUME_30:
				set_mixer (30);
				break;
			case KEY_CMD_VOLUME_40:
				set_mixer (40);
				break;
			case KEY_CMD_VOLUME_50:
				set_mixer (50);
				break;
			case KEY_CMD_VOLUME_60:
				set_mixer (60);
				break;
			case KEY_CMD_VOLUME_70:
				set_mixer (70);
				break;
			case KEY_CMD_VOLUME_80:
				set_mixer (80);
				break;
			case KEY_CMD_VOLUME_90:
				set_mixer (90);
				break;
			case KEY_CMD_FAST_DIR_1:
				if (options_get_str("FastDir1")) {
					go_to_dir (options_get_str(
								"FastDir1"));
					do_update_menu = 1;
				}
				else
					error ("FastDir1 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_2:
				if (options_get_str("FastDir2")) {
					go_to_dir (options_get_str(
								"FastDir2"));
					do_update_menu = 1;
				}
				else
					error ("FastDir2 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_3:
				if (options_get_str("FastDir3")) {
					go_to_dir (options_get_str(
								"FastDir3"));
					do_update_menu = 1;
				}
				else
					error ("FastDir3 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_4:
				if (options_get_str("FastDir4")) {
					go_to_dir (options_get_str(
								"FastDir4"));
					do_update_menu = 1;
				}
				else
					error ("FastDir4 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_5:
				if (options_get_str("FastDir5")) {
					go_to_dir (options_get_str(
								"FastDir5"));
					do_update_menu = 1;
				}
				else
					error ("FastDir5 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_6:
				if (options_get_str("FastDir6")) {
					go_to_dir (options_get_str(
								"FastDir6"));
					do_update_menu = 1;
				}
				else
					error ("FastDir6 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_7:
				if (options_get_str("FastDir7")) {
					go_to_dir (options_get_str(
								"FastDir7"));
					do_update_menu = 1;
				}
				else
					error ("FastDir7 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_8:
				if (options_get_str("FastDir8")) {
					go_to_dir (options_get_str(
								"FastDir8"));
					do_update_menu = 1;
				}
				else
					error ("FastDir8 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_9:
				if (options_get_str("FastDir9")) {
					go_to_dir (options_get_str(
								"FastDir9"));
					do_update_menu = 1;
				}
				else
					error ("FastDir9 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_10:
				if (options_get_str("FastDir10")) {
					go_to_dir (options_get_str(
								"FastDir10"));
					do_update_menu = 1;
				}
				else
					error ("FastDir10 not "
							"defined");
				break;
			case KEY_CMD_TOGGLE_MIXER:
				send_int_to_srv (CMD_TOGGLE_MIXER_CHANNEL);
				break;
#endif
			default:
				//abort ();
		}
	}
}

/* Get event from the server and handle it. */
static void get_and_handle_event ()
{
	int type;

	if (!get_int_from_srv_noblock(&type)) {
		debug ("Getting event would block.");
		return;
	}

	server_event (type, get_event_data(type));
}

/* Handle events from the queue. */
static void dequeue_events ()
{
	struct event *e;
	
	debug ("Dequeuing events...");

	while ((e = event_get_first(&events))) {
		server_event (e->type, e->data);
		event_pop (&events);
	}

	debug ("done");
}

void interface_loop ()
{
	while (!want_quit) {
		fd_set fds;
		int ret;
		struct timeval timeout = { 1, 0 };
		
		FD_ZERO (&fds);
		FD_SET (srv_sock, &fds);
		FD_SET (STDIN_FILENO, &fds);

		ret = select (srv_sock + 1, &fds, NULL, NULL, &timeout);
		
		if (ret == 0) {
			/*if (msg_timeout && msg_timeout < time(NULL)
					&& !msg_is_error) {
				update_info_win ();
				wrefresh (info_win);
				msg_timeout = 0;
			}*/

			//do_silent_seek ();
		}
		else if (ret == -1 && !want_quit && errno != EINTR)
			fatal ("select() failed: %s", strerror(errno));

#ifdef SIGWINCH
		if (want_resize)
			do_resize ();
#endif

		if (ret > 0) {
			if (FD_ISSET(STDIN_FILENO, &fds)) {
				int ch = iface_get_char ();

				clear_interrupt ();
				
				menu_key (ch);
				dequeue_events ();
			}

			if (!want_quit) {
				if (FD_ISSET(srv_sock, &fds))
					get_and_handle_event ();
				//do_silent_seek ();
				dequeue_events ();
			}
		}
		else if (user_wants_interrupt())
			/*handle_interrupt ()*/;
	}
}

void interface_end ()
{
	send_int_to_srv (CMD_DISCONNECT);
	close (srv_sock);
	srv_sock = -1;
	
	windows_end ();
	keys_cleanup ();
	
	plist_free (dir_plist);
	plist_free (playlist);
	free (dir_plist);
	free (playlist);

	event_queue_free (&events);
	
	logit ("Interface exited");
}

void interface_fatal (const char *format, ...)
{
	char err_msg[512];
	va_list va;
	
	va_start (va, format);
	vsnprintf (err_msg, sizeof(err_msg), format, va);
	err_msg[sizeof(err_msg) - 1] = 0;
	va_end (va);

	logit ("FATAL ERROR: %s", err_msg);
	windows_end ();
	fatal ("%s", err_msg);
}

void interface_error (const char *msg)
{
}

void interface_cmdline_clear_plist (int server_sock)
{
}

void interface_cmdline_append (int server_sock, char **args,
		const int arg_num)
{
}

void interface_cmdline_play_first (int server_sock)
{
}

void interface_cmdline_file_info (const int server_sock)
{
}

void interface_cmdline_playit (int server_sock, char **args, const int arg_num)
{
}
