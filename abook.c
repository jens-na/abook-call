/*
 * $Id$
 *
 * by JH <jheinonen@users.sourceforge.net>
 *
 * Copyright (C) Jaakko Heinonen
 */

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#if defined(HAVE_LOCALE_H) && defined(HAVE_SETLOCALE)
#	include <locale.h>
#endif
#include <assert.h>
#include "abook.h"
#include "gettext.h"
#include "ui.h"
#include "database.h"
#include "list.h"
#include "filter.h"
#include "edit.h"
#include "misc.h"
#include "options.h"
#include "getname.h"
#include "getopt.h"
#include "views.h"
#include "xmalloc.h"

static void             init_abook();
static void		quit_abook_sig(int i);
static void             set_filenames();
static void             parse_command_line(int argc, char **argv);
static void             show_usage();
static void             mutt_query(char *str);
static void             init_mutt_query();
static void		convert(char *srcformat, char *srcfile,
				char *dstformat, char *dstfile);
static void		add_email(int);

char *datafile = NULL;
static char *rcfile = NULL;

bool alternative_datafile = FALSE;
bool alternative_rcfile = FALSE;


static int
datafile_writeable()
{
	FILE *f;

	assert(datafile != NULL);

	if( (f = fopen(datafile, "a")) == NULL)
		return FALSE;

	fclose(f);

	return TRUE;
}

static void
check_abook_directory()
{
	struct stat s;
	char *dir;

	assert(!is_ui_initialized());

	if(alternative_datafile)
		return;

	dir = strconcat(getenv("HOME"), "/" DIR_IN_HOME, NULL);
	assert(dir != NULL);

	if(stat(dir, &s) == -1) {
		if(errno != ENOENT) {
			perror(dir);
                        free(dir);
                        exit(EXIT_FAILURE);
		}
		if(mkdir(dir, 0700) == -1) {
			printf(_("Cannot create directory %s\n"), dir);
			perror(dir);
			free(dir);
			exit(EXIT_FAILURE);
		}
	} else if(!S_ISDIR(s.st_mode)) {
		printf(_("%s is not a directory\n"), dir);
		free(dir);
		exit(EXIT_FAILURE);
	}

	free(dir);
}

static void
xmalloc_error_handler(int err)
{
	/*
	 * We don't try to save addressbook here because we don't know
	 * if it's fully loaded to memory.
	 */
	if(is_ui_initialized())
		close_ui();

	fprintf(stderr, _("Memory allocation failure: %s\n"), strerror(err));
	exit(EXIT_FAILURE);
}

static void
init_abook()
{
	set_filenames();
	check_abook_directory();
	init_opts();
	if(load_opts(rcfile) > 0) {
		printf(_("Press enter to continue...\n"));
		fgetc(stdin);
	}
	init_default_views();

	signal(SIGTERM, quit_abook_sig);

	init_index();

	if(init_ui())
		exit(EXIT_FAILURE);

	umask(DEFAULT_UMASK);

	if(!datafile_writeable()) {
		char *s = strdup_printf(_("File %s is not writeable"), datafile);
		refresh_screen();
		statusline_msg(s);
		free(s);
		if(load_database(datafile) || !statusline_ask_boolean(
					_("If you continue all changes will "
				"be lost. Do you want to continue?"), FALSE)) {
			free_opts();
			/*close_database();*/
			close_ui();
			exit(EXIT_FAILURE);
		}
	} else
		load_database(datafile);

	refresh_screen();
}

void
quit_abook(int save_db)
{
	if(save_db)  {
		if(opt_get_bool(BOOL_AUTOSAVE))
			save_database();
		else if(statusline_ask_boolean(_("Save database"), TRUE))
			save_database();
	} else if(!statusline_ask_boolean(_("Quit without saving"), FALSE))
		return;

	free_opts();
	close_database();

	close_ui();

	exit(EXIT_SUCCESS);
}

static void
quit_abook_sig(int i)
{
	quit_abook(QUIT_SAVE);
}

int
main(int argc, char **argv)
{
#if defined(HAVE_SETLOCALE) && defined(HAVE_LOCALE_H)
	setlocale(LC_MESSAGES, "");
	setlocale(LC_TIME, "");
	setlocale(LC_CTYPE, "");
	setlocale(LC_COLLATE, "");
#endif
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	xmalloc_set_error_handler(xmalloc_error_handler);

	prepare_database_internals();

	parse_command_line(argc, argv);

	init_abook();

	get_commands();

	quit_abook(QUIT_SAVE);

	return 0;
}

static void
free_filenames()
{
	xfree(rcfile);
	xfree(datafile);
}


static void
set_filenames()
{
	struct stat s;

	if( (stat(getenv("HOME"), &s)) == -1 || ! S_ISDIR(s.st_mode) ) {
		fprintf(stderr,_("%s is not a valid HOME directory\n"), getenv("HOME") );
		exit(EXIT_FAILURE);
	}

	if(!datafile)
		datafile = strconcat(getenv("HOME"), "/" DIR_IN_HOME "/"
				DATAFILE, NULL);

	if(!rcfile)
		rcfile = strconcat(getenv("HOME"), "/" DIR_IN_HOME "/"
				RCFILE, NULL);

	atexit(free_filenames);
}

/*
 * CLI
 */

enum {
	MODE_CONT,
	MODE_ADD_EMAIL,
	MODE_ADD_EMAIL_QUIET,
	MODE_QUERY,
	MODE_CONVERT
};

static void
change_mode(int *current, int mode)
{
	if(*current != MODE_CONT) {
		fprintf(stderr, _("Cannot combine options --mutt-query, "
				"--convert, "
				"--add-email or --add-email-quiet\n"));
		exit(EXIT_FAILURE);
	}

	*current = mode;
}

void
set_filename(char **var, char *path)
{
	char *cwd;

	assert(var != NULL);
	assert(*var == NULL); /* or else we probably leak memory */
	assert(path != NULL);

	if(*path == '/') {
		*var = xstrdup(path);
		return;
	}

	cwd = my_getcwd();

	*var = strconcat(cwd, "/", path, NULL);

	free(cwd);
}

#define set_convert_var(X) do { if(mode != MODE_CONVERT) {\
	fprintf(stderr, _("please use option --%s after --convert option\n"),\
			long_options[option_index].name);\
		exit(EXIT_FAILURE);\
	} else\
		X = optarg;\
	} while(0)

static void
parse_command_line(int argc, char **argv)
{
	int mode = MODE_CONT;
	char *query_string = NULL;
	char *informat = "abook",
		*outformat = "text",
		*infile = "-",
		*outfile = "-";
	int c;

	for(;;) {
		int option_index = 0;
		enum {
			OPT_ADD_EMAIL,
			OPT_ADD_EMAIL_QUIET,
			OPT_MUTT_QUERY,
			OPT_CONVERT,
			OPT_INFORMAT,
			OPT_OUTFORMAT,
			OPT_INFILE,
			OPT_OUTFILE,
			OPT_FORMATS
		};
		static struct option long_options[] = {
			{ "help", 0, 0, 'h' },
			{ "add-email", 0, 0, OPT_ADD_EMAIL },
			{ "add-email-quiet", 0, 0, OPT_ADD_EMAIL_QUIET },
			{ "datafile", 1, 0, 'f' },
			{ "mutt-query", 1, 0, OPT_MUTT_QUERY },
			{ "config", 1, 0, 'C' },
			{ "convert", 0, 0, OPT_CONVERT },
			{ "informat", 1, 0, OPT_INFORMAT },
			{ "outformat", 1, 0, OPT_OUTFORMAT },
			{ "infile", 1, 0, OPT_INFILE },
			{ "outfile", 1, 0, OPT_OUTFILE },
			{ "formats", 0, 0, OPT_FORMATS },
			{ 0, 0, 0, 0 }
		};

		c = getopt_long(argc, argv, "hC:",
				long_options, &option_index);

		if(c == -1)
			break;

		switch(c) {
			case 'h':
				show_usage();
				exit(EXIT_SUCCESS);
			case OPT_ADD_EMAIL:
				change_mode(&mode, MODE_ADD_EMAIL);
				break;
			case OPT_ADD_EMAIL_QUIET:
				change_mode(&mode, MODE_ADD_EMAIL_QUIET);
				break;
			case 'f':
				set_filename(&datafile, optarg);
				alternative_datafile = TRUE;
				break;
			case OPT_MUTT_QUERY:
				query_string = optarg;
				change_mode(&mode, MODE_QUERY);
				break;
			case 'C':
				set_filename(&rcfile, optarg);
				alternative_rcfile = TRUE;
				break;
			case OPT_CONVERT:
				change_mode(&mode, MODE_CONVERT);
				break;
			case OPT_INFORMAT:
				set_convert_var(informat);
				break;
			case OPT_OUTFORMAT:
				set_convert_var(outformat);
				break;
			case OPT_INFILE:
				set_convert_var(infile);
				break;
			case OPT_OUTFILE:
				set_convert_var(outfile);
				break;
			case OPT_FORMATS:
				print_filters();
				exit(EXIT_SUCCESS);
			default:
				exit(EXIT_FAILURE);
		}
	}

	if(optind < argc) {
		fprintf(stderr, _("%s: unrecognized arguments on command line\n"),
				argv[0]);
		exit(EXIT_FAILURE);
	}

	switch(mode) {
		case MODE_ADD_EMAIL:
			add_email(0);
		case MODE_ADD_EMAIL_QUIET:
			add_email(1);
		case MODE_QUERY:
			mutt_query(query_string);
		case MODE_CONVERT:
			convert(informat, infile, outformat, outfile);
	}
}


static void
show_usage()
{
	puts	(PACKAGE " v " VERSION "\n");
	puts	(_("     -h	--help				show usage"));
	puts	(_("     -C	--config	<file>		use an alternative configuration file"));
	puts	(_("	--datafile	<file>		use an alternative addressbook file"));
	puts	(_("	--mutt-query	<string>	make a query for mutt"));
	puts	(_("	--add-email			"
			"read an e-mail message from stdin and\n"
		"					"
		"add the sender to the addressbook"));
	puts	(_("	--add-email-quiet		"
		"same as --add-email but doesn't\n"
		"					require to confirm adding"));
	putchar('\n');
	puts	(_("	--convert			convert address book files"));
	puts	(_("	options to use with --convert:"));
	puts	(_("	--informat	<format>	format for input file"));
	puts	(_("					(default: abook)"));
	puts	(_("	--infile	<file>		source file"));
	puts	(_("					(default: stdin)"));
	puts	(_("	--outformat	<format>	format for output file"));
	puts	(_("					(default: text)"));
	puts	(_("	--outfile	<file>		destination file"));
	puts	(_("					(default: stdout)"));
	puts	(_("	--formats			list available formats"));
}

/*
 * end of CLI
 */


static void
quit_mutt_query(int status)
{
	close_database();
	free_opts();

	exit(status);
}

static void
muttq_print_item(FILE *file, int item)
{
	abook_list *emails, *e;
	char *tmp = db_email_get(item);

	emails = csv_to_abook_list(tmp);
	free(tmp);

	for(e = emails; e; e = e->next) {
		fprintf(file, "%s\t%s\t%s\n", e->data, db_name_get(item),
				!db_fget(item, NOTES) ?" " :db_fget(item, NOTES)
				);
		if(!opt_get_bool(BOOL_MUTT_RETURN_ALL_EMAILS))
			break;
	}
	abook_list_free(&emails);
}

static void
mutt_query(char *str)
{
	init_mutt_query();

	if( str == NULL || !strcasecmp(str, "all") ) {
		struct db_enumerator e = init_db_enumerator(ENUM_ALL);
		printf("All items\n");
		db_enumerate_items(e)
			muttq_print_item(stdout, e.item);
	} else {
		int search_fields[] = {NAME, EMAIL, NICK, -1};
		int i;
		if( (i = find_item(str, 0, search_fields)) < 0 ) {
			printf("Not found\n");
			quit_mutt_query(EXIT_FAILURE);
		}
		putchar('\n');
		while(i >= 0) {
			muttq_print_item(stdout, i);
			i = find_item(str, i + 1, search_fields);
		}
	}

	quit_mutt_query(EXIT_SUCCESS);
}

static void
init_mutt_query()
{
	set_filenames();
	init_opts();
	load_opts(rcfile);

	if( load_database(datafile) ) {
		printf(_("Cannot open database\n"));
		quit_mutt_query(EXIT_FAILURE);
		exit(EXIT_FAILURE);
	}
}


static char *
make_mailstr(int item)
{
	char email[MAX_EMAIL_LEN];
	char *ret;
	char *name = strdup_printf("\"%s\"", db_name_get(item));

	get_first_email(email, item);

	ret = *email ?
		strdup_printf("%s <%s>", name, email) :
		xstrdup(name);

	free(name);

	return ret;
}

void
print_stderr(int item)
{
	fprintf (stderr, "%c", '\n');

	if( is_valid_item(item) )
		muttq_print_item(stderr, item);
	else {
		struct db_enumerator e = init_db_enumerator(ENUM_SELECTED);
		db_enumerate_items(e) {
			muttq_print_item(stderr, e.item);
		}
	}

}

void
launch_mutt(int item)
{
	char *cmd = NULL, *mailstr = NULL;
	char *mutt_command = opt_get_str(STR_MUTT_COMMAND);

	if(mutt_command == NULL || !*mutt_command)
		return;

	if( is_valid_item(item) )
		mailstr = make_mailstr(item);
	else {
		struct db_enumerator e = init_db_enumerator(ENUM_SELECTED);
		char *tmp = NULL;
		db_enumerate_items(e) {
			tmp = mailstr;
			mailstr = tmp ?
				strconcat(tmp, ",", make_mailstr(e.item), NULL):
				strconcat(make_mailstr(e.item), NULL);
			free(tmp);
		}
	}

	cmd = strconcat(mutt_command, " \'", mailstr, "\'", NULL);
	free(mailstr);
#ifdef DEBUG
	fprintf(stderr, "cmd: %s\n", cmd);
#endif
	system(cmd);
	free(cmd);

	/*
	 * we need to make sure that curses settings are correct
	 */
	ui_init_curses();
}

void
launch_wwwbrowser(int item)
{
	char *cmd = NULL;

	if( !is_valid_item(item) )
		return;

	if(db_fget(item, URL))
		cmd = strdup_printf("%s '%s'",
				opt_get_str(STR_WWW_COMMAND),
				safe_str(db_fget(item, URL)));
	else
		return;

	if ( cmd )
		system(cmd);

	free(cmd);

	/*
	 * we need to make sure that curses settings are correct
	 */
	ui_init_curses();
}

void
launch_callpgm(int item, char type)
{
  char *cmd = NULL;
  
  if( !is_valid_item(item) )
	return;

  if(type == 'H')
  {
    if(db_fget(item, PHONE))
	  cmd = strdup_printf("%s '%s'",
		opt_get_str(STR_CALL_COMMAND),
		safe_str(db_fget(item, PHONE)));
  }
  else if(type == 'W')
  {
    if(db_fget(item, WORKPHONE))
	  cmd = strdup_printf("%s '%s'",
		opt_get_str(STR_CALL_COMMAND),
		safe_str(db_fget(item, WORKPHONE)));
  }
  else if(type == 'M')
  {  
    if(db_fget(item, MOBILEPHONE))
	  cmd = strdup_printf("%s '%s'",
		opt_get_str(STR_CALL_COMMAND),
		safe_str(db_fget(item, MOBILEPHONE)));
  }
  else
	return;

  if(cmd)
	system(cmd);

  free(cmd);

  /*
   * we need to make sure that curses settings are correct
   */
  ui_init_curses();
}

FILE *
abook_fopen (const char *path, const char *mode)
{
	struct stat s;
	bool stat_ok;

	stat_ok = (stat(path, &s) != -1);

	if(strchr(mode, 'r'))
		return (stat_ok && S_ISREG(s.st_mode)) ?
			fopen(path, mode) : NULL;
	else
		return (stat_ok && S_ISDIR(s.st_mode)) ?
			NULL : fopen(path, mode);
}

static void
convert(char *srcformat, char *srcfile, char *dstformat, char *dstfile)
{
	int ret=0;

	if( !srcformat || !srcfile || !dstformat || !dstfile ) {
		fprintf(stderr, _("too few arguments to make conversion\n"));
		fprintf(stderr, _("try --help\n"));
	}

#ifndef DEBUG
	if( !strcasecmp(srcformat, dstformat) ) {
		printf(	_("input and output formats are the same\n"
			"exiting...\n"));
		exit(EXIT_FAILURE);
	}
#endif

	set_filenames();
	init_opts();
	load_opts(rcfile);
	init_standard_fields();

	switch(import_file(srcformat, srcfile)) {
		case -1:
			fprintf(stderr,
				_("input format %s not supported\n"), srcformat);
			ret = 1;
			break;
		case 1:
			fprintf(stderr, _("cannot read file %s\n"), srcfile);
			ret = 1;
			break;
	}

	if(!ret)
		switch(export_file(dstformat, dstfile)) {
			case -1:
				fprintf(stderr,
					_("output format %s not supported\n"),
					dstformat);
				ret = 1;
				break;
			case 1:
				fprintf(stderr,
					_("cannot write file %s\n"), dstfile);
				ret = 1;
				break;
		}

	close_database();
	free_opts();
	exit(ret);
}

/*
 * --add-email handling
 */

static int add_email_count = 0;

static void
quit_add_email()
{
	if(add_email_count > 0) {
		if(save_database() < 0) {
			fprintf(stderr, _("cannot open %s\n"), datafile);
			exit(EXIT_FAILURE);
		}
		printf(_("%d item(s) added to %s\n"), add_email_count, datafile);
	} else {
		puts(_("Valid sender address not found"));
	}

	exit(EXIT_SUCCESS);
}

static void
quit_add_email_sig(int signal)
{
	quit_add_email();
}

static void
init_add_email()
{
	set_filenames();
	check_abook_directory();
	init_opts();
	load_opts(rcfile);
	atexit(free_opts);

	/*
	 * we don't actually care if loading fails or not
	 */
	load_database(datafile);

	atexit(close_database);

	signal(SIGINT, quit_add_email_sig);
}

static int
add_email_add_item(int quiet, char *name, char *email)
{
	list_item item;

	if(opt_get_bool(BOOL_ADD_EMAIL_PREVENT_DUPLICATES)) {
		int search_fields[] = { EMAIL, -1 };
		if(find_item(email, 0, search_fields) >= 0) {
			if(!quiet)
				printf(_("Address %s already in addressbook\n"),
						email);
			return 0;
		}
	}

	if(!quiet) {
		FILE *in = fopen("/dev/tty", "r");
		char c;
		if(!in) {
			fprintf(stderr, _("cannot open /dev/tty\n"
				"you may want to use --add-email-quiet\n"));
			exit(EXIT_FAILURE);
		}

		do {
			printf(_("Add \"%s <%s>\" to %s? (%c/%c)\n"),
					name,
					email,
					datafile,
					*S_("keybinding for yes|y"),
					*S_("keybinding for no|n"));
			c = tolower(getc(in));
			if(c == *S_("keybinding for no|n")) {
				fclose(in);
				return 0;
			}
		} while(c != *S_("keybinding for yes|y"));
		fclose(in);
	}

	item = item_create();
	item_fput(item, NAME, xstrdup(name));
	item_fput(item, EMAIL, xstrdup(email));
	add_item2database(item);
	item_free(&item);

	return 1;
}

static void
add_email(int quiet)
{
	char *line;
	char *name = NULL, *email = NULL;
	struct stat s;

	if( (fstat(fileno(stdin), &s)) == -1 || S_ISDIR(s.st_mode) ) {
		fprintf(stderr, _("stdin is a directory or cannot stat stdin\n"));
		exit(EXIT_FAILURE);
	}

	init_add_email();

	do {
		line = getaline(stdin);
		if(line && !strncasecmp("From:", line, 5) ) {
			getname(line, &name, &email);
			add_email_count += add_email_add_item(quiet,
					name, email);
			xfree(name);
			xfree(email);
		}
		xfree(line);
	} while( !feof(stdin) );

	quit_add_email();
}

/*
 * end of --add-email handling
 */
