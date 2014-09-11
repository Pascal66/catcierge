//
// This file is part of the Catcierge project.
//
// Copyright (c) Joakim Soderberg 2013-2014
//
//    Catcierge is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    Catcierge is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Catcierge.  If not, see <http://www.gnu.org/licenses/>.
//

#include "catcierge_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef CATCIERGE_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef CATCIERGE_HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef CATCIERGE_HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "catcierge_log.h"
#include "catcierge_util.h"
#include "catcierge_types.h"
#include "catcierge_output.h"
#include "catcierge_output_types.h"
#include "catcierge_fsm.h"
#include "catcierge_strftime.h"

catcierge_output_var_t vars[] =
{
	{ "state", "The current state machine state." },
	{ "prev_state", "The previous state machine state."},
	{ "matcher", "The matching algorithm used."},
	{ "matchtime", "Value of --matchtime."},
	{ "ok_matches_needed", "Value of --ok_matches_needed" },
	{ "lockout_method", "Value of --lockout_method." },
	{ "lockout_time", "Value of --lockout_time." },
	{ "lockout_error", "Value of --lockout_error." },
	{ "lockout_error_delay", "Value of --lockout_error_delay."},
	{ "match_success", "Match success status."},
	{ "match_desc", "Match description."},
	{ "match#_id", "Unique ID for match #." },
	{ "match#_path", "Image path for match #." },
	{ "match#_success", "Success status for match #." },
	{ "match#_direction", "Direction for match #." },
	{ "match#_description", "Description of match #." },
	{ "match#_result", "Result for match #." },
	{ "match#_time", "Time of match #." },
	{ "match#_step#_path", "Image path for match step # for match #."},
	{ "match#_step#_name", "Short name for match step # for match #."},
	{ "match#_step#_desc", "Description for match step # for match #."},
	{ "match#_step#_active", "If this match step was used for match #."},
	{ "match#_step_count", "The number of match steps for match #."},
	{ "time", "The current time when generating template." },
	{
		"time:<fmt>",
		"The time using the given format string"
		"using strftime formatting (replace % with @)."
		#ifdef _WIN32
		" Note that Windows only supports a subset of formatting characters."
		#endif // _WIN32
	},
	{ "git_hash", "The git commit hash for this build of catcierge."},
	{ "git_hash_short", "The short version of the git commit hash."},
	{ "git_tainted", "Was the git working tree changed when building."},
	{ "version", "The catcierge version." },
};

void catcierge_output_print_usage()
{
	size_t i;

	fprintf(stderr, "Output template variables:\n");

	for (i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
	{
		fprintf(stderr, "%20s   %s\n", vars[i].name, vars[i].description);
	}
}

int catcierge_output_init(catcierge_output_t *ctx)
{
	assert(ctx);
	memset(ctx, 0, sizeof(catcierge_output_t));
	ctx->template_max_count = 10;

	if (!(ctx->templates = calloc(ctx->template_max_count,
		sizeof(catcierge_output_template_t))))
	{
		CATERR("Out of memory\n");
		return -1;
	}

	return 0;
}

void catcierge_output_free_template(catcierge_output_template_t *t)
{
	if (!t)
		return;

	if (t->target_path) free(t->target_path);
	t->target_path = NULL;
	if (t->tmpl) free(t->tmpl);
	t->tmpl = NULL;
	if (t->name) free(t->name);
	t->name = NULL;
	if (t->generated_path) free(t->generated_path);
	t->generated_path = NULL;

	catcierge_free_list(t->settings.event_filter, t->settings.event_filter_count);
	t->settings.event_filter = NULL;
	t->settings.event_filter_count = 0;
}

void catcierge_output_destroy(catcierge_output_t *ctx)
{
	catcierge_output_template_t *t;
	assert(ctx);

	if (ctx->templates)
	{
		size_t i;

		for (i = 0; i < ctx->template_count; i++)
		{
			t = &ctx->templates[i];
			catcierge_output_free_template(t);
		}

		free(ctx->templates);
		ctx->templates = NULL;
	}

	ctx->template_count = 0;
	ctx->template_max_count = 0;
}

int catcierge_output_read_event_setting(catcierge_output_settings_t *settings, const char *events)
{
	assert(settings);

	if (!(settings->event_filter = catcierge_parse_list(events, &settings->event_filter_count, 1)))
	{
		return -1;
	}

	return 0;
}

const char *catcierge_output_read_template_settings(const char *name,
	catcierge_output_settings_t *settings, const char *template_str)
{
	const char *s = NULL;
	char *it = NULL;
	char *row_start = NULL;
	char *row_end = NULL;
	char *end = NULL;
	char *tmp = NULL;
	size_t bytes_read;
	assert(template_str);

	if (!(tmp = strdup(template_str)))
	{
		CATERR("Out of memory!\n");
		return NULL;
	}

	it = tmp;
	row_start = it;
	row_end = it;
	end = it + strlen(it);

	// Consume all the settings in the file.
	while (it < end)
	{
		if (it == row_end)
		{
			// On the first row_end and row_start
			// will be the same. Otherwise we have
			// reached the replaced \n, so advance
			// past it.
			if (it != row_start)
				it++;

			it = catcierge_skip_whitespace_alt(it);
			row_start = it;

			if ((row_end = strchr(it, '\n')))
			{
				*row_end = '\0';
			}
			else
			{
				break;
			}
		}

		// Break as soon as we find a row without a setting.
		if (strncmp(it, "%!", 2))
		{
			break;
		}

		it += 2;
		it = catcierge_skip_whitespace_alt(it);

		if (!strncmp(it, "event", 5))
		{
			it += 5;
			it = catcierge_skip_whitespace_alt(it);

			if (catcierge_output_read_event_setting(settings, it))
			{
				CATERR("Failed to parse event setting\n");
				free(tmp);
				return NULL;
			}
			it = row_end;
			continue;
		}
		else if (!strncmp(it, "nop", 3))
		{
			// So we can test the logic for 2 settings for now.
			it += 3;
			it = catcierge_skip_whitespace_alt(it);
			continue;
		}
		else
		{
			const char *unknown_end = strchr(it, '\n');
			int line_len = unknown_end ? (int)(unknown_end - it) : strlen(it);
			CATERR("Unknown template setting: \"%*s\"\n", line_len, it);
			it += line_len;
			free(tmp);
			return NULL;
		}
	}

	bytes_read = it - tmp;
	free(tmp);

	if (settings->event_filter_count == 0)
	{
		CATERR("!!! Output template \"%s\" missing event filter, nothing will be generated !!!\n", name);
	}

	return template_str + bytes_read;
}

int catcierge_output_add_template(catcierge_output_t *ctx,
		const char *template_str, const char *target_path)
{
	const char *path;
	catcierge_output_template_t *t;
	assert(ctx);

	// Get only the filename.
	if ((path = strrchr(target_path, catcierge_path_sep()[0])))
	{
		target_path = path + 1;
	}

	// Grow the templates array if needed.
	if (ctx->template_max_count < (ctx->template_count + 1))
	{
		ctx->template_max_count *= 2;

		if (!(ctx->templates = realloc(ctx->templates,
			ctx->template_max_count * sizeof(catcierge_output_template_t))))
		{
			CATERR("Out of memory!\n"); return -1;
		}
	}

	t = &ctx->templates[ctx->template_count];
	memset(t, 0, sizeof(*t));

	// If the target template filename starts with
	// [name]bla_bla_%stuff%.json
	// The template will get the "name" inside the [].
	// Otherwise, simply use the template index as the name.
	// This is so that we can pass the path of the generated
	// target path at run time to the catcierge_execute function
	// and the external program can distinguish between multiple templates.
	{
		char name[4096];
		memset(name, 0, sizeof(name));

		if (sscanf(target_path, "[%[^]]", name) == 1)
		{
			size_t name_len = strlen(name);

			if (name_len >= sizeof(name))
			{
				CATERR("Template name \"%s\" is too long, max %d characters allowed\n",
					name, sizeof(name));
				goto fail;
			}

			target_path += 1 + name_len + 1; // Name + the []
		}
		else
		{
			snprintf(name, sizeof(name) - 1, "%d", (int)ctx->template_count);
		}
		
		if (!(t->name = strdup(name)))
		{
			goto out_of_memory;
		}
	}

	if (!(t->target_path = strdup(target_path)))
	{
		goto out_of_memory;
	}

	if (!(template_str = catcierge_output_read_template_settings(t->target_path,
							&t->settings, template_str)))
	{
		goto fail;
	}

	if (!(t->tmpl = strdup(template_str)))
	{
		goto out_of_memory;
	}

	ctx->template_count++;

	CATLOG(" %s (%s)\n", t->name, t->target_path);

	return 0;

out_of_memory:
	CATERR("Out of memory\n");
fail:
	catcierge_output_free_template(t);

	return -1;
}

static char *catcierge_replace_time_format_char(char *fmt)
{
	char *s;
	s = fmt;

	while (*s)
	{
		if (*s == '@')
			*s = '%';
		s++;
	}

	return fmt;
}

static char *catcierge_get_time_var_format(char *var,
	char *buf, size_t bufsize, const char *default_fmt, time_t t, struct timeval *tv)
{
	int ret;
	char *fmt = NULL;
	char *var_fmt = var + 4;
	assert(!strncmp(var, "time", 4));

	if (*var_fmt == ':')
	{
		char *s = NULL;
		var_fmt++;

		if (!(fmt = strdup(var_fmt)))
		{
			CATERR("Out of memory!\n"); return NULL;
		}

		catcierge_replace_time_format_char(fmt);
	}
	else
	{
		if (!(fmt = strdup(default_fmt)))
		{
			CATERR("Out of memory!\n"); return NULL;
		}
	}

	ret = catcierge_strftime(buf, bufsize - 1, fmt, localtime(&t), tv);

	if (!ret)
	{
		CATERR("Invalid time formatting string \"%s\"\n", fmt);
		buf = NULL;
	}

	if (fmt)
	{
		free(fmt);
	}

	return buf;
}

static char *catcierge_get_template_path(catcierge_grb_t *grb, const char *var)
{
	size_t i;
	catcierge_output_t *o = &grb->output;
	const char *subvar = var + strlen("template_path");

	if (*subvar == ':')
	{
		subvar++;

		// Find the template with the given name.
		for (i = 0; i < o->template_count; i++)
		{
			if (!strcmp(subvar, o->templates[i].name))
			{
				return o->templates[i].generated_path;
			}
		}
	}
	else
	{
		// If no template name is given, simply use the first one.
		// (This will probably be the most common case).
		if (o->template_count > 0)
		{
			return o->templates[0].generated_path;
		}
	}

	return NULL;
}

const char *catcierge_output_translate(catcierge_grb_t *grb,
	char *buf, size_t bufsize, char *var)
{
	if (!strncmp(var, "template_path", 13))
	{
		return catcierge_get_template_path(grb, var);
	}

	// Current time.
	if (!strncmp(var, "time", 4))
	{
		struct timeval tv;
		gettimeofday(&tv, NULL);
		return catcierge_get_time_var_format(var, buf, bufsize,
			"%Y-%m-%d %H:%M:%S.%f", time(NULL), &tv);
	}

	if (!strcmp(var, "state"))
	{
		return catcierge_get_state_string(grb->state);
	}

	if (!strcmp(var, "prev_state"))
	{
		return catcierge_get_state_string(grb->prev_state);
	}

	if (!strcmp(var, "git_commit") || !strcmp(var, "git_hash"))
	{
		return CATCIERGE_GIT_HASH;
	}

	if (!strcmp(var, "git_commit_short") || !strcmp(var, "git_hash_short"))
	{
		return CATCIERGE_GIT_HASH_SHORT;
	}

	if (!strcmp(var, "git_tainted"))
	{
		snprintf(buf, bufsize - 1, "%d", CATCIERGE_GIT_TAINTED);
		return buf;
	}

	if (!strcmp(var, "version"))
	{
		return CATCIERGE_VERSION_STR;
	}

	if (!strcmp(var, "matcher"))
	{
		return grb->args.matcher;
	}

	if (!strcmp(var, "ok_matches_needed"))
	{
		snprintf(buf, bufsize - 1, "%d", grb->args.ok_matches_needed);
		return buf;
	}

	if (!strcmp(var, "matchtime"))
	{
		snprintf(buf, bufsize - 1, "%d", grb->args.match_time);
		return buf;
	}

	if (!strcmp(var, "lockout_method"))
	{
		snprintf(buf, bufsize - 1, "%d", (int)grb->args.lockout_method);
		return buf;
	}

	if (!strcmp(var, "lockout_error"))
	{
		snprintf(buf, bufsize - 1, "%d", grb->args.max_consecutive_lockout_count);
		return buf;
	}

	if (!strcmp(var, "lockout_error_delay"))
	{
		snprintf(buf, bufsize - 1, "%0.2f", grb->args.consecutive_lockout_delay);
		return buf;
	}

	if (!strcmp(var, "lockout_time"))
	{
		snprintf(buf, bufsize - 1, "%d", grb->args.lockout_time);
		return buf;
	}

	if (!strcmp(var, "match_success"))
	{
		snprintf(buf, bufsize - 1, "%d", grb->match_group.success);
		return buf;
	}

	if (!strcmp(var, "match_count"))
	{
		snprintf(buf, bufsize - 1, "%d", (int)grb->match_group.match_count);
		return buf;
	}

	if (!strncmp(var, "match", 5))
	{
		int idx = -1;
		match_state_t *m = NULL;
		char *subvar = NULL;

		if (!strncmp(var, "matchcur", 8))
		{
			idx = grb->match_group.match_count - 1;
			subvar = var + strlen("matchcur_");
		}
		else
		{
			subvar = var + strlen("match#_");

			if (sscanf(var, "match%d_", &idx) == EOF)
			{
				return NULL;
			}

			idx--; // Convert to 0-based index.
		}

		if ((idx < 0) || (idx >= MATCH_MAX_COUNT))
		{
			return NULL;
		}

		m = &grb->match_group.matches[idx];

		if (idx >= grb->match_group.match_count)
		{
			return "";
		}

		if (!strcmp(subvar, "path"))
		{
			return m->path;
		}
		else if (!strcmp(subvar, "id"))
		{
			snprintf(buf, bufsize - 1, "%x%x%x%x%x",
				m->sha.Message_Digest[0],
				m->sha.Message_Digest[1],
				m->sha.Message_Digest[2],
				m->sha.Message_Digest[3],
				m->sha.Message_Digest[4]);
			return buf;
		}
		else if (!strcmp(subvar, "success"))
		{
			snprintf(buf, bufsize - 1, "%d", m->result.success);
			return buf;
		}
		else if (!strcmp(subvar, "direction"))
		{
			return catcierge_get_direction_str(m->result.direction);
		}
		else if (!strncmp(subvar, "desc", 4))
		{
			return m->result.description;
		}
		else if (!strcmp(subvar, "result"))
		{
			snprintf(buf, bufsize - 1, "%f", m->result.result);
			return buf;
		}
		else if (!strncmp(subvar, "time", 4))
		{
			return catcierge_get_time_var_format(subvar, buf, bufsize,
					"%Y-%m-%d %H:%M:%S.%f", m->time, &m->tv);
		}
		else if (!strcmp(subvar, "step_count"))
		{
			snprintf(buf, bufsize - 1, "%d", (int)m->result.step_img_count);
			return buf;
		}
		else if (!strncmp(subvar, "step", 4))
		{
			// Match step images / descriptions.
			int stepidx = -1;
			match_step_t *step = NULL;

			const char *stepvar = subvar + strlen("step#_");

			if (sscanf(subvar, "step%d_", &stepidx) == EOF)
			{
				CATERR("Failed to parse step#_\n");
				return NULL;
			}

			// "step##_" instead of just "step#_"
			if (stepidx >= 10)
				stepvar++;

			stepidx--; // Convert to 0-based index.

			if ((stepidx < 0) || (stepidx >= MAX_STEPS))
			{
				CATERR("Step index out of range %d\n", stepidx);
				return NULL;
			}

			step = &m->result.steps[stepidx];

			if (!strcmp(stepvar, "path"))
			{
				return step->path ? step->path : "";
			}
			else if (!strcmp(stepvar, "name"))
			{
				return step->name ? step->name : "";
			}
			else if (!strncmp(stepvar, "desc", 4))
			{
				return step->description ? step->description : "";
			}
			else if (!strcmp(stepvar, "active"))
			{
				snprintf(buf, bufsize - 1, "%d", step->img != NULL);
				return buf;
			}
		}
	}

	return NULL;
}

static char *catcierge_output_generate_ex(catcierge_output_t *ctx,
	catcierge_grb_t *grb, const char *template_str)
{
	char buf[4096];
	char *s;
	char *it;
	char *output = NULL;
	char *tmp = NULL;
	size_t orig_len = strlen(template_str);
	size_t out_len = 2 * orig_len + 1;
	size_t len;
	size_t linenum;
	assert(ctx);
	assert(grb);

	if (!(output = malloc(out_len)))
	{
		return NULL;
	}

	if (!(tmp = strdup(template_str)))
	{
		free(output);
		return NULL;
	}

	len = 0;
	linenum = 0;
	it = tmp;

	while (*it)
	{
		if (*it == '\n')
		{
			linenum++;
		}

		// Replace any variables signified by %varname%
		if (*it == '%')
		{
			const char *res;
			it++;

			// %% means a literal %
			if (*it && (*it == '%'))
			{
				output[len] = *it++;
				len++;
				continue;
			}

			// Save position of beginning of var name.
			s = it;

			// Look for the ending %
			while (*it && (*it != '%') && (*it != '\n'))
			{
				it++;
			}

			// Either we found it or the end of string.
			if (*it != '%')
			{
				*it = '\0';
				CATERR("Variable \"%s\" not terminated in output template line %d\n",
					s, (int)linenum);
				free(output);
				output = NULL;
				goto fail;
			}

			// Terminate so we get the var name in a nice comparable string.
			*it++ = '\0';

			// Find the value of the variable and append it to the output.
			if ((res = catcierge_output_translate(grb, buf, sizeof(buf), s)))
			{
				size_t reslen = strlen(res);

				// Make sure we have enough room.
				while ((len + reslen + 1) >= out_len)
				{
					out_len *= 2;

					if (!(output = realloc(output, out_len)))
					{
						CATERR("Out of memory\n"); goto fail;
					}
				}

				// Append ...
				while (*res)
				{
					output[len] = *res++;
					len++;
				}
			}
			else
			{
				CATERR("Unknown template variable \"%s\"\n", s);
				free(output);
				output = NULL;
				goto fail;
			}
		}
		else
		{
			output[len] = *it++;
			len++;
		}
	}

	output[len] = '\0';

fail:
	if (tmp)
		free(tmp);

	return output;
}

char *catcierge_output_generate(catcierge_output_t *ctx, catcierge_grb_t *grb,
		const char *template_str)
{
	return catcierge_output_generate_ex(ctx, grb, template_str);
}

int catcierge_output_validate(catcierge_output_t *ctx,
	catcierge_grb_t *grb, const char *template_str)
{
	int is_valid = 0;
	char *output = catcierge_output_generate_ex(ctx, grb, template_str);
	is_valid = (output != NULL);

	if (output)
		free(output);

	return is_valid;
}

static char *catcierge_replace_whitespace(char *path, char *extra_chars)
{
	char *p = path;
	char *e = NULL;

	while (*p)
	{
		if ((*p == ' ') || (*p == '\t') || (*p == '\n'))
		{
			*p = '_';
		}

		if (extra_chars)
		{
			e = extra_chars;
			while (*e)
			{
				if (*p == *e)
				{
					*p = '_';
				}

				e++;
			}
		}

		p++;
	}

	return 0;
}

static void catcierge_output_free_generated_paths(catcierge_output_t *ctx)
{
	size_t i;
	catcierge_output_template_t *t = NULL;
	assert(ctx);

	for (i = 0; i < ctx->template_count; i++)
	{
		t = &ctx->templates[i];

		if (t->generated_path)
		{
			free(t->generated_path);
			t->generated_path = NULL;
		}
	}
}

int catcierge_output_template_registered_to_event(catcierge_output_template_t *t, const char *event)
{
	size_t i;
	assert(t);
	assert(event);

	for (i = 0; i < t->settings.event_filter_count; i++)
	{
		if (!strcmp(t->settings.event_filter[i], "all")
		 || !strcmp(t->settings.event_filter[i], "*"))
		{
			return 1;
		}

		if (!strcmp(t->settings.event_filter[i], event))
		{
			return 1;
		}
	}

	return 0;
}

int catcierge_output_generate_templates(catcierge_output_t *ctx,
	catcierge_grb_t *grb, const char *output_path, const char *event)
{
	catcierge_output_template_t *t = NULL;
	char *output = NULL;
	char *path = NULL;
	char full_path[4096];
	char *dir = NULL;
	size_t i;
	FILE *f = NULL;
	assert(ctx);
	assert(grb);

	if (output_path)
	{
		catcierge_make_path(output_path);
	}
	else
	{
		output_path = ".";
	}

	catcierge_output_free_generated_paths(ctx);

	for (i = 0; i < ctx->template_count; i++)
	{
		t = &ctx->templates[i];

		// Filter out any events that don't have the current "event" in their list.
		if (!catcierge_output_template_registered_to_event(t, event))
		{
			continue;
		}

		// First generate the target path.
		{
			if (!(path = catcierge_output_generate_ex(ctx, grb, t->target_path)))
			{
				CATERR("Failed to generate output path for template \"%s\"\n", t->target_path);
				if (output) free(output);
				return -1;
			}

			// Replace whitespace with underscore.
			catcierge_replace_whitespace(path, ":");

			// Assemble the full output path.
			snprintf(full_path, sizeof(full_path), "%s%s%s",
				output_path, catcierge_path_sep(), path);

			// We make a copy so that we can use the generated
			// path as a variable in the templates contents, or
			// when passed to catcierge_execute. 
			if (!(t->generated_path = strdup(full_path)))
			{
				CATERR("Out of memory!\n");
				free(path);
				return -1;
			}
		}

		// And then generate the template contents.
		if (!(output = catcierge_output_generate_ex(ctx, grb, t->tmpl)))
		{
			CATERR("Failed to generate output for template \"%s\"\n", t->target_path);
			free(path);
			return -1;
		}

		if (!(f = fopen(full_path, "w")))
		{
			CATERR("Failed to open template output file \"%s\" for writing\n", full_path);
			free(output);
			free(path);
			return -1;
		}
		else
		{
			size_t len = strlen(output);
			size_t written = fwrite(output, sizeof(char), len, f);
			fclose(f);
		}

		if (output)
		{
			free(output);
			output = NULL;
		}

		if (path)
		{
			free(path);
			path = NULL;
		}
	}

	return 0;
}

int catcierge_output_load_template(catcierge_output_t *ctx, char *path)
{
	int ret = 0;
	size_t fsize;
	size_t read_bytes;
	FILE *f = NULL;
	char *contents = NULL;
	char *settings_end = NULL;
	assert(path);
	assert(ctx);

	if (!(f = fopen(path, "r")))
	{
		CATERR("Failed to open input template file \"%s\"\n", path);
		ret = -1; goto fail;
	}

	// Get file size.
	if (fseek(f, 0, SEEK_END))
	{
		CATERR("Failed to seek in template file \"%s\"\n", path);
		ret = -1; goto fail;
	}

	if ((fsize = ftell(f)) == -1)
	{
		CATERR("Failed to get file size for template file \"%s\"\n", path);
		ret = -1; goto fail;
	}

	rewind(f);

	// Make sure we allocate enough to fit a NULL
	// character at the end of the file contents.
	if (!(contents = calloc(1, fsize + 1)))
	{
		CATERR("Out of memory!\n");
		ret = -1; goto fail;
	}

	read_bytes = fread(contents, 1, fsize, f);
	contents[read_bytes] = '\0';

	#ifndef _WIN32
	if (read_bytes != fsize)
	{
		CATERR("Failed to read file contents of template file \"%s\". "
				"Got %d expected %d\n", path, (int)read_bytes, (int)fsize);
		ret = -1; goto fail;
	}
	#endif // !_WIN32

	if (catcierge_output_add_template(ctx, contents, path))
	{
		CATERR("Failed to load template file \"%s\"\n", path);
		ret = -1; goto fail;
	}

fail:
	if (contents)
	{
		free(contents);
		contents = NULL;
	}

	if (f)
	{
		fclose(f);
	}

	return ret;
}

int catcierge_output_load_templates(catcierge_output_t *ctx,
		char **inputs, size_t input_count)
{
	int ret = 0;
	size_t i;

	if (input_count > 0)
	{
		CATLOG("Loading output templates:\n");
	}

	for (i = 0; i < input_count; i++)
	{
		if (catcierge_output_load_template(ctx, inputs[i]))
		{
			return -1;
		}
	}

	return ret;
}

void catcierge_output_execute(catcierge_grb_t *grb,
		const char *event, const char *command)
{
	char *generated_cmd = NULL;

	if (!command)
		return;

	if (catcierge_output_generate_templates(&grb->output,
		grb, grb->args.output_path, event))
	{
		CATERR("Failed to generate templates on execute!\n");
		return;
	}

	if (!(generated_cmd = catcierge_output_generate(&grb->output, grb, command)))
	{
		CATERR("Failed to execute command \"%s\"!\n", command);
		return;
	}

	catcierge_run(generated_cmd);

	free(generated_cmd);
}


