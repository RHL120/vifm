/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <curses.h>
#include <regex.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "menus.h"
#include "status.h"
#include "utils.h"

#include "filetype.h"

const assoc_record_t VIFM_PSEUDO_PROG =
{
	.command = VIFM_PSEUDO_CMD,
	.description = "Enter directory",
};

const assoc_record_t NONE_PSEUDO_PROG =
{
	.command = "",
	.description = "",
};

/* Internal list that stores only currently active associations.
 * Since it holds only copies of structures from filetype and filextype lists,
 * it doesn't consume much memory, and its items shouldn't be freed */
static assoc_list_t active_filetypes;

TESTABLE_STATIC void replace_double_comma(char *cmd, int put_null);
static int get_filetype_number(const char *file, assoc_list_t assoc_list);
static void assoc_programs(const char *pattern, const char *records,
		int for_x);
static void register_assoc(assoc_t assoc, int for_x);
static void add_assoc(assoc_list_t *assoc_list, assoc_t assoc);
static void assoc_viewer(const char *pattern, const char *viewer);
static void reset_list(assoc_list_t *assoc_list);
static void reset_list_head(assoc_list_t *assoc_list);
static void free_assoc(assoc_t *assoc);
static void safe_free(char **adr);

static char *
to_regex(const char *global)
{
	char *result = strdup("^$");
	int result_len = 1;
	while(*global != '\0')
	{
		if(strchr("^.$()|+{", *global) != NULL)
		{
		  if(*global != '^' || result[result_len - 1] != '[')
			{
				result = realloc(result, result_len + 2 + 1 + 1);
				result[result_len++] = '\\';
			}
		}
		else if(*global == '!' && result[result_len - 1] == '[')
		{
			result = realloc(result, result_len + 2 + 1 + 1);
			result[result_len++] = '^';
			continue;
		}
		else if(*global == '\\')
		{
			result = realloc(result, result_len + 2 + 1 + 1);
			result[result_len++] = *global++;
		}
		else if(*global == '?')
		{
			result = realloc(result, result_len + 1 + 1 + 1);
			result[result_len++] = '.';
			global++;
			continue;
		}
		else if(*global == '*')
		{
			if(result_len == 1)
			{
				result = realloc(result, result_len + 9 + 1 + 1);
				result[result_len++] = '[';
				result[result_len++] = '^';
				result[result_len++] = '.';
				result[result_len++] = ']';
				result[result_len++] = '.';
				result[result_len++] = '*';
			}
			else
			{
				result = realloc(result, result_len + 2 + 1 + 1);
				result[result_len++] = '.';
				result[result_len++] = '*';
			}
			global++;
			continue;
		}
		else
		{
			result = realloc(result, result_len + 1 + 1 + 1);
		}
		result[result_len++] = *global++;
	}
	result[result_len++] = '$';
	result[result_len] = '\0';
	return result;
}

static int
global_matches(const char *global, const char *file)
{
	char *regex;
	regex_t re;

	regex = to_regex(global);

	if(regcomp(&re, regex, REG_EXTENDED | REG_ICASE) == 0)
	{
		if(regexec(&re, file, 0, NULL, 0) == 0)
		{
			regfree(&re);
			free(regex);
			return 1;
		}
	}
	regfree(&re);
	free(regex);
	return 0;
}

TESTABLE_STATIC void
replace_double_comma(char *cmd, int put_null)
{
	char *p = cmd;
	while(*cmd != '\0')
	{
		if(cmd[0] == ',')
		{
			if(cmd[1] == ',')
			{
				*p++ = *cmd++;
				cmd++;
				continue;
			}
			else if(put_null)
			{
				break;
			}
		}
		*p++ = *cmd++;
	}
	*p = '\0';
}

int
get_default_program_for_file(const char *file, assoc_record_t *result)
{
	assoc_record_t prog;
	int i;

	i = get_filetype_number(file, active_filetypes);
	if(i < 0)
	{
		return 0;
	}

	prog = active_filetypes.list[i].records.list[0];
	result->command = strdup(prog.command);
	result->description = strdup(prog.description);

	replace_double_comma(result->command, 1);
	return 1;
}

char *
get_viewer_for_file(char *file)
{
	int i = get_filetype_number(file, fileviewers);

	if(i < 0)
	{
		return NULL;
	}

	return fileviewers.list[i].records.list[0].command;
}

static int
get_filetype_number(const char *file, assoc_list_t assoc_list)
{
	int i;
	for(i = 0; i < assoc_list.count; i++)
	{
		if(global_matches(assoc_list.list[i].pattern, file))
		{
			return i;
		}
	}
	return -1;
}

assoc_records_t
get_all_programs_for_file(const char *file)
{
	int i;
	assoc_records_t result = {};

	for(i = 0; i < active_filetypes.count; i++)
	{
		assoc_records_t progs;
		int j;

		if(!global_matches(active_filetypes.list[i].pattern, file))
		{
			continue;
		}

		progs = active_filetypes.list[i].records;
		for(j = 0; j < progs.count; j++)
		{
			assoc_record_t prog = progs.list[j];
			add_assoc_record(&result, prog.command, prog.description);
		}
	}

	return result;
}

void
set_programs(const char *patterns, const char *records, int x)
{
	char *exptr;
	char *ex_copy = strdup(patterns);
	char *free_this = ex_copy;
	while((exptr = strchr(ex_copy, ',')) != NULL)
	{
		*exptr = '\0';

		assoc_programs(ex_copy, records, x);

		ex_copy = exptr + 1;
	}
	assoc_programs(ex_copy, records, x);
	free(free_this);
}

static void
assoc_programs(const char *pattern, const char *records, int for_x)
{
	assoc_t assoc;
	char *prog;
	char *free_this;

	if(pattern[0] == '\0')
	{
		return;
	}

	assoc.pattern = strdup(pattern);
	assoc.records.list = NULL;
	assoc.records.count = 0;

	prog = strdup(records);
	free_this = prog;

	while(prog != NULL)
	{
		char *ptr;
		const char *description = "";

		if((ptr = strchr(prog, ',')) != NULL)
		{
			while(ptr != NULL && ptr[1] == ',')
			{
				ptr = strchr(ptr + 2, ',');
			}
			if(ptr != NULL)
			{
				*ptr = '\0';
				ptr++;
			}
		}

		while(isspace(*prog) || *prog == ',')
		{
			prog++;
		}

		if(*prog == '{')
		{
			char *p = strchr(prog + 1, '}');
			if(p != NULL)
			{
				*p = '\0';
				description = prog + 1;
				prog = skip_whitespace(p + 1);
			}
		}

		if(prog[0] != '\0')
		{
			replace_double_comma(prog, 0);
			add_assoc_record(&assoc.records, prog, description);
		}
		prog = ptr;
	}

	free(free_this);

	register_assoc(assoc, for_x);
}

static void
register_assoc(assoc_t assoc, int for_x)
{
	add_assoc(for_x ? &xfiletypes : &filetypes, assoc);
	if(!for_x || !curr_stats.is_console)
	{
		add_assoc(&active_filetypes, assoc);
	}
}

void
set_fileviewer(const char *patterns, const char *viewer)
{
	char *exptr;
	char *ex_copy = strdup(patterns);
	char *free_this = ex_copy;
	while((exptr = strchr(ex_copy, ',')) != NULL)
	{
		*exptr = '\0';

		assoc_viewer(ex_copy, viewer);

		ex_copy = exptr + 1;
	}
	assoc_viewer(ex_copy, viewer);
	free(free_this);
}

static void
assoc_viewer(const char *pattern, const char *viewer)
{
	int i;

	if(pattern[0] == '\0')
	{
		return;
	}

	for(i = 0; i < fileviewers.count; i++)
	{
		if(strcasecmp(fileviewers.list[i].pattern, pattern) == 0)
		{
			break;
		}
	}
	if(i == fileviewers.count)
	{
		assoc_t assoc =
		{
			.pattern = strdup(pattern),
			.records.list = NULL,
			.records.count = 0,
		};
		add_assoc_record(&assoc.records, viewer, "");
		add_assoc(&fileviewers, assoc);
	}
	else
	{
		free(fileviewers.list[i].records.list[0].command);
		fileviewers.list[i].records.list[0].command = strdup(viewer);
	}
}

static void
add_assoc(assoc_list_t *assoc_list, assoc_t assoc)
{
	void *p = realloc(assoc_list->list, (assoc_list->count + 1)*sizeof(assoc_t));
	if(p == NULL)
	{
		(void)show_error_msg("Memory Error", "Unable to allocate enough memory");
		return;
	}

	assoc_list->list = p;
	assoc_list->list[assoc_list->count] = assoc;
	assoc_list->count++;
}

void
reset_all_file_associations(void)
{
	reset_list(&filetypes);
	reset_list(&xfiletypes);
	reset_list(&fileviewers);

	reset_list_head(&active_filetypes);
}

static void
reset_list(assoc_list_t *assoc_list)
{
	int i;
	for(i = 0; i < assoc_list->count; i++)
	{
		free_assoc(&assoc_list->list[i]);
	}
	reset_list_head(assoc_list);
}

static void
reset_list_head(assoc_list_t *assoc_list)
{
	free(assoc_list->list);
	assoc_list->list = NULL;
	assoc_list->count = 0;
}

static void
free_assoc(assoc_t *assoc)
{
	safe_free(&assoc->pattern);
	free_assoc_records(&assoc->records);
}

void
free_assoc_records(assoc_records_t *records)
{
	int i;
	for(i = 0; i < records->count; i++)
	{
		free_assoc_record(&records->list[i]);
	}

	free(records->list);
	records->list = NULL;
	records->count = 0;
}

void
free_assoc_record(assoc_record_t *record)
{
	safe_free(&record->command);
	safe_free(&record->description);
}

void
add_assoc_record(assoc_records_t *records, const char *command,
		const char *description)
{
	void *p = realloc(records->list, sizeof(assoc_record_t)*(records->count + 1));
	if(p == NULL)
	{
		(void)show_error_msg("Memory Error", "Unable to allocate enough memory");
		return;
	}

	records->list = p;
	records->list[records->count].command = strdup(command);
	records->list[records->count].description = strdup(description);
	records->count++;
}

static void
safe_free(char **adr)
{
	free(*adr);
	*adr = NULL;
}

int
assoc_prog_is_empty(const assoc_record_t *record)
{
	return record->command == NULL && record->description == NULL;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
