// SPDX-License-Identifier: GPL-2.0
/*
 * trace_events_hist - trace event hist triggers
 *
 * Copyright (C) 2015 Tom Zanussi <tom.zanussi@linux.intel.com>
 */

#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/rculist.h>
#include <linux/tracefs.h>

#include "tracing_map.h"
#include "trace.h"

#define SYNTH_SYSTEM		"synthetic"
#define SYNTH_FIELDS_MAX	16

#define STR_VAR_LEN_MAX		32 /* must be multiple of sizeof(u64) */

struct hist_field;

typedef u64 (*hist_field_fn_t) (struct hist_field *field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event);

#define HIST_FIELD_OPERANDS_MAX	2
#define HIST_FIELDS_MAX		(TRACING_MAP_FIELDS_MAX + TRACING_MAP_VARS_MAX)
#define HIST_ACTIONS_MAX	8

enum field_op_id {
	FIELD_OP_NONE,
	FIELD_OP_PLUS,
	FIELD_OP_MINUS,
	FIELD_OP_UNARY_MINUS,
};

struct hist_var {
	char				*name;
	struct hist_trigger_data	*hist_data;
	unsigned int			idx;
};

struct hist_field {
	struct ftrace_event_field	*field;
	unsigned long			flags;
	hist_field_fn_t			fn;
	unsigned int			size;
	unsigned int			offset;
	unsigned int                    is_signed;
	const char			*type;
	struct hist_field		*operands[HIST_FIELD_OPERANDS_MAX];
	struct hist_trigger_data	*hist_data;
	struct hist_var			var;
	enum field_op_id		operator;
	char				*system;
	char				*event_name;
	char				*name;
	unsigned int			var_idx;
	unsigned int			var_ref_idx;
	bool                            read_once;
};

static u64 hist_field_none(struct hist_field *field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	return 0;
}

static u64 hist_field_counter(struct hist_field *field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	return 1;
}

static u64 hist_field_string(struct hist_field *hist_field,
			     struct tracing_map_elt *elt,
			     struct ring_buffer_event *rbe,
			     void *event)
{
	char *addr = (char *)(event + hist_field->field->offset);

	return (u64)(unsigned long)addr;
}

static u64 hist_field_dynstring(struct hist_field *hist_field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event)
{
	u32 str_item = *(u32 *)(event + hist_field->field->offset);
	int str_loc = str_item & 0xffff;
	char *addr = (char *)(event + str_loc);

	return (u64)(unsigned long)addr;
}

static u64 hist_field_pstring(struct hist_field *hist_field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	char **addr = (char **)(event + hist_field->field->offset);

	return (u64)(unsigned long)*addr;
}

static u64 hist_field_log2(struct hist_field *hist_field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	struct hist_field *operand = hist_field->operands[0];

	u64 val = operand->fn(operand, elt, rbe, event);

	return (u64) ilog2(roundup_pow_of_two(val));
}

static u64 hist_field_plus(struct hist_field *hist_field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	struct hist_field *operand1 = hist_field->operands[0];
	struct hist_field *operand2 = hist_field->operands[1];

	u64 val1 = operand1->fn(operand1, elt, rbe, event);
	u64 val2 = operand2->fn(operand2, elt, rbe, event);

	return val1 + val2;
}

static u64 hist_field_minus(struct hist_field *hist_field,
			    struct tracing_map_elt *elt,
			    struct ring_buffer_event *rbe,
			    void *event)
{
	struct hist_field *operand1 = hist_field->operands[0];
	struct hist_field *operand2 = hist_field->operands[1];

	u64 val1 = operand1->fn(operand1, elt, rbe, event);
	u64 val2 = operand2->fn(operand2, elt, rbe, event);

	return val1 - val2;
}

static u64 hist_field_unary_minus(struct hist_field *hist_field,
				  struct tracing_map_elt *elt,
				  struct ring_buffer_event *rbe,
				  void *event)
{
	struct hist_field *operand = hist_field->operands[0];

	s64 sval = (s64)operand->fn(operand, elt, rbe, event);
	u64 val = (u64)-sval;

	return val;
}

#define DEFINE_HIST_FIELD_FN(type)					\
	static u64 hist_field_##type(struct hist_field *hist_field,	\
				     struct tracing_map_elt *elt,	\
				     struct ring_buffer_event *rbe,	\
				     void *event)			\
{									\
	type *addr = (type *)(event + hist_field->field->offset);	\
									\
	return (u64)(unsigned long)*addr;				\
}

DEFINE_HIST_FIELD_FN(s64);
DEFINE_HIST_FIELD_FN(u64);
DEFINE_HIST_FIELD_FN(s32);
DEFINE_HIST_FIELD_FN(u32);
DEFINE_HIST_FIELD_FN(s16);
DEFINE_HIST_FIELD_FN(u16);
DEFINE_HIST_FIELD_FN(s8);
DEFINE_HIST_FIELD_FN(u8);

#define for_each_hist_field(i, hist_data)	\
	for ((i) = 0; (i) < (hist_data)->n_fields; (i)++)

#define for_each_hist_val_field(i, hist_data)	\
	for ((i) = 0; (i) < (hist_data)->n_vals; (i)++)

#define for_each_hist_key_field(i, hist_data)	\
	for ((i) = (hist_data)->n_vals; (i) < (hist_data)->n_fields; (i)++)

#define HIST_STACKTRACE_DEPTH	16
#define HIST_STACKTRACE_SIZE	(HIST_STACKTRACE_DEPTH * sizeof(unsigned long))
#define HIST_STACKTRACE_SKIP	5

#define HITCOUNT_IDX		0
#define HIST_KEY_SIZE_MAX	(MAX_FILTER_STR_VAL + HIST_STACKTRACE_SIZE)

enum hist_field_flags {
	HIST_FIELD_FL_HITCOUNT		= 1 << 0,
	HIST_FIELD_FL_KEY		= 1 << 1,
	HIST_FIELD_FL_STRING		= 1 << 2,
	HIST_FIELD_FL_HEX		= 1 << 3,
	HIST_FIELD_FL_SYM		= 1 << 4,
	HIST_FIELD_FL_SYM_OFFSET	= 1 << 5,
	HIST_FIELD_FL_EXECNAME		= 1 << 6,
	HIST_FIELD_FL_SYSCALL		= 1 << 7,
	HIST_FIELD_FL_STACKTRACE	= 1 << 8,
	HIST_FIELD_FL_LOG2		= 1 << 9,
	HIST_FIELD_FL_TIMESTAMP		= 1 << 10,
	HIST_FIELD_FL_TIMESTAMP_USECS	= 1 << 11,
	HIST_FIELD_FL_VAR		= 1 << 12,
	HIST_FIELD_FL_EXPR		= 1 << 13,
	HIST_FIELD_FL_VAR_REF		= 1 << 14,
	HIST_FIELD_FL_CPU		= 1 << 15,
	HIST_FIELD_FL_ALIAS		= 1 << 16,
};

struct var_defs {
	unsigned int	n_vars;
	char		*name[TRACING_MAP_VARS_MAX];
	char		*expr[TRACING_MAP_VARS_MAX];
};

struct hist_trigger_attrs {
	char		*keys_str;
	char		*vals_str;
	char		*sort_key_str;
	char		*name;
	char		*clock;
	bool		pause;
	bool		cont;
	bool		clear;
	bool		ts_in_usecs;
	unsigned int	map_bits;

	char		*assignment_str[TRACING_MAP_VARS_MAX];
	unsigned int	n_assignments;

	char		*action_str[HIST_ACTIONS_MAX];
	unsigned int	n_actions;

	struct var_defs	var_defs;
};

struct field_var {
	struct hist_field	*var;
	struct hist_field	*val;
};

struct field_var_hist {
	struct hist_trigger_data	*hist_data;
	char				*cmd;
};

struct hist_trigger_data {
	struct hist_field               *fields[HIST_FIELDS_MAX];
	unsigned int			n_vals;
	unsigned int			n_keys;
	unsigned int			n_fields;
	unsigned int			n_vars;
	unsigned int			key_size;
	struct tracing_map_sort_key	sort_keys[TRACING_MAP_SORT_KEYS_MAX];
	unsigned int			n_sort_keys;
	struct trace_event_file		*event_file;
	struct hist_trigger_attrs	*attrs;
	struct tracing_map		*map;
	bool				enable_timestamps;
	bool				remove;
	struct hist_field               *var_refs[TRACING_MAP_VARS_MAX];
	unsigned int			n_var_refs;

	struct action_data		*actions[HIST_ACTIONS_MAX];
	unsigned int			n_actions;

	struct hist_field               *synth_var_refs[SYNTH_FIELDS_MAX];
	unsigned int                    n_synth_var_refs;
	struct field_var		*field_vars[SYNTH_FIELDS_MAX];
	unsigned int			n_field_vars;
	unsigned int			n_field_var_str;
	struct field_var_hist		*field_var_hists[SYNTH_FIELDS_MAX];
	unsigned int			n_field_var_hists;

	struct field_var		*max_vars[SYNTH_FIELDS_MAX];
	unsigned int			n_max_vars;
	unsigned int			n_max_var_str;
};

struct synth_field {
	char *type;
	char *name;
	size_t size;
	bool is_signed;
	bool is_string;
};

struct synth_event {
	struct list_head			list;
	int					ref;
	char					*name;
	struct synth_field			**fields;
	unsigned int				n_fields;
	unsigned int				n_u64;
	struct trace_event_class		class;
	struct trace_event_call			call;
	struct tracepoint			*tp;
};

struct action_data;

typedef void (*action_fn_t) (struct hist_trigger_data *hist_data,
			     struct tracing_map_elt *elt, void *rec,
			     struct ring_buffer_event *rbe,
			     struct action_data *data, u64 *var_ref_vals);

struct action_data {
	action_fn_t		fn;
	unsigned int		n_params;
	char			*params[SYNTH_FIELDS_MAX];

	union {
		struct {
			unsigned int		var_ref_idx;
			char			*match_event;
			char			*match_event_system;
			char			*synth_event_name;
			struct synth_event	*synth_event;
		} onmatch;

		struct {
			char			*var_str;
			char			*fn_name;
			unsigned int		max_var_ref_idx;
			struct hist_field	*max_var;
			struct hist_field	*var;
		} onmax;
	};
};


static char last_hist_cmd[MAX_FILTER_STR_VAL];
static char hist_err_str[MAX_FILTER_STR_VAL];

static void last_cmd_set(char *str)
{
	if (!str)
		return;

	strncpy(last_hist_cmd, str, MAX_FILTER_STR_VAL - 1);
}

static void hist_err(char *str, char *var)
{
	int maxlen = MAX_FILTER_STR_VAL - 1;

	if (!str)
		return;

	if (strlen(hist_err_str))
		return;

	if (!var)
		var = "";

	if (strlen(hist_err_str) + strlen(str) + strlen(var) > maxlen)
		return;

	strcat(hist_err_str, str);
	strcat(hist_err_str, var);
}

static void hist_err_event(char *str, char *system, char *event, char *var)
{
	char err[MAX_FILTER_STR_VAL];

	if (system && var)
		snprintf(err, MAX_FILTER_STR_VAL, "%s.%s.%s", system, event, var);
	else if (system)
		snprintf(err, MAX_FILTER_STR_VAL, "%s.%s", system, event);
	else
		strscpy(err, var, MAX_FILTER_STR_VAL);

	hist_err(str, err);
}

static void hist_err_clear(void)
{
	hist_err_str[0] = '\0';
}

static bool have_hist_err(void)
{
	if (strlen(hist_err_str))
		return true;

	return false;
}

static LIST_HEAD(synth_event_list);
static DEFINE_MUTEX(synth_event_mutex);

struct synth_trace_event {
	struct trace_entry	ent;
	u64			fields[];
};

static int synth_event_define_fields(struct trace_event_call *call)
{
	struct synth_trace_event trace;
	int offset = offsetof(typeof(trace), fields);
	struct synth_event *event = call->data;
	unsigned int i, size, n_u64;
	char *name, *type;
	bool is_signed;
	int ret = 0;

	for (i = 0, n_u64 = 0; i < event->n_fields; i++) {
		size = event->fields[i]->size;
		is_signed = event->fields[i]->is_signed;
		type = event->fields[i]->type;
		name = event->fields[i]->name;
		ret = trace_define_field(call, type, name, offset, size,
					 is_signed, FILTER_OTHER);
		if (ret)
			break;

		if (event->fields[i]->is_string) {
			offset += STR_VAR_LEN_MAX;
			n_u64 += STR_VAR_LEN_MAX / sizeof(u64);
		} else {
			offset += sizeof(u64);
			n_u64++;
		}
	}

	event->n_u64 = n_u64;

	return ret;
}

static bool synth_field_signed(char *type)
{
	if (strncmp(type, "u", 1) == 0)
		return false;
	if (strcmp(type, "gfp_t") == 0)
		return false;

	return true;
}

static int synth_field_is_string(char *type)
{
	if (strstr(type, "char[") != NULL)
		return true;

	return false;
}

static int synth_field_string_size(char *type)
{
	char buf[4], *end, *start;
	unsigned int len;
	int size, err;

	start = strstr(type, "char[");
	if (start == NULL)
		return -EINVAL;
	start += strlen("char[");

	end = strchr(type, ']');
	if (!end || end < start)
		return -EINVAL;

	len = end - start;
	if (len > 3)
		return -EINVAL;

	strncpy(buf, start, len);
	buf[len] = '\0';

	err = kstrtouint(buf, 0, &size);
	if (err)
		return err;

	if (size > STR_VAR_LEN_MAX)
		return -EINVAL;

	return size;
}

static int synth_field_size(char *type)
{
	int size = 0;

	if (strcmp(type, "s64") == 0)
		size = sizeof(s64);
	else if (strcmp(type, "u64") == 0)
		size = sizeof(u64);
	else if (strcmp(type, "s32") == 0)
		size = sizeof(s32);
	else if (strcmp(type, "u32") == 0)
		size = sizeof(u32);
	else if (strcmp(type, "s16") == 0)
		size = sizeof(s16);
	else if (strcmp(type, "u16") == 0)
		size = sizeof(u16);
	else if (strcmp(type, "s8") == 0)
		size = sizeof(s8);
	else if (strcmp(type, "u8") == 0)
		size = sizeof(u8);
	else if (strcmp(type, "char") == 0)
		size = sizeof(char);
	else if (strcmp(type, "unsigned char") == 0)
		size = sizeof(unsigned char);
	else if (strcmp(type, "int") == 0)
		size = sizeof(int);
	else if (strcmp(type, "unsigned int") == 0)
		size = sizeof(unsigned int);
	else if (strcmp(type, "long") == 0)
		size = sizeof(long);
	else if (strcmp(type, "unsigned long") == 0)
		size = sizeof(unsigned long);
	else if (strcmp(type, "pid_t") == 0)
		size = sizeof(pid_t);
	else if (synth_field_is_string(type))
		size = synth_field_string_size(type);

	return size;
}

static const char *synth_field_fmt(char *type)
{
	const char *fmt = "%llu";

	if (strcmp(type, "s64") == 0)
		fmt = "%lld";
	else if (strcmp(type, "u64") == 0)
		fmt = "%llu";
	else if (strcmp(type, "s32") == 0)
		fmt = "%d";
	else if (strcmp(type, "u32") == 0)
		fmt = "%u";
	else if (strcmp(type, "s16") == 0)
		fmt = "%d";
	else if (strcmp(type, "u16") == 0)
		fmt = "%u";
	else if (strcmp(type, "s8") == 0)
		fmt = "%d";
	else if (strcmp(type, "u8") == 0)
		fmt = "%u";
	else if (strcmp(type, "char") == 0)
		fmt = "%d";
	else if (strcmp(type, "unsigned char") == 0)
		fmt = "%u";
	else if (strcmp(type, "int") == 0)
		fmt = "%d";
	else if (strcmp(type, "unsigned int") == 0)
		fmt = "%u";
	else if (strcmp(type, "long") == 0)
		fmt = "%ld";
	else if (strcmp(type, "unsigned long") == 0)
		fmt = "%lu";
	else if (strcmp(type, "pid_t") == 0)
		fmt = "%d";
	else if (synth_field_is_string(type))
		fmt = "%s";

	return fmt;
}

static enum print_line_t print_synth_event(struct trace_iterator *iter,
					   int flags,
					   struct trace_event *event)
{
	struct trace_array *tr = iter->tr;
	struct trace_seq *s = &iter->seq;
	struct synth_trace_event *entry;
	struct synth_event *se;
	unsigned int i, n_u64;
	char print_fmt[32];
	const char *fmt;

	entry = (struct synth_trace_event *)iter->ent;
	se = container_of(event, struct synth_event, call.event);

	trace_seq_printf(s, "%s: ", se->name);

	for (i = 0, n_u64 = 0; i < se->n_fields; i++) {
		if (trace_seq_has_overflowed(s))
			goto end;

		fmt = synth_field_fmt(se->fields[i]->type);

		/* parameter types */
		if (tr->trace_flags & TRACE_ITER_VERBOSE)
			trace_seq_printf(s, "%s ", fmt);

		snprintf(print_fmt, sizeof(print_fmt), "%%s=%s%%s", fmt);

		/* parameter values */
		if (se->fields[i]->is_string) {
			trace_seq_printf(s, print_fmt, se->fields[i]->name,
					 (char *)&entry->fields[n_u64],
					 i == se->n_fields - 1 ? "" : " ");
			n_u64 += STR_VAR_LEN_MAX / sizeof(u64);
		} else {
			trace_seq_printf(s, print_fmt, se->fields[i]->name,
					 entry->fields[n_u64],
					 i == se->n_fields - 1 ? "" : " ");
			n_u64++;
		}
	}
end:
	trace_seq_putc(s, '\n');

	return trace_handle_return(s);
}

static struct trace_event_functions synth_event_funcs = {
	.trace		= print_synth_event
};

static notrace void trace_event_raw_event_synth(void *__data,
						u64 *var_ref_vals,
						unsigned int var_ref_idx)
{
	struct trace_event_file *trace_file = __data;
	struct synth_trace_event *entry;
	struct trace_event_buffer fbuffer;
	struct ring_buffer *buffer;
	struct synth_event *event;
	unsigned int i, n_u64;
	int fields_size = 0;

	event = trace_file->event_call->data;

	if (trace_trigger_soft_disabled(trace_file))
		return;

	fields_size = event->n_u64 * sizeof(u64);

	/*
	 * Avoid ring buffer recursion detection, as this event
	 * is being performed within another event.
	 */
	buffer = trace_file->tr->trace_buffer.buffer;
	ring_buffer_nest_start(buffer);

	entry = trace_event_buffer_reserve(&fbuffer, trace_file,
					   sizeof(*entry) + fields_size);
	if (!entry)
		goto out;

	for (i = 0, n_u64 = 0; i < event->n_fields; i++) {
		if (event->fields[i]->is_string) {
			char *str_val = (char *)(long)var_ref_vals[var_ref_idx + i];
			char *str_field = (char *)&entry->fields[n_u64];

			strscpy(str_field, str_val, STR_VAR_LEN_MAX);
			n_u64 += STR_VAR_LEN_MAX / sizeof(u64);
		} else {
			struct synth_field *field = event->fields[i];
			u64 val = var_ref_vals[var_ref_idx + i];

			switch (field->size) {
			case 1:
				*(u8 *)&entry->fields[n_u64] = (u8)val;
				break;

			case 2:
				*(u16 *)&entry->fields[n_u64] = (u16)val;
				break;

			case 4:
				*(u32 *)&entry->fields[n_u64] = (u32)val;
				break;

			default:
				entry->fields[n_u64] = val;
				break;
			}
			n_u64++;
		}
	}

	trace_event_buffer_commit(&fbuffer);
out:
	ring_buffer_nest_end(buffer);
}

static void free_synth_event_print_fmt(struct trace_event_call *call)
{
	if (call) {
		kfree(call->print_fmt);
		call->print_fmt = NULL;
	}
}

static int __set_synth_event_print_fmt(struct synth_event *event,
				       char *buf, int len)
{
	const char *fmt;
	int pos = 0;
	int i;

	/* When len=0, we just calculate the needed length */
#define LEN_OR_ZERO (len ? len - pos : 0)

	pos += snprintf(buf + pos, LEN_OR_ZERO, "\"");
	for (i = 0; i < event->n_fields; i++) {
		fmt = synth_field_fmt(event->fields[i]->type);
		pos += snprintf(buf + pos, LEN_OR_ZERO, "%s=%s%s",
				event->fields[i]->name, fmt,
				i == event->n_fields - 1 ? "" : ", ");
	}
	pos += snprintf(buf + pos, LEN_OR_ZERO, "\"");

	for (i = 0; i < event->n_fields; i++) {
		pos += snprintf(buf + pos, LEN_OR_ZERO,
				", REC->%s", event->fields[i]->name);
	}

#undef LEN_OR_ZERO

	/* return the length of print_fmt */
	return pos;
}

static int set_synth_event_print_fmt(struct trace_event_call *call)
{
	struct synth_event *event = call->data;
	char *print_fmt;
	int len;

	/* First: called with 0 length to calculate the needed length */
	len = __set_synth_event_print_fmt(event, NULL, 0);

	print_fmt = kmalloc(len + 1, GFP_KERNEL);
	if (!print_fmt)
		return -ENOMEM;

	/* Second: actually write the @print_fmt */
	__set_synth_event_print_fmt(event, print_fmt, len + 1);
	call->print_fmt = print_fmt;

	return 0;
}

static void free_synth_field(struct synth_field *field)
{
	kfree(field->type);
	kfree(field->name);
	kfree(field);
}

static struct synth_field *parse_synth_field(int argc, char **argv,
					     int *consumed)
{
	struct synth_field *field;
	const char *prefix = NULL;
	char *field_type = argv[0], *field_name;
	int len, ret = 0;
	char *array;

	if (field_type[0] == ';')
		field_type++;

	if (!strcmp(field_type, "unsigned")) {
		if (argc < 3)
			return ERR_PTR(-EINVAL);
		prefix = "unsigned ";
		field_type = argv[1];
		field_name = argv[2];
		*consumed = 3;
	} else {
		field_name = argv[1];
		*consumed = 2;
	}

	len = strlen(field_name);
	if (field_name[len - 1] == ';')
		field_name[len - 1] = '\0';

	field = kzalloc(sizeof(*field), GFP_KERNEL);
	if (!field)
		return ERR_PTR(-ENOMEM);

	len = strlen(field_type) + 1;
	array = strchr(field_name, '[');
	if (array)
		len += strlen(array);
	if (prefix)
		len += strlen(prefix);
	field->type = kzalloc(len, GFP_KERNEL);
	if (!field->type) {
		ret = -ENOMEM;
		goto free;
	}
	if (prefix)
		strcat(field->type, prefix);
	strcat(field->type, field_type);
	if (array) {
		strcat(field->type, array);
		*array = '\0';
	}

	field->size = synth_field_size(field->type);
	if (!field->size) {
		ret = -EINVAL;
		goto free;
	}

	if (synth_field_is_string(field->type))
		field->is_string = true;

	field->is_signed = synth_field_signed(field->type);

	field->name = kstrdup(field_name, GFP_KERNEL);
	if (!field->name) {
		ret = -ENOMEM;
		goto free;
	}
 out:
	return field;
 free:
	free_synth_field(field);
	field = ERR_PTR(ret);
	goto out;
}

static void free_synth_tracepoint(struct tracepoint *tp)
{
	if (!tp)
		return;

	kfree(tp->name);
	kfree(tp);
}

static struct tracepoint *alloc_synth_tracepoint(char *name)
{
	struct tracepoint *tp;

	tp = kzalloc(sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return ERR_PTR(-ENOMEM);

	tp->name = kstrdup(name, GFP_KERNEL);
	if (!tp->name) {
		kfree(tp);
		return ERR_PTR(-ENOMEM);
	}

	return tp;
}

typedef void (*synth_probe_func_t) (void *__data, u64 *var_ref_vals,
				    unsigned int var_ref_idx);

static inline void trace_synth(struct synth_event *event, u64 *var_ref_vals,
			       unsigned int var_ref_idx)
{
	struct tracepoint *tp = event->tp;

	if (unlikely(atomic_read(&tp->key.enabled) > 0)) {
		struct tracepoint_func *probe_func_ptr;
		synth_probe_func_t probe_func;
		void *__data;

		if (!(cpu_online(raw_smp_processor_id())))
			return;

		probe_func_ptr = rcu_dereference_sched((tp)->funcs);
		if (probe_func_ptr) {
			do {
				probe_func = probe_func_ptr->func;
				__data = probe_func_ptr->data;
				probe_func(__data, var_ref_vals, var_ref_idx);
			} while ((++probe_func_ptr)->func);
		}
	}
}

static struct synth_event *find_synth_event(const char *name)
{
	struct synth_event *event;

	list_for_each_entry(event, &synth_event_list, list) {
		if (strcmp(event->name, name) == 0)
			return event;
	}

	return NULL;
}

static int register_synth_event(struct synth_event *event)
{
	struct trace_event_call *call = &event->call;
	int ret = 0;

	event->call.class = &event->class;
	event->class.system = kstrdup(SYNTH_SYSTEM, GFP_KERNEL);
	if (!event->class.system) {
		ret = -ENOMEM;
		goto out;
	}

	event->tp = alloc_synth_tracepoint(event->name);
	if (IS_ERR(event->tp)) {
		ret = PTR_ERR(event->tp);
		event->tp = NULL;
		goto out;
	}

	INIT_LIST_HEAD(&call->class->fields);
	call->event.funcs = &synth_event_funcs;
	call->class->define_fields = synth_event_define_fields;

	ret = register_trace_event(&call->event);
	if (!ret) {
		ret = -ENODEV;
		goto out;
	}
	call->flags = TRACE_EVENT_FL_TRACEPOINT;
	call->class->reg = trace_event_reg;
	call->class->probe = trace_event_raw_event_synth;
	call->data = event;
	call->tp = event->tp;

	ret = trace_add_event_call_nolock(call);
	if (ret) {
		pr_warn("Failed to register synthetic event: %s\n",
			trace_event_name(call));
		goto err;
	}

	ret = set_synth_event_print_fmt(call);
	if (ret < 0) {
		trace_remove_event_call(call);
		goto err;
	}
 out:
	return ret;
 err:
	unregister_trace_event(&call->event);
	goto out;
}

static int unregister_synth_event(struct synth_event *event)
{
	struct trace_event_call *call = &event->call;
	int ret;

	ret = trace_remove_event_call_nolock(call);

	return ret;
}

static void free_synth_event(struct synth_event *event)
{
	unsigned int i;

	if (!event)
		return;

	for (i = 0; i < event->n_fields; i++)
		free_synth_field(event->fields[i]);

	kfree(event->fields);
	kfree(event->name);
	kfree(event->class.system);
	free_synth_tracepoint(event->tp);
	free_synth_event_print_fmt(&event->call);
	kfree(event);
}

static struct synth_event *alloc_synth_event(char *event_name, int n_fields,
					     struct synth_field **fields)
{
	struct synth_event *event;
	unsigned int i;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		event = ERR_PTR(-ENOMEM);
		goto out;
	}

	event->name = kstrdup(event_name, GFP_KERNEL);
	if (!event->name) {
		kfree(event);
		event = ERR_PTR(-ENOMEM);
		goto out;
	}

	event->fields = kcalloc(n_fields, sizeof(*event->fields), GFP_KERNEL);
	if (!event->fields) {
		free_synth_event(event);
		event = ERR_PTR(-ENOMEM);
		goto out;
	}

	for (i = 0; i < n_fields; i++)
		event->fields[i] = fields[i];

	event->n_fields = n_fields;
 out:
	return event;
}

static void action_trace(struct hist_trigger_data *hist_data,
			 struct tracing_map_elt *elt, void *rec,
			 struct ring_buffer_event *rbe,
			 struct action_data *data, u64 *var_ref_vals)
{
	struct synth_event *event = data->onmatch.synth_event;

	trace_synth(event, var_ref_vals, data->onmatch.var_ref_idx);
}

struct hist_var_data {
	struct list_head list;
	struct hist_trigger_data *hist_data;
};

static void add_or_delete_synth_event(struct synth_event *event, int delete)
{
	if (delete)
		free_synth_event(event);
	else {
		if (!find_synth_event(event->name))
			list_add(&event->list, &synth_event_list);
		else
			free_synth_event(event);
	}
}

static int create_synth_event(int argc, char **argv)
{
	struct synth_field *field, *fields[SYNTH_FIELDS_MAX];
	struct synth_event *event = NULL;
	bool delete_event = false;
	int i, consumed = 0, n_fields = 0, ret = 0;
	char *name;

	mutex_lock(&event_mutex);
	mutex_lock(&synth_event_mutex);

	/*
	 * Argument syntax:
	 *  - Add synthetic event: <event_name> field[;field] ...
	 *  - Remove synthetic event: !<event_name> field[;field] ...
	 *      where 'field' = type field_name
	 */
	if (argc < 1) {
		ret = -EINVAL;
		goto out;
	}

	name = argv[0];
	if (name[0] == '!') {
		delete_event = true;
		name++;
	}

	event = find_synth_event(name);
	if (event) {
		if (delete_event) {
			if (event->ref) {
				event = NULL;
				ret = -EBUSY;
				goto out;
			}
			list_del(&event->list);
			goto out;
		}
		event = NULL;
		ret = -EEXIST;
		goto out;
	} else if (delete_event) {
		ret = -ENOENT;
		goto out;
	}

	if (argc < 2) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 1; i < argc - 1; i++) {
		if (strcmp(argv[i], ";") == 0)
			continue;
		if (n_fields == SYNTH_FIELDS_MAX) {
			ret = -EINVAL;
			goto err;
		}

		field = parse_synth_field(argc - i, &argv[i], &consumed);
		if (IS_ERR(field)) {
			ret = PTR_ERR(field);
			goto err;
		}
		fields[n_fields++] = field;
		i += consumed - 1;
	}

	if (i < argc && strcmp(argv[i], ";") != 0) {
		ret = -EINVAL;
		goto err;
	}

	event = alloc_synth_event(name, n_fields, fields);
	if (IS_ERR(event)) {
		ret = PTR_ERR(event);
		event = NULL;
		goto err;
	}
 out:
	if (event) {
		if (delete_event) {
			ret = unregister_synth_event(event);
			add_or_delete_synth_event(event, !ret);
		} else {
			ret = register_synth_event(event);
			add_or_delete_synth_event(event, ret);
		}
	}
	mutex_unlock(&synth_event_mutex);
	mutex_unlock(&event_mutex);

	return ret;
 err:
	mutex_unlock(&synth_event_mutex);
	mutex_unlock(&event_mutex);

	for (i = 0; i < n_fields; i++)
		free_synth_field(fields[i]);
	free_synth_event(event);

	return ret;
}

static int release_all_synth_events(void)
{
	struct synth_event *event, *e;
	int ret = 0;

	mutex_lock(&event_mutex);
	mutex_lock(&synth_event_mutex);

	list_for_each_entry(event, &synth_event_list, list) {
		if (event->ref) {
			mutex_unlock(&synth_event_mutex);
			return -EBUSY;
		}
	}

	list_for_each_entry_safe(event, e, &synth_event_list, list) {
		list_del(&event->list);

		ret = unregister_synth_event(event);
		add_or_delete_synth_event(event, !ret);
	}
	mutex_unlock(&synth_event_mutex);
	mutex_unlock(&event_mutex);

	return ret;
}


static void *synth_events_seq_start(struct seq_file *m, loff_t *pos)
{
	mutex_lock(&synth_event_mutex);

	return seq_list_start(&synth_event_list, *pos);
}

static void *synth_events_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_list_next(v, &synth_event_list, pos);
}

static void synth_events_seq_stop(struct seq_file *m, void *v)
{
	mutex_unlock(&synth_event_mutex);
}

static int synth_events_seq_show(struct seq_file *m, void *v)
{
	struct synth_field *field;
	struct synth_event *event = v;
	unsigned int i;

	seq_printf(m, "%s\t", event->name);

	for (i = 0; i < event->n_fields; i++) {
		field = event->fields[i];

		/* parameter values */
		seq_printf(m, "%s %s%s", field->type, field->name,
			   i == event->n_fields - 1 ? "" : "; ");
	}

	seq_putc(m, '\n');

	return 0;
}

static const struct seq_operations synth_events_seq_op = {
	.start  = synth_events_seq_start,
	.next   = synth_events_seq_next,
	.stop   = synth_events_seq_stop,
	.show   = synth_events_seq_show
};

static int synth_events_open(struct inode *inode, struct file *file)
{
	int ret;

	if ((file->f_mode & FMODE_WRITE) && (file->f_flags & O_TRUNC)) {
		ret = release_all_synth_events();
		if (ret < 0)
			return ret;
	}

	return seq_open(file, &synth_events_seq_op);
}

static ssize_t synth_events_write(struct file *file,
				  const char __user *buffer,
				  size_t count, loff_t *ppos)
{
	return trace_parse_run_command(file, buffer, count, ppos,
				       create_synth_event);
}

static const struct file_operations synth_events_fops = {
	.open           = synth_events_open,
	.write		= synth_events_write,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = seq_release,
};

static u64 hist_field_timestamp(struct hist_field *hist_field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event)
{
	struct hist_trigger_data *hist_data = hist_field->hist_data;
	struct trace_array *tr = hist_data->event_file->tr;

	u64 ts = ring_buffer_event_time_stamp(rbe);

	if (hist_data->attrs->ts_in_usecs && trace_clock_in_ns(tr))
		ts = ns2usecs(ts);

	return ts;
}

static u64 hist_field_cpu(struct hist_field *hist_field,
			  struct tracing_map_elt *elt,
			  struct ring_buffer_event *rbe,
			  void *event)
{
	int cpu = smp_processor_id();

	return cpu;
}

static struct hist_field *
check_field_for_var_ref(struct hist_field *hist_field,
			struct hist_trigger_data *var_data,
			unsigned int var_idx)
{
	struct hist_field *found = NULL;

	if (hist_field && hist_field->flags & HIST_FIELD_FL_VAR_REF) {
		if (hist_field->var.idx == var_idx &&
		    hist_field->var.hist_data == var_data) {
			found = hist_field;
		}
	}

	return found;
}

static struct hist_field *
check_field_for_var_refs(struct hist_trigger_data *hist_data,
			 struct hist_field *hist_field,
			 struct hist_trigger_data *var_data,
			 unsigned int var_idx,
			 unsigned int level)
{
	struct hist_field *found = NULL;
	unsigned int i;

	if (level > 3)
		return found;

	if (!hist_field)
		return found;

	found = check_field_for_var_ref(hist_field, var_data, var_idx);
	if (found)
		return found;

	for (i = 0; i < HIST_FIELD_OPERANDS_MAX; i++) {
		struct hist_field *operand;

		operand = hist_field->operands[i];
		found = check_field_for_var_refs(hist_data, operand, var_data,
						 var_idx, level + 1);
		if (found)
			return found;
	}

	return found;
}

static struct hist_field *find_var_ref(struct hist_trigger_data *hist_data,
				       struct hist_trigger_data *var_data,
				       unsigned int var_idx)
{
	struct hist_field *hist_field, *found = NULL;
	unsigned int i;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		found = check_field_for_var_refs(hist_data, hist_field,
						 var_data, var_idx, 0);
		if (found)
			return found;
	}

	for (i = 0; i < hist_data->n_synth_var_refs; i++) {
		hist_field = hist_data->synth_var_refs[i];
		found = check_field_for_var_refs(hist_data, hist_field,
						 var_data, var_idx, 0);
		if (found)
			return found;
	}

	return found;
}

static struct hist_field *find_any_var_ref(struct hist_trigger_data *hist_data,
					   unsigned int var_idx)
{
	struct trace_array *tr = hist_data->event_file->tr;
	struct hist_field *found = NULL;
	struct hist_var_data *var_data;

	list_for_each_entry(var_data, &tr->hist_vars, list) {
		if (var_data->hist_data == hist_data)
			continue;
		found = find_var_ref(var_data->hist_data, hist_data, var_idx);
		if (found)
			break;
	}

	return found;
}

static bool check_var_refs(struct hist_trigger_data *hist_data)
{
	struct hist_field *field;
	bool found = false;
	int i;

	for_each_hist_field(i, hist_data) {
		field = hist_data->fields[i];
		if (field && field->flags & HIST_FIELD_FL_VAR) {
			if (find_any_var_ref(hist_data, field->var.idx)) {
				found = true;
				break;
			}
		}
	}

	return found;
}

static struct hist_var_data *find_hist_vars(struct hist_trigger_data *hist_data)
{
	struct trace_array *tr = hist_data->event_file->tr;
	struct hist_var_data *var_data, *found = NULL;

	list_for_each_entry(var_data, &tr->hist_vars, list) {
		if (var_data->hist_data == hist_data) {
			found = var_data;
			break;
		}
	}

	return found;
}

static bool field_has_hist_vars(struct hist_field *hist_field,
				unsigned int level)
{
	int i;

	if (level > 3)
		return false;

	if (!hist_field)
		return false;

	if (hist_field->flags & HIST_FIELD_FL_VAR ||
	    hist_field->flags & HIST_FIELD_FL_VAR_REF)
		return true;

	for (i = 0; i < HIST_FIELD_OPERANDS_MAX; i++) {
		struct hist_field *operand;

		operand = hist_field->operands[i];
		if (field_has_hist_vars(operand, level + 1))
			return true;
	}

	return false;
}

static bool has_hist_vars(struct hist_trigger_data *hist_data)
{
	struct hist_field *hist_field;
	int i;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (field_has_hist_vars(hist_field, 0))
			return true;
	}

	return false;
}

static int save_hist_vars(struct hist_trigger_data *hist_data)
{
	struct trace_array *tr = hist_data->event_file->tr;
	struct hist_var_data *var_data;

	var_data = find_hist_vars(hist_data);
	if (var_data)
		return 0;

	if (trace_array_get(tr) < 0)
		return -ENODEV;

	var_data = kzalloc(sizeof(*var_data), GFP_KERNEL);
	if (!var_data) {
		trace_array_put(tr);
		return -ENOMEM;
	}

	var_data->hist_data = hist_data;
	list_add(&var_data->list, &tr->hist_vars);

	return 0;
}

static void remove_hist_vars(struct hist_trigger_data *hist_data)
{
	struct trace_array *tr = hist_data->event_file->tr;
	struct hist_var_data *var_data;

	var_data = find_hist_vars(hist_data);
	if (!var_data)
		return;

	if (WARN_ON(check_var_refs(hist_data)))
		return;

	list_del(&var_data->list);

	kfree(var_data);

	trace_array_put(tr);
}

static struct hist_field *find_var_field(struct hist_trigger_data *hist_data,
					 const char *var_name)
{
	struct hist_field *hist_field, *found = NULL;
	int i;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field && hist_field->flags & HIST_FIELD_FL_VAR &&
		    strcmp(hist_field->var.name, var_name) == 0) {
			found = hist_field;
			break;
		}
	}

	return found;
}

static struct hist_field *find_var(struct hist_trigger_data *hist_data,
				   struct trace_event_file *file,
				   const char *var_name)
{
	struct hist_trigger_data *test_data;
	struct event_trigger_data *test;
	struct hist_field *hist_field;

	hist_field = find_var_field(hist_data, var_name);
	if (hist_field)
		return hist_field;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			test_data = test->private_data;
			hist_field = find_var_field(test_data, var_name);
			if (hist_field)
				return hist_field;
		}
	}

	return NULL;
}

static struct trace_event_file *find_var_file(struct trace_array *tr,
					      char *system,
					      char *event_name,
					      char *var_name)
{
	struct hist_trigger_data *var_hist_data;
	struct hist_var_data *var_data;
	struct trace_event_file *file, *found = NULL;

	if (system)
		return find_event_file(tr, system, event_name);

	list_for_each_entry(var_data, &tr->hist_vars, list) {
		var_hist_data = var_data->hist_data;
		file = var_hist_data->event_file;
		if (file == found)
			continue;

		if (find_var_field(var_hist_data, var_name)) {
			if (found) {
				hist_err_event("Variable name not unique, need to use fully qualified name (subsys.event.var) for variable: ", system, event_name, var_name);
				return NULL;
			}

			found = file;
		}
	}

	return found;
}

static struct hist_field *find_file_var(struct trace_event_file *file,
					const char *var_name)
{
	struct hist_trigger_data *test_data;
	struct event_trigger_data *test;
	struct hist_field *hist_field;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			test_data = test->private_data;
			hist_field = find_var_field(test_data, var_name);
			if (hist_field)
				return hist_field;
		}
	}

	return NULL;
}

static struct hist_field *
find_match_var(struct hist_trigger_data *hist_data, char *var_name)
{
	struct trace_array *tr = hist_data->event_file->tr;
	struct hist_field *hist_field, *found = NULL;
	struct trace_event_file *file;
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];

		if (data->fn == action_trace) {
			char *system = data->onmatch.match_event_system;
			char *event_name = data->onmatch.match_event;

			file = find_var_file(tr, system, event_name, var_name);
			if (!file)
				continue;
			hist_field = find_file_var(file, var_name);
			if (hist_field) {
				if (found) {
					hist_err_event("Variable name not unique, need to use fully qualified name (subsys.event.var) for variable: ", system, event_name, var_name);
					return ERR_PTR(-EINVAL);
				}

				found = hist_field;
			}
		}
	}
	return found;
}

static struct hist_field *find_event_var(struct hist_trigger_data *hist_data,
					 char *system,
					 char *event_name,
					 char *var_name)
{
	struct trace_array *tr = hist_data->event_file->tr;
	struct hist_field *hist_field = NULL;
	struct trace_event_file *file;

	if (!system || !event_name) {
		hist_field = find_match_var(hist_data, var_name);
		if (IS_ERR(hist_field))
			return NULL;
		if (hist_field)
			return hist_field;
	}

	file = find_var_file(tr, system, event_name, var_name);
	if (!file)
		return NULL;

	hist_field = find_file_var(file, var_name);

	return hist_field;
}

struct hist_elt_data {
	char *comm;
	u64 *var_ref_vals;
	char *field_var_str[SYNTH_FIELDS_MAX];
};

static u64 hist_field_var_ref(struct hist_field *hist_field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	struct hist_elt_data *elt_data;
	u64 var_val = 0;

	if (WARN_ON_ONCE(!elt))
		return var_val;

	elt_data = elt->private_data;
	var_val = elt_data->var_ref_vals[hist_field->var_ref_idx];

	return var_val;
}

static bool resolve_var_refs(struct hist_trigger_data *hist_data, void *key,
			     u64 *var_ref_vals, bool self)
{
	struct hist_trigger_data *var_data;
	struct tracing_map_elt *var_elt;
	struct hist_field *hist_field;
	unsigned int i, var_idx;
	bool resolved = true;
	u64 var_val = 0;

	for (i = 0; i < hist_data->n_var_refs; i++) {
		hist_field = hist_data->var_refs[i];
		var_idx = hist_field->var.idx;
		var_data = hist_field->var.hist_data;

		if (var_data == NULL) {
			resolved = false;
			break;
		}

		if ((self && var_data != hist_data) ||
		    (!self && var_data == hist_data))
			continue;

		var_elt = tracing_map_lookup(var_data->map, key);
		if (!var_elt) {
			resolved = false;
			break;
		}

		if (!tracing_map_var_set(var_elt, var_idx)) {
			resolved = false;
			break;
		}

		if (self || !hist_field->read_once)
			var_val = tracing_map_read_var(var_elt, var_idx);
		else
			var_val = tracing_map_read_var_once(var_elt, var_idx);

		var_ref_vals[i] = var_val;
	}

	return resolved;
}

static const char *hist_field_name(struct hist_field *field,
				   unsigned int level)
{
	const char *field_name = "";

	if (level > 1)
		return field_name;

	if (field->field)
		field_name = field->field->name;
	else if (field->flags & HIST_FIELD_FL_LOG2 ||
		 field->flags & HIST_FIELD_FL_ALIAS)
		field_name = hist_field_name(field->operands[0], ++level);
	else if (field->flags & HIST_FIELD_FL_CPU)
		field_name = "common_cpu";
	else if (field->flags & HIST_FIELD_FL_EXPR ||
		 field->flags & HIST_FIELD_FL_VAR_REF) {
		if (field->system) {
			static char full_name[MAX_FILTER_STR_VAL];

			strcat(full_name, field->system);
			strcat(full_name, ".");
			strcat(full_name, field->event_name);
			strcat(full_name, ".");
			strcat(full_name, field->name);
			field_name = full_name;
		} else
			field_name = field->name;
	} else if (field->flags & HIST_FIELD_FL_TIMESTAMP)
		field_name = "common_timestamp";

	if (field_name == NULL)
		field_name = "";

	return field_name;
}

static hist_field_fn_t select_value_fn(int field_size, int field_is_signed)
{
	hist_field_fn_t fn = NULL;

	switch (field_size) {
	case 8:
		if (field_is_signed)
			fn = hist_field_s64;
		else
			fn = hist_field_u64;
		break;
	case 4:
		if (field_is_signed)
			fn = hist_field_s32;
		else
			fn = hist_field_u32;
		break;
	case 2:
		if (field_is_signed)
			fn = hist_field_s16;
		else
			fn = hist_field_u16;
		break;
	case 1:
		if (field_is_signed)
			fn = hist_field_s8;
		else
			fn = hist_field_u8;
		break;
	}

	return fn;
}

static int parse_map_size(char *str)
{
	unsigned long size, map_bits;
	int ret;

	strsep(&str, "=");
	if (!str) {
		ret = -EINVAL;
		goto out;
	}

	ret = kstrtoul(str, 0, &size);
	if (ret)
		goto out;

	map_bits = ilog2(roundup_pow_of_two(size));
	if (map_bits < TRACING_MAP_BITS_MIN ||
	    map_bits > TRACING_MAP_BITS_MAX)
		ret = -EINVAL;
	else
		ret = map_bits;
 out:
	return ret;
}

static void destroy_hist_trigger_attrs(struct hist_trigger_attrs *attrs)
{
	unsigned int i;

	if (!attrs)
		return;

	for (i = 0; i < attrs->n_assignments; i++)
		kfree(attrs->assignment_str[i]);

	for (i = 0; i < attrs->n_actions; i++)
		kfree(attrs->action_str[i]);

	kfree(attrs->name);
	kfree(attrs->sort_key_str);
	kfree(attrs->keys_str);
	kfree(attrs->vals_str);
	kfree(attrs->clock);
	kfree(attrs);
}

static int parse_action(char *str, struct hist_trigger_attrs *attrs)
{
	int ret = -EINVAL;

	if (attrs->n_actions >= HIST_ACTIONS_MAX)
		return ret;

	if ((strncmp(str, "onmatch(", strlen("onmatch(")) == 0) ||
	    (strncmp(str, "onmax(", strlen("onmax(")) == 0)) {
		attrs->action_str[attrs->n_actions] = kstrdup(str, GFP_KERNEL);
		if (!attrs->action_str[attrs->n_actions]) {
			ret = -ENOMEM;
			return ret;
		}
		attrs->n_actions++;
		ret = 0;
	}

	return ret;
}

static int parse_assignment(char *str, struct hist_trigger_attrs *attrs)
{
	int ret = 0;

	if ((strncmp(str, "key=", strlen("key=")) == 0) ||
	    (strncmp(str, "keys=", strlen("keys=")) == 0)) {
		attrs->keys_str = kstrdup(str, GFP_KERNEL);
		if (!attrs->keys_str) {
			ret = -ENOMEM;
			goto out;
		}
	} else if ((strncmp(str, "val=", strlen("val=")) == 0) ||
		 (strncmp(str, "vals=", strlen("vals=")) == 0) ||
		 (strncmp(str, "values=", strlen("values=")) == 0)) {
		attrs->vals_str = kstrdup(str, GFP_KERNEL);
		if (!attrs->vals_str) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (strncmp(str, "sort=", strlen("sort=")) == 0) {
		attrs->sort_key_str = kstrdup(str, GFP_KERNEL);
		if (!attrs->sort_key_str) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (strncmp(str, "name=", strlen("name=")) == 0) {
		attrs->name = kstrdup(str, GFP_KERNEL);
		if (!attrs->name) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (strncmp(str, "clock=", strlen("clock=")) == 0) {
		strsep(&str, "=");
		if (!str) {
			ret = -EINVAL;
			goto out;
		}

		str = strstrip(str);
		attrs->clock = kstrdup(str, GFP_KERNEL);
		if (!attrs->clock) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (strncmp(str, "size=", strlen("size=")) == 0) {
		int map_bits = parse_map_size(str);

		if (map_bits < 0) {
			ret = map_bits;
			goto out;
		}
		attrs->map_bits = map_bits;
	} else {
		char *assignment;

		if (attrs->n_assignments == TRACING_MAP_VARS_MAX) {
			hist_err("Too many variables defined: ", str);
			ret = -EINVAL;
			goto out;
		}

		assignment = kstrdup(str, GFP_KERNEL);
		if (!assignment) {
			ret = -ENOMEM;
			goto out;
		}

		attrs->assignment_str[attrs->n_assignments++] = assignment;
	}
 out:
	return ret;
}

static struct hist_trigger_attrs *parse_hist_trigger_attrs(char *trigger_str)
{
	struct hist_trigger_attrs *attrs;
	int ret = 0;

	attrs = kzalloc(sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return ERR_PTR(-ENOMEM);

	while (trigger_str) {
		char *str = strsep(&trigger_str, ":");

		if (strchr(str, '=')) {
			ret = parse_assignment(str, attrs);
			if (ret)
				goto free;
		} else if (strcmp(str, "pause") == 0)
			attrs->pause = true;
		else if ((strcmp(str, "cont") == 0) ||
			 (strcmp(str, "continue") == 0))
			attrs->cont = true;
		else if (strcmp(str, "clear") == 0)
			attrs->clear = true;
		else {
			ret = parse_action(str, attrs);
			if (ret)
				goto free;
		}
	}

	if (!attrs->keys_str) {
		ret = -EINVAL;
		goto free;
	}

	if (!attrs->clock) {
		attrs->clock = kstrdup("global", GFP_KERNEL);
		if (!attrs->clock) {
			ret = -ENOMEM;
			goto free;
		}
	}

	return attrs;
 free:
	destroy_hist_trigger_attrs(attrs);

	return ERR_PTR(ret);
}

static inline void save_comm(char *comm, struct task_struct *task)
{
	if (!task->pid) {
		strcpy(comm, "<idle>");
		return;
	}

	if (WARN_ON_ONCE(task->pid < 0)) {
		strcpy(comm, "<XXX>");
		return;
	}

	memcpy(comm, task->comm, TASK_COMM_LEN);
}

static void hist_elt_data_free(struct hist_elt_data *elt_data)
{
	unsigned int i;

	for (i = 0; i < SYNTH_FIELDS_MAX; i++)
		kfree(elt_data->field_var_str[i]);

	kfree(elt_data->comm);
	kfree(elt_data);
}

static void hist_trigger_elt_data_free(struct tracing_map_elt *elt)
{
	struct hist_elt_data *elt_data = elt->private_data;

	hist_elt_data_free(elt_data);
}

static int hist_trigger_elt_data_alloc(struct tracing_map_elt *elt)
{
	struct hist_trigger_data *hist_data = elt->map->private_data;
	unsigned int size = TASK_COMM_LEN;
	struct hist_elt_data *elt_data;
	struct hist_field *key_field;
	unsigned int i, n_str;

	elt_data = kzalloc(sizeof(*elt_data), GFP_KERNEL);
	if (!elt_data)
		return -ENOMEM;

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (key_field->flags & HIST_FIELD_FL_EXECNAME) {
			elt_data->comm = kzalloc(size, GFP_KERNEL);
			if (!elt_data->comm) {
				kfree(elt_data);
				return -ENOMEM;
			}
			break;
		}
	}

	n_str = hist_data->n_field_var_str + hist_data->n_max_var_str;

	size = STR_VAR_LEN_MAX;

	for (i = 0; i < n_str; i++) {
		elt_data->field_var_str[i] = kzalloc(size, GFP_KERNEL);
		if (!elt_data->field_var_str[i]) {
			hist_elt_data_free(elt_data);
			return -ENOMEM;
		}
	}

	elt->private_data = elt_data;

	return 0;
}

static void hist_trigger_elt_data_init(struct tracing_map_elt *elt)
{
	struct hist_elt_data *elt_data = elt->private_data;

	if (elt_data->comm)
		save_comm(elt_data->comm, current);
}

static const struct tracing_map_ops hist_trigger_elt_data_ops = {
	.elt_alloc	= hist_trigger_elt_data_alloc,
	.elt_free	= hist_trigger_elt_data_free,
	.elt_init	= hist_trigger_elt_data_init,
};

static const char *get_hist_field_flags(struct hist_field *hist_field)
{
	const char *flags_str = NULL;

	if (hist_field->flags & HIST_FIELD_FL_HEX)
		flags_str = "hex";
	else if (hist_field->flags & HIST_FIELD_FL_SYM)
		flags_str = "sym";
	else if (hist_field->flags & HIST_FIELD_FL_SYM_OFFSET)
		flags_str = "sym-offset";
	else if (hist_field->flags & HIST_FIELD_FL_EXECNAME)
		flags_str = "execname";
	else if (hist_field->flags & HIST_FIELD_FL_SYSCALL)
		flags_str = "syscall";
	else if (hist_field->flags & HIST_FIELD_FL_LOG2)
		flags_str = "log2";
	else if (hist_field->flags & HIST_FIELD_FL_TIMESTAMP_USECS)
		flags_str = "usecs";

	return flags_str;
}

static void expr_field_str(struct hist_field *field, char *expr)
{
	if (field->flags & HIST_FIELD_FL_VAR_REF)
		strcat(expr, "$");

	strcat(expr, hist_field_name(field, 0));

	if (field->flags && !(field->flags & HIST_FIELD_FL_VAR_REF)) {
		const char *flags_str = get_hist_field_flags(field);

		if (flags_str) {
			strcat(expr, ".");
			strcat(expr, flags_str);
		}
	}
}

static char *expr_str(struct hist_field *field, unsigned int level)
{
	char *expr;

	if (level > 1)
		return NULL;

	expr = kzalloc(MAX_FILTER_STR_VAL, GFP_KERNEL);
	if (!expr)
		return NULL;

	if (!field->operands[0]) {
		expr_field_str(field, expr);
		return expr;
	}

	if (field->operator == FIELD_OP_UNARY_MINUS) {
		char *subexpr;

		strcat(expr, "-(");
		subexpr = expr_str(field->operands[0], ++level);
		if (!subexpr) {
			kfree(expr);
			return NULL;
		}
		strcat(expr, subexpr);
		strcat(expr, ")");

		kfree(subexpr);

		return expr;
	}

	expr_field_str(field->operands[0], expr);

	switch (field->operator) {
	case FIELD_OP_MINUS:
		strcat(expr, "-");
		break;
	case FIELD_OP_PLUS:
		strcat(expr, "+");
		break;
	default:
		kfree(expr);
		return NULL;
	}

	expr_field_str(field->operands[1], expr);

	return expr;
}

static int contains_operator(char *str)
{
	enum field_op_id field_op = FIELD_OP_NONE;
	char *op;

	op = strpbrk(str, "+-");
	if (!op)
		return FIELD_OP_NONE;

	switch (*op) {
	case '-':
		if (*str == '-')
			field_op = FIELD_OP_UNARY_MINUS;
		else
			field_op = FIELD_OP_MINUS;
		break;
	case '+':
		field_op = FIELD_OP_PLUS;
		break;
	default:
		break;
	}

	return field_op;
}

static void destroy_hist_field(struct hist_field *hist_field,
			       unsigned int level)
{
	unsigned int i;

	if (level > 3)
		return;

	if (!hist_field)
		return;

	for (i = 0; i < HIST_FIELD_OPERANDS_MAX; i++)
		destroy_hist_field(hist_field->operands[i], level + 1);

	kfree(hist_field->var.name);
	kfree(hist_field->name);
	kfree(hist_field->type);

	kfree(hist_field);
}

static struct hist_field *create_hist_field(struct hist_trigger_data *hist_data,
					    struct ftrace_event_field *field,
					    unsigned long flags,
					    char *var_name)
{
	struct hist_field *hist_field;

	if (field && is_function_field(field))
		return NULL;

	hist_field = kzalloc(sizeof(struct hist_field), GFP_KERNEL);
	if (!hist_field)
		return NULL;

	hist_field->hist_data = hist_data;

	if (flags & HIST_FIELD_FL_EXPR || flags & HIST_FIELD_FL_ALIAS)
		goto out; /* caller will populate */

	if (flags & HIST_FIELD_FL_VAR_REF) {
		hist_field->fn = hist_field_var_ref;
		goto out;
	}

	if (flags & HIST_FIELD_FL_HITCOUNT) {
		hist_field->fn = hist_field_counter;
		hist_field->size = sizeof(u64);
		hist_field->type = kstrdup("u64", GFP_KERNEL);
		if (!hist_field->type)
			goto free;
		goto out;
	}

	if (flags & HIST_FIELD_FL_STACKTRACE) {
		hist_field->fn = hist_field_none;
		goto out;
	}

	if (flags & HIST_FIELD_FL_LOG2) {
		unsigned long fl = flags & ~HIST_FIELD_FL_LOG2;
		hist_field->fn = hist_field_log2;
		hist_field->operands[0] = create_hist_field(hist_data, field, fl, NULL);
		if (!hist_field->operands[0])
			goto free;
		hist_field->size = hist_field->operands[0]->size;
		hist_field->type = kstrdup(hist_field->operands[0]->type, GFP_KERNEL);
		if (!hist_field->type)
			goto free;
		goto out;
	}

	if (flags & HIST_FIELD_FL_TIMESTAMP) {
		hist_field->fn = hist_field_timestamp;
		hist_field->size = sizeof(u64);
		hist_field->type = kstrdup("u64", GFP_KERNEL);
		if (!hist_field->type)
			goto free;
		goto out;
	}

	if (flags & HIST_FIELD_FL_CPU) {
		hist_field->fn = hist_field_cpu;
		hist_field->size = sizeof(int);
		hist_field->type = kstrdup("unsigned int", GFP_KERNEL);
		if (!hist_field->type)
			goto free;
		goto out;
	}

	if (WARN_ON_ONCE(!field))
		goto out;

	/* Pointers to strings are just pointers and dangerous to dereference */
	if (is_string_field(field) &&
	    (field->filter_type != FILTER_PTR_STRING)) {
		flags |= HIST_FIELD_FL_STRING;

		hist_field->size = MAX_FILTER_STR_VAL;
		hist_field->type = kstrdup(field->type, GFP_KERNEL);
		if (!hist_field->type)
			goto free;

		if (field->filter_type == FILTER_STATIC_STRING)
			hist_field->fn = hist_field_string;
		else if (field->filter_type == FILTER_DYN_STRING)
			hist_field->fn = hist_field_dynstring;
		else
			hist_field->fn = hist_field_pstring;
	} else {
		hist_field->size = field->size;
		hist_field->is_signed = field->is_signed;
		hist_field->type = kstrdup(field->type, GFP_KERNEL);
		if (!hist_field->type)
			goto free;

		hist_field->fn = select_value_fn(field->size,
						 field->is_signed);
		if (!hist_field->fn) {
			destroy_hist_field(hist_field, 0);
			return NULL;
		}
	}
 out:
	hist_field->field = field;
	hist_field->flags = flags;

	if (var_name) {
		hist_field->var.name = kstrdup(var_name, GFP_KERNEL);
		if (!hist_field->var.name)
			goto free;
	}

	return hist_field;
 free:
	destroy_hist_field(hist_field, 0);
	return NULL;
}

static void destroy_hist_fields(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < HIST_FIELDS_MAX; i++) {
		if (hist_data->fields[i]) {
			destroy_hist_field(hist_data->fields[i], 0);
			hist_data->fields[i] = NULL;
		}
	}
}

static int init_var_ref(struct hist_field *ref_field,
			struct hist_field *var_field,
			char *system, char *event_name)
{
	int err = 0;

	ref_field->var.idx = var_field->var.idx;
	ref_field->var.hist_data = var_field->hist_data;
	ref_field->size = var_field->size;
	ref_field->is_signed = var_field->is_signed;
	ref_field->flags |= var_field->flags &
		(HIST_FIELD_FL_TIMESTAMP | HIST_FIELD_FL_TIMESTAMP_USECS);

	if (system) {
		ref_field->system = kstrdup(system, GFP_KERNEL);
		if (!ref_field->system)
			return -ENOMEM;
	}

	if (event_name) {
		ref_field->event_name = kstrdup(event_name, GFP_KERNEL);
		if (!ref_field->event_name) {
			err = -ENOMEM;
			goto free;
		}
	}

	if (var_field->var.name) {
		ref_field->name = kstrdup(var_field->var.name, GFP_KERNEL);
		if (!ref_field->name) {
			err = -ENOMEM;
			goto free;
		}
	} else if (var_field->name) {
		ref_field->name = kstrdup(var_field->name, GFP_KERNEL);
		if (!ref_field->name) {
			err = -ENOMEM;
			goto free;
		}
	}

	ref_field->type = kstrdup(var_field->type, GFP_KERNEL);
	if (!ref_field->type) {
		err = -ENOMEM;
		goto free;
	}
 out:
	return err;
 free:
	kfree(ref_field->system);
	kfree(ref_field->event_name);
	kfree(ref_field->name);

	goto out;
}

static struct hist_field *create_var_ref(struct hist_field *var_field,
					 char *system, char *event_name)
{
	unsigned long flags = HIST_FIELD_FL_VAR_REF;
	struct hist_field *ref_field;

	ref_field = create_hist_field(var_field->hist_data, NULL, flags, NULL);
	if (ref_field) {
		if (init_var_ref(ref_field, var_field, system, event_name)) {
			destroy_hist_field(ref_field, 0);
			return NULL;
		}
	}

	return ref_field;
}

static bool is_var_ref(char *var_name)
{
	if (!var_name || strlen(var_name) < 2 || var_name[0] != '$')
		return false;

	return true;
}

static char *field_name_from_var(struct hist_trigger_data *hist_data,
				 char *var_name)
{
	char *name, *field;
	unsigned int i;

	for (i = 0; i < hist_data->attrs->var_defs.n_vars; i++) {
		name = hist_data->attrs->var_defs.name[i];

		if (strcmp(var_name, name) == 0) {
			field = hist_data->attrs->var_defs.expr[i];
			if (contains_operator(field) || is_var_ref(field))
				continue;
			return field;
		}
	}

	return NULL;
}

static char *local_field_var_ref(struct hist_trigger_data *hist_data,
				 char *system, char *event_name,
				 char *var_name)
{
	struct trace_event_call *call;

	if (system && event_name) {
		call = hist_data->event_file->event_call;

		if (strcmp(system, call->class->system) != 0)
			return NULL;

		if (strcmp(event_name, trace_event_name(call)) != 0)
			return NULL;
	}

	if (!!system != !!event_name)
		return NULL;

	if (!is_var_ref(var_name))
		return NULL;

	var_name++;

	return field_name_from_var(hist_data, var_name);
}

static struct hist_field *parse_var_ref(struct hist_trigger_data *hist_data,
					char *system, char *event_name,
					char *var_name)
{
	struct hist_field *var_field = NULL, *ref_field = NULL;

	if (!is_var_ref(var_name))
		return NULL;

	var_name++;

	var_field = find_event_var(hist_data, system, event_name, var_name);
	if (var_field)
		ref_field = create_var_ref(var_field, system, event_name);

	if (!ref_field)
		hist_err_event("Couldn't find variable: $",
			       system, event_name, var_name);

	return ref_field;
}

static struct ftrace_event_field *
parse_field(struct hist_trigger_data *hist_data, struct trace_event_file *file,
	    char *field_str, unsigned long *flags)
{
	struct ftrace_event_field *field = NULL;
	char *field_name, *modifier, *str;

	modifier = str = kstrdup(field_str, GFP_KERNEL);
	if (!modifier)
		return ERR_PTR(-ENOMEM);

	field_name = strsep(&modifier, ".");
	if (modifier) {
		if (strcmp(modifier, "hex") == 0)
			*flags |= HIST_FIELD_FL_HEX;
		else if (strcmp(modifier, "sym") == 0)
			*flags |= HIST_FIELD_FL_SYM;
		else if (strcmp(modifier, "sym-offset") == 0)
			*flags |= HIST_FIELD_FL_SYM_OFFSET;
		else if ((strcmp(modifier, "execname") == 0) &&
			 (strcmp(field_name, "common_pid") == 0))
			*flags |= HIST_FIELD_FL_EXECNAME;
		else if (strcmp(modifier, "syscall") == 0)
			*flags |= HIST_FIELD_FL_SYSCALL;
		else if (strcmp(modifier, "log2") == 0)
			*flags |= HIST_FIELD_FL_LOG2;
		else if (strcmp(modifier, "usecs") == 0)
			*flags |= HIST_FIELD_FL_TIMESTAMP_USECS;
		else {
			hist_err("Invalid field modifier: ", modifier);
			field = ERR_PTR(-EINVAL);
			goto out;
		}
	}

	if (strcmp(field_name, "common_timestamp") == 0) {
		*flags |= HIST_FIELD_FL_TIMESTAMP;
		hist_data->enable_timestamps = true;
		if (*flags & HIST_FIELD_FL_TIMESTAMP_USECS)
			hist_data->attrs->ts_in_usecs = true;
	} else if (strcmp(field_name, "common_cpu") == 0)
		*flags |= HIST_FIELD_FL_CPU;
	else {
		field = trace_find_event_field(file->event_call, field_name);
		if (!field || !field->size) {
			/*
			 * For backward compatibility, if field_name
			 * was "cpu", then we treat this the same as
			 * common_cpu.
			 */
			if (strcmp(field_name, "cpu") == 0) {
				*flags |= HIST_FIELD_FL_CPU;
			} else {
				hist_err("Couldn't find field: ", field_name);
				field = ERR_PTR(-EINVAL);
				goto out;
			}
		}
	}
 out:
	kfree(str);

	return field;
}

static struct hist_field *create_alias(struct hist_trigger_data *hist_data,
				       struct hist_field *var_ref,
				       char *var_name)
{
	struct hist_field *alias = NULL;
	unsigned long flags = HIST_FIELD_FL_ALIAS | HIST_FIELD_FL_VAR;

	alias = create_hist_field(hist_data, NULL, flags, var_name);
	if (!alias)
		return NULL;

	alias->fn = var_ref->fn;
	alias->operands[0] = var_ref;

	if (init_var_ref(alias, var_ref, var_ref->system, var_ref->event_name)) {
		destroy_hist_field(alias, 0);
		return NULL;
	}

	alias->var_ref_idx = var_ref->var_ref_idx;

	return alias;
}

static struct hist_field *parse_atom(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file, char *str,
				     unsigned long *flags, char *var_name)
{
	char *s, *ref_system = NULL, *ref_event = NULL, *ref_var = str;
	struct ftrace_event_field *field = NULL;
	struct hist_field *hist_field = NULL;
	int ret = 0;

	s = strchr(str, '.');
	if (s) {
		s = strchr(++s, '.');
		if (s) {
			ref_system = strsep(&str, ".");
			if (!str) {
				ret = -EINVAL;
				goto out;
			}
			ref_event = strsep(&str, ".");
			if (!str) {
				ret = -EINVAL;
				goto out;
			}
			ref_var = str;
		}
	}

	s = local_field_var_ref(hist_data, ref_system, ref_event, ref_var);
	if (!s) {
		hist_field = parse_var_ref(hist_data, ref_system, ref_event, ref_var);
		if (hist_field) {
			hist_data->var_refs[hist_data->n_var_refs] = hist_field;
			hist_field->var_ref_idx = hist_data->n_var_refs++;
			if (var_name) {
				hist_field = create_alias(hist_data, hist_field, var_name);
				if (!hist_field) {
					ret = -ENOMEM;
					goto out;
				}
			}
			return hist_field;
		}
	} else
		str = s;

	field = parse_field(hist_data, file, str, flags);
	if (IS_ERR(field)) {
		ret = PTR_ERR(field);
		goto out;
	}

	hist_field = create_hist_field(hist_data, field, *flags, var_name);
	if (!hist_field) {
		ret = -ENOMEM;
		goto out;
	}

	return hist_field;
 out:
	return ERR_PTR(ret);
}

static struct hist_field *parse_expr(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file,
				     char *str, unsigned long flags,
				     char *var_name, unsigned int level);

static struct hist_field *parse_unary(struct hist_trigger_data *hist_data,
				      struct trace_event_file *file,
				      char *str, unsigned long flags,
				      char *var_name, unsigned int level)
{
	struct hist_field *operand1, *expr = NULL;
	unsigned long operand_flags;
	int ret = 0;
	char *s;

	/* we support only -(xxx) i.e. explicit parens required */

	if (level > 3) {
		hist_err("Too many subexpressions (3 max): ", str);
		ret = -EINVAL;
		goto free;
	}

	str++; /* skip leading '-' */

	s = strchr(str, '(');
	if (s)
		str++;
	else {
		ret = -EINVAL;
		goto free;
	}

	s = strrchr(str, ')');
	if (s)
		*s = '\0';
	else {
		ret = -EINVAL; /* no closing ')' */
		goto free;
	}

	flags |= HIST_FIELD_FL_EXPR;
	expr = create_hist_field(hist_data, NULL, flags, var_name);
	if (!expr) {
		ret = -ENOMEM;
		goto free;
	}

	operand_flags = 0;
	operand1 = parse_expr(hist_data, file, str, operand_flags, NULL, ++level);
	if (IS_ERR(operand1)) {
		ret = PTR_ERR(operand1);
		goto free;
	}
	if (operand1->flags & HIST_FIELD_FL_STRING) {
		/* String type can not be the operand of unary operator. */
		destroy_hist_field(operand1, 0);
		ret = -EINVAL;
		goto free;
	}

	expr->flags |= operand1->flags &
		(HIST_FIELD_FL_TIMESTAMP | HIST_FIELD_FL_TIMESTAMP_USECS);
	expr->fn = hist_field_unary_minus;
	expr->operands[0] = operand1;
	expr->operator = FIELD_OP_UNARY_MINUS;
	expr->name = expr_str(expr, 0);
	expr->type = kstrdup(operand1->type, GFP_KERNEL);
	if (!expr->type) {
		ret = -ENOMEM;
		goto free;
	}

	return expr;
 free:
	destroy_hist_field(expr, 0);
	return ERR_PTR(ret);
}

static int check_expr_operands(struct hist_field *operand1,
			       struct hist_field *operand2)
{
	unsigned long operand1_flags = operand1->flags;
	unsigned long operand2_flags = operand2->flags;

	if ((operand1_flags & HIST_FIELD_FL_VAR_REF) ||
	    (operand1_flags & HIST_FIELD_FL_ALIAS)) {
		struct hist_field *var;

		var = find_var_field(operand1->var.hist_data, operand1->name);
		if (!var)
			return -EINVAL;
		operand1_flags = var->flags;
	}

	if ((operand2_flags & HIST_FIELD_FL_VAR_REF) ||
	    (operand2_flags & HIST_FIELD_FL_ALIAS)) {
		struct hist_field *var;

		var = find_var_field(operand2->var.hist_data, operand2->name);
		if (!var)
			return -EINVAL;
		operand2_flags = var->flags;
	}

	if ((operand1_flags & HIST_FIELD_FL_TIMESTAMP_USECS) !=
	    (operand2_flags & HIST_FIELD_FL_TIMESTAMP_USECS)) {
		hist_err("Timestamp units in expression don't match", NULL);
		return -EINVAL;
	}

	return 0;
}

static struct hist_field *parse_expr(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file,
				     char *str, unsigned long flags,
				     char *var_name, unsigned int level)
{
	struct hist_field *operand1 = NULL, *operand2 = NULL, *expr = NULL;
	unsigned long operand_flags;
	int field_op, ret = -EINVAL;
	char *sep, *operand1_str;

	if (level > 3) {
		hist_err("Too many subexpressions (3 max): ", str);
		return ERR_PTR(-EINVAL);
	}

	field_op = contains_operator(str);

	if (field_op == FIELD_OP_NONE)
		return parse_atom(hist_data, file, str, &flags, var_name);

	if (field_op == FIELD_OP_UNARY_MINUS)
		return parse_unary(hist_data, file, str, flags, var_name, ++level);

	switch (field_op) {
	case FIELD_OP_MINUS:
		sep = "-";
		break;
	case FIELD_OP_PLUS:
		sep = "+";
		break;
	default:
		goto free;
	}

	operand1_str = strsep(&str, sep);
	if (!operand1_str || !str)
		goto free;

	operand_flags = 0;
	operand1 = parse_atom(hist_data, file, operand1_str,
			      &operand_flags, NULL);
	if (IS_ERR(operand1)) {
		ret = PTR_ERR(operand1);
		operand1 = NULL;
		goto free;
	}
	if (operand1->flags & HIST_FIELD_FL_STRING) {
		ret = -EINVAL;
		goto free;
	}

	/* rest of string could be another expression e.g. b+c in a+b+c */
	operand_flags = 0;
	operand2 = parse_expr(hist_data, file, str, operand_flags, NULL, ++level);
	if (IS_ERR(operand2)) {
		ret = PTR_ERR(operand2);
		operand2 = NULL;
		goto free;
	}
	if (operand2->flags & HIST_FIELD_FL_STRING) {
		ret = -EINVAL;
		goto free;
	}

	ret = check_expr_operands(operand1, operand2);
	if (ret)
		goto free;

	flags |= HIST_FIELD_FL_EXPR;

	flags |= operand1->flags &
		(HIST_FIELD_FL_TIMESTAMP | HIST_FIELD_FL_TIMESTAMP_USECS);

	expr = create_hist_field(hist_data, NULL, flags, var_name);
	if (!expr) {
		ret = -ENOMEM;
		goto free;
	}

	operand1->read_once = true;
	operand2->read_once = true;

	expr->operands[0] = operand1;
	expr->operands[1] = operand2;

	/* The operand sizes should be the same, so just pick one */
	expr->size = operand1->size;

	expr->operator = field_op;
	expr->name = expr_str(expr, 0);
	expr->type = kstrdup(operand1->type, GFP_KERNEL);
	if (!expr->type) {
		ret = -ENOMEM;
		goto free;
	}

	switch (field_op) {
	case FIELD_OP_MINUS:
		expr->fn = hist_field_minus;
		break;
	case FIELD_OP_PLUS:
		expr->fn = hist_field_plus;
		break;
	default:
		ret = -EINVAL;
		goto free;
	}

	return expr;
 free:
	destroy_hist_field(operand1, 0);
	destroy_hist_field(operand2, 0);
	destroy_hist_field(expr, 0);

	return ERR_PTR(ret);
}

static char *find_trigger_filter(struct hist_trigger_data *hist_data,
				 struct trace_event_file *file)
{
	struct event_trigger_data *test;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (test->private_data == hist_data)
				return test->filter_str;
		}
	}

	return NULL;
}

static struct event_command trigger_hist_cmd;
static int event_hist_trigger_func(struct event_command *cmd_ops,
				   struct trace_event_file *file,
				   char *glob, char *cmd, char *param);

static bool compatible_keys(struct hist_trigger_data *target_hist_data,
			    struct hist_trigger_data *hist_data,
			    unsigned int n_keys)
{
	struct hist_field *target_hist_field, *hist_field;
	unsigned int n, i, j;

	if (hist_data->n_fields - hist_data->n_vals != n_keys)
		return false;

	i = hist_data->n_vals;
	j = target_hist_data->n_vals;

	for (n = 0; n < n_keys; n++) {
		hist_field = hist_data->fields[i + n];
		target_hist_field = target_hist_data->fields[j + n];

		if (strcmp(hist_field->type, target_hist_field->type) != 0)
			return false;
		if (hist_field->size != target_hist_field->size)
			return false;
		if (hist_field->is_signed != target_hist_field->is_signed)
			return false;
	}

	return true;
}

static struct hist_trigger_data *
find_compatible_hist(struct hist_trigger_data *target_hist_data,
		     struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data;
	struct event_trigger_data *test;
	unsigned int n_keys;

	n_keys = target_hist_data->n_fields - target_hist_data->n_vals;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			hist_data = test->private_data;

			if (compatible_keys(target_hist_data, hist_data, n_keys))
				return hist_data;
		}
	}

	return NULL;
}

static struct trace_event_file *event_file(struct trace_array *tr,
					   char *system, char *event_name)
{
	struct trace_event_file *file;

	file = __find_event_file(tr, system, event_name);
	if (!file)
		return ERR_PTR(-EINVAL);

	return file;
}

static struct hist_field *
find_synthetic_field_var(struct hist_trigger_data *target_hist_data,
			 char *system, char *event_name, char *field_name)
{
	struct hist_field *event_var;
	char *synthetic_name;

	synthetic_name = kzalloc(MAX_FILTER_STR_VAL, GFP_KERNEL);
	if (!synthetic_name)
		return ERR_PTR(-ENOMEM);

	strcpy(synthetic_name, "synthetic_");
	strcat(synthetic_name, field_name);

	event_var = find_event_var(target_hist_data, system, event_name, synthetic_name);

	kfree(synthetic_name);

	return event_var;
}

/**
 * create_field_var_hist - Automatically create a histogram and var for a field
 * @target_hist_data: The target hist trigger
 * @subsys_name: Optional subsystem name
 * @event_name: Optional event name
 * @field_name: The name of the field (and the resulting variable)
 *
 * Hist trigger actions fetch data from variables, not directly from
 * events.  However, for convenience, users are allowed to directly
 * specify an event field in an action, which will be automatically
 * converted into a variable on their behalf.

 * If a user specifies a field on an event that isn't the event the
 * histogram currently being defined (the target event histogram), the
 * only way that can be accomplished is if a new hist trigger is
 * created and the field variable defined on that.
 *
 * This function creates a new histogram compatible with the target
 * event (meaning a histogram with the same key as the target
 * histogram), and creates a variable for the specified field, but
 * with 'synthetic_' prepended to the variable name in order to avoid
 * collision with normal field variables.
 *
 * Return: The variable created for the field.
 */
static struct hist_field *
create_field_var_hist(struct hist_trigger_data *target_hist_data,
		      char *subsys_name, char *event_name, char *field_name)
{
	struct trace_array *tr = target_hist_data->event_file->tr;
	struct hist_field *event_var = ERR_PTR(-EINVAL);
	struct hist_trigger_data *hist_data;
	unsigned int i, n, first = true;
	struct field_var_hist *var_hist;
	struct trace_event_file *file;
	struct hist_field *key_field;
	char *saved_filter;
	char *cmd;
	int ret;

	if (target_hist_data->n_field_var_hists >= SYNTH_FIELDS_MAX) {
		hist_err_event("onmatch: Too many field variables defined: ",
			       subsys_name, event_name, field_name);
		return ERR_PTR(-EINVAL);
	}

	file = event_file(tr, subsys_name, event_name);

	if (IS_ERR(file)) {
		hist_err_event("onmatch: Event file not found: ",
			       subsys_name, event_name, field_name);
		ret = PTR_ERR(file);
		return ERR_PTR(ret);
	}

	/*
	 * Look for a histogram compatible with target.  We'll use the
	 * found histogram specification to create a new matching
	 * histogram with our variable on it.  target_hist_data is not
	 * yet a registered histogram so we can't use that.
	 */
	hist_data = find_compatible_hist(target_hist_data, file);
	if (!hist_data) {
		hist_err_event("onmatch: Matching event histogram not found: ",
			       subsys_name, event_name, field_name);
		return ERR_PTR(-EINVAL);
	}

	/* See if a synthetic field variable has already been created */
	event_var = find_synthetic_field_var(target_hist_data, subsys_name,
					     event_name, field_name);
	if (!IS_ERR_OR_NULL(event_var))
		return event_var;

	var_hist = kzalloc(sizeof(*var_hist), GFP_KERNEL);
	if (!var_hist)
		return ERR_PTR(-ENOMEM);

	cmd = kzalloc(MAX_FILTER_STR_VAL, GFP_KERNEL);
	if (!cmd) {
		kfree(var_hist);
		return ERR_PTR(-ENOMEM);
	}

	/* Use the same keys as the compatible histogram */
	strcat(cmd, "keys=");

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];
		if (!first)
			strcat(cmd, ",");
		strcat(cmd, key_field->field->name);
		first = false;
	}

	/* Create the synthetic field variable specification */
	strcat(cmd, ":synthetic_");
	strcat(cmd, field_name);
	strcat(cmd, "=");
	strcat(cmd, field_name);

	/* Use the same filter as the compatible histogram */
	saved_filter = find_trigger_filter(hist_data, file);
	if (saved_filter) {
		strcat(cmd, " if ");
		strcat(cmd, saved_filter);
	}

	var_hist->cmd = kstrdup(cmd, GFP_KERNEL);
	if (!var_hist->cmd) {
		kfree(cmd);
		kfree(var_hist);
		return ERR_PTR(-ENOMEM);
	}

	/* Save the compatible histogram information */
	var_hist->hist_data = hist_data;

	/* Create the new histogram with our variable */
	ret = event_hist_trigger_func(&trigger_hist_cmd, file,
				      "", "hist", cmd);
	if (ret) {
		kfree(cmd);
		kfree(var_hist->cmd);
		kfree(var_hist);
		hist_err_event("onmatch: Couldn't create histogram for field: ",
			       subsys_name, event_name, field_name);
		return ERR_PTR(ret);
	}

	kfree(cmd);

	/* If we can't find the variable, something went wrong */
	event_var = find_synthetic_field_var(target_hist_data, subsys_name,
					     event_name, field_name);
	if (IS_ERR_OR_NULL(event_var)) {
		kfree(var_hist->cmd);
		kfree(var_hist);
		hist_err_event("onmatch: Couldn't find synthetic variable: ",
			       subsys_name, event_name, field_name);
		return ERR_PTR(-EINVAL);
	}

	n = target_hist_data->n_field_var_hists;
	target_hist_data->field_var_hists[n] = var_hist;
	target_hist_data->n_field_var_hists++;

	return event_var;
}

static struct hist_field *
find_target_event_var(struct hist_trigger_data *hist_data,
		      char *subsys_name, char *event_name, char *var_name)
{
	struct trace_event_file *file = hist_data->event_file;
	struct hist_field *hist_field = NULL;

	if (subsys_name) {
		struct trace_event_call *call;

		if (!event_name)
			return NULL;

		call = file->event_call;

		if (strcmp(subsys_name, call->class->system) != 0)
			return NULL;

		if (strcmp(event_name, trace_event_name(call)) != 0)
			return NULL;
	}

	hist_field = find_var_field(hist_data, var_name);

	return hist_field;
}

static inline void __update_field_vars(struct tracing_map_elt *elt,
				       struct ring_buffer_event *rbe,
				       void *rec,
				       struct field_var **field_vars,
				       unsigned int n_field_vars,
				       unsigned int field_var_str_start)
{
	struct hist_elt_data *elt_data = elt->private_data;
	unsigned int i, j, var_idx;
	u64 var_val;

	for (i = 0, j = field_var_str_start; i < n_field_vars; i++) {
		struct field_var *field_var = field_vars[i];
		struct hist_field *var = field_var->var;
		struct hist_field *val = field_var->val;

		var_val = val->fn(val, elt, rbe, rec);
		var_idx = var->var.idx;

		if (val->flags & HIST_FIELD_FL_STRING) {
			char *str = elt_data->field_var_str[j++];
			char *val_str = (char *)(uintptr_t)var_val;

			strscpy(str, val_str, STR_VAR_LEN_MAX);
			var_val = (u64)(uintptr_t)str;
		}
		tracing_map_set_var(elt, var_idx, var_val);
	}
}

static void update_field_vars(struct hist_trigger_data *hist_data,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *rec)
{
	__update_field_vars(elt, rbe, rec, hist_data->field_vars,
			    hist_data->n_field_vars, 0);
}

static void update_max_vars(struct hist_trigger_data *hist_data,
			    struct tracing_map_elt *elt,
			    struct ring_buffer_event *rbe,
			    void *rec)
{
	__update_field_vars(elt, rbe, rec, hist_data->max_vars,
			    hist_data->n_max_vars, hist_data->n_field_var_str);
}

static struct hist_field *create_var(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file,
				     char *name, int size, const char *type)
{
	struct hist_field *var;
	int idx;

	if (find_var(hist_data, file, name) && !hist_data->remove) {
		var = ERR_PTR(-EINVAL);
		goto out;
	}

	var = kzalloc(sizeof(struct hist_field), GFP_KERNEL);
	if (!var) {
		var = ERR_PTR(-ENOMEM);
		goto out;
	}

	idx = tracing_map_add_var(hist_data->map);
	if (idx < 0) {
		kfree(var);
		var = ERR_PTR(-EINVAL);
		goto out;
	}

	var->flags = HIST_FIELD_FL_VAR;
	var->var.idx = idx;
	var->var.hist_data = var->hist_data = hist_data;
	var->size = size;
	var->var.name = kstrdup(name, GFP_KERNEL);
	var->type = kstrdup(type, GFP_KERNEL);
	if (!var->var.name || !var->type) {
		kfree(var->var.name);
		kfree(var->type);
		kfree(var);
		var = ERR_PTR(-ENOMEM);
	}
 out:
	return var;
}

static struct field_var *create_field_var(struct hist_trigger_data *hist_data,
					  struct trace_event_file *file,
					  char *field_name)
{
	struct hist_field *val = NULL, *var = NULL;
	unsigned long flags = HIST_FIELD_FL_VAR;
	struct field_var *field_var;
	int ret = 0;

	if (hist_data->n_field_vars >= SYNTH_FIELDS_MAX) {
		hist_err("Too many field variables defined: ", field_name);
		ret = -EINVAL;
		goto err;
	}

	val = parse_atom(hist_data, file, field_name, &flags, NULL);
	if (IS_ERR(val)) {
		hist_err("Couldn't parse field variable: ", field_name);
		ret = PTR_ERR(val);
		goto err;
	}

	var = create_var(hist_data, file, field_name, val->size, val->type);
	if (IS_ERR(var)) {
		hist_err("Couldn't create or find variable: ", field_name);
		kfree(val);
		ret = PTR_ERR(var);
		goto err;
	}

	field_var = kzalloc(sizeof(struct field_var), GFP_KERNEL);
	if (!field_var) {
		kfree(val);
		kfree(var);
		ret =  -ENOMEM;
		goto err;
	}

	field_var->var = var;
	field_var->val = val;
 out:
	return field_var;
 err:
	field_var = ERR_PTR(ret);
	goto out;
}

/**
 * create_target_field_var - Automatically create a variable for a field
 * @target_hist_data: The target hist trigger
 * @subsys_name: Optional subsystem name
 * @event_name: Optional event name
 * @var_name: The name of the field (and the resulting variable)
 *
 * Hist trigger actions fetch data from variables, not directly from
 * events.  However, for convenience, users are allowed to directly
 * specify an event field in an action, which will be automatically
 * converted into a variable on their behalf.

 * This function creates a field variable with the name var_name on
 * the hist trigger currently being defined on the target event.  If
 * subsys_name and event_name are specified, this function simply
 * verifies that they do in fact match the target event subsystem and
 * event name.
 *
 * Return: The variable created for the field.
 */
static struct field_var *
create_target_field_var(struct hist_trigger_data *target_hist_data,
			char *subsys_name, char *event_name, char *var_name)
{
	struct trace_event_file *file = target_hist_data->event_file;

	if (subsys_name) {
		struct trace_event_call *call;

		if (!event_name)
			return NULL;

		call = file->event_call;

		if (strcmp(subsys_name, call->class->system) != 0)
			return NULL;

		if (strcmp(event_name, trace_event_name(call)) != 0)
			return NULL;
	}

	return create_field_var(target_hist_data, file, var_name);
}

static void onmax_print(struct seq_file *m,
			struct hist_trigger_data *hist_data,
			struct tracing_map_elt *elt,
			struct action_data *data)
{
	unsigned int i, save_var_idx, max_idx = data->onmax.max_var->var.idx;

	seq_printf(m, "\n\tmax: %10llu", tracing_map_read_var(elt, max_idx));

	for (i = 0; i < hist_data->n_max_vars; i++) {
		struct hist_field *save_val = hist_data->max_vars[i]->val;
		struct hist_field *save_var = hist_data->max_vars[i]->var;
		u64 val;

		save_var_idx = save_var->var.idx;

		val = tracing_map_read_var(elt, save_var_idx);

		if (save_val->flags & HIST_FIELD_FL_STRING) {
			seq_printf(m, "  %s: %-32s", save_var->var.name,
				   (char *)(uintptr_t)(val));
		} else
			seq_printf(m, "  %s: %10llu", save_var->var.name, val);
	}
}

static void onmax_save(struct hist_trigger_data *hist_data,
		       struct tracing_map_elt *elt, void *rec,
		       struct ring_buffer_event *rbe,
		       struct action_data *data, u64 *var_ref_vals)
{
	unsigned int max_idx = data->onmax.max_var->var.idx;
	unsigned int max_var_ref_idx = data->onmax.max_var_ref_idx;

	u64 var_val, max_val;

	var_val = var_ref_vals[max_var_ref_idx];
	max_val = tracing_map_read_var(elt, max_idx);

	if (var_val <= max_val)
		return;

	tracing_map_set_var(elt, max_idx, var_val);

	update_max_vars(hist_data, elt, rbe, rec);
}

static void onmax_destroy(struct action_data *data)
{
	unsigned int i;

	destroy_hist_field(data->onmax.max_var, 0);
	destroy_hist_field(data->onmax.var, 0);

	kfree(data->onmax.var_str);
	kfree(data->onmax.fn_name);

	for (i = 0; i < data->n_params; i++)
		kfree(data->params[i]);

	kfree(data);
}

static int onmax_create(struct hist_trigger_data *hist_data,
			struct action_data *data)
{
	struct trace_event_file *file = hist_data->event_file;
	struct hist_field *var_field, *ref_field, *max_var;
	unsigned int var_ref_idx = hist_data->n_var_refs;
	struct field_var *field_var;
	char *onmax_var_str, *param;
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	onmax_var_str = data->onmax.var_str;
	if (onmax_var_str[0] != '$') {
		hist_err("onmax: For onmax(x), x must be a variable: ", onmax_var_str);
		return -EINVAL;
	}
	onmax_var_str++;

	var_field = find_target_event_var(hist_data, NULL, NULL, onmax_var_str);
	if (!var_field) {
		hist_err("onmax: Couldn't find onmax variable: ", onmax_var_str);
		return -EINVAL;
	}

	flags = HIST_FIELD_FL_VAR_REF;
	ref_field = create_hist_field(hist_data, NULL, flags, NULL);
	if (!ref_field)
		return -ENOMEM;

	if (init_var_ref(ref_field, var_field, NULL, NULL)) {
		destroy_hist_field(ref_field, 0);
		ret = -ENOMEM;
		goto out;
	}
	hist_data->var_refs[hist_data->n_var_refs] = ref_field;
	ref_field->var_ref_idx = hist_data->n_var_refs++;
	data->onmax.var = ref_field;

	data->fn = onmax_save;
	data->onmax.max_var_ref_idx = var_ref_idx;
	max_var = create_var(hist_data, file, "max", sizeof(u64), "u64");
	if (IS_ERR(max_var)) {
		hist_err("onmax: Couldn't create onmax variable: ", "max");
		ret = PTR_ERR(max_var);
		goto out;
	}
	data->onmax.max_var = max_var;

	for (i = 0; i < data->n_params; i++) {
		param = kstrdup(data->params[i], GFP_KERNEL);
		if (!param) {
			ret = -ENOMEM;
			goto out;
		}

		field_var = create_target_field_var(hist_data, NULL, NULL, param);
		if (IS_ERR(field_var)) {
			hist_err("onmax: Couldn't create field variable: ", param);
			ret = PTR_ERR(field_var);
			kfree(param);
			goto out;
		}

		hist_data->max_vars[hist_data->n_max_vars++] = field_var;
		if (field_var->val->flags & HIST_FIELD_FL_STRING)
			hist_data->n_max_var_str++;

		kfree(param);
	}
 out:
	return ret;
}

static int parse_action_params(char *params, struct action_data *data)
{
	char *param, *saved_param;
	int ret = 0;

	while (params) {
		if (data->n_params >= SYNTH_FIELDS_MAX)
			goto out;

		param = strsep(&params, ",");
		if (!param) {
			ret = -EINVAL;
			goto out;
		}

		param = strstrip(param);
		if (strlen(param) < 2) {
			hist_err("Invalid action param: ", param);
			ret = -EINVAL;
			goto out;
		}

		saved_param = kstrdup(param, GFP_KERNEL);
		if (!saved_param) {
			ret = -ENOMEM;
			goto out;
		}

		data->params[data->n_params++] = saved_param;
	}
 out:
	return ret;
}

static struct action_data *onmax_parse(char *str)
{
	char *onmax_fn_name, *onmax_var_str;
	struct action_data *data;
	int ret = -EINVAL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	onmax_var_str = strsep(&str, ")");
	if (!onmax_var_str || !str) {
		ret = -EINVAL;
		goto free;
	}

	data->onmax.var_str = kstrdup(onmax_var_str, GFP_KERNEL);
	if (!data->onmax.var_str) {
		ret = -ENOMEM;
		goto free;
	}

	strsep(&str, ".");
	if (!str)
		goto free;

	onmax_fn_name = strsep(&str, "(");
	if (!onmax_fn_name || !str)
		goto free;

	if (strncmp(onmax_fn_name, "save", strlen("save")) == 0) {
		char *params = strsep(&str, ")");

		if (!params) {
			ret = -EINVAL;
			goto free;
		}

		ret = parse_action_params(params, data);
		if (ret)
			goto free;
	} else
		goto free;

	data->onmax.fn_name = kstrdup(onmax_fn_name, GFP_KERNEL);
	if (!data->onmax.fn_name) {
		ret = -ENOMEM;
		goto free;
	}
 out:
	return data;
 free:
	onmax_destroy(data);
	data = ERR_PTR(ret);
	goto out;
}

static void onmatch_destroy(struct action_data *data)
{
	unsigned int i;

	mutex_lock(&synth_event_mutex);

	kfree(data->onmatch.match_event);
	kfree(data->onmatch.match_event_system);
	kfree(data->onmatch.synth_event_name);

	for (i = 0; i < data->n_params; i++)
		kfree(data->params[i]);

	if (data->onmatch.synth_event)
		data->onmatch.synth_event->ref--;

	kfree(data);

	mutex_unlock(&synth_event_mutex);
}

static void destroy_field_var(struct field_var *field_var)
{
	if (!field_var)
		return;

	destroy_hist_field(field_var->var, 0);
	destroy_hist_field(field_var->val, 0);

	kfree(field_var);
}

static void destroy_field_vars(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_field_vars; i++)
		destroy_field_var(hist_data->field_vars[i]);
}

static void save_field_var(struct hist_trigger_data *hist_data,
			   struct field_var *field_var)
{
	hist_data->field_vars[hist_data->n_field_vars++] = field_var;

	if (field_var->val->flags & HIST_FIELD_FL_STRING)
		hist_data->n_field_var_str++;
}


static void destroy_synth_var_refs(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_synth_var_refs; i++)
		destroy_hist_field(hist_data->synth_var_refs[i], 0);
}

static void save_synth_var_ref(struct hist_trigger_data *hist_data,
			 struct hist_field *var_ref)
{
	hist_data->synth_var_refs[hist_data->n_synth_var_refs++] = var_ref;

	hist_data->var_refs[hist_data->n_var_refs] = var_ref;
	var_ref->var_ref_idx = hist_data->n_var_refs++;
}

static int check_synth_field(struct synth_event *event,
			     struct hist_field *hist_field,
			     unsigned int field_pos)
{
	struct synth_field *field;

	if (field_pos >= event->n_fields)
		return -EINVAL;

	field = event->fields[field_pos];

	if (strcmp(field->type, hist_field->type) != 0)
		return -EINVAL;

	return 0;
}

static struct hist_field *
onmatch_find_var(struct hist_trigger_data *hist_data, struct action_data *data,
		 char *system, char *event, char *var)
{
	struct hist_field *hist_field;

	var++; /* skip '$' */

	hist_field = find_target_event_var(hist_data, system, event, var);
	if (!hist_field) {
		if (!system) {
			system = data->onmatch.match_event_system;
			event = data->onmatch.match_event;
		}

		hist_field = find_event_var(hist_data, system, event, var);
	}

	if (!hist_field)
		hist_err_event("onmatch: Couldn't find onmatch param: $", system, event, var);

	return hist_field;
}

static struct hist_field *
onmatch_create_field_var(struct hist_trigger_data *hist_data,
			 struct action_data *data, char *system,
			 char *event, char *var)
{
	struct hist_field *hist_field = NULL;
	struct field_var *field_var;

	/*
	 * First try to create a field var on the target event (the
	 * currently being defined).  This will create a variable for
	 * unqualified fields on the target event, or if qualified,
	 * target fields that have qualified names matching the target.
	 */
	field_var = create_target_field_var(hist_data, system, event, var);

	if (field_var && !IS_ERR(field_var)) {
		save_field_var(hist_data, field_var);
		hist_field = field_var->var;
	} else {
		field_var = NULL;
		/*
		 * If no explicit system.event is specfied, default to
		 * looking for fields on the onmatch(system.event.xxx)
		 * event.
		 */
		if (!system) {
			system = data->onmatch.match_event_system;
			event = data->onmatch.match_event;
		}

		if (!event)
			goto free;
		/*
		 * At this point, we're looking at a field on another
		 * event.  Because we can't modify a hist trigger on
		 * another event to add a variable for a field, we need
		 * to create a new trigger on that event and create the
		 * variable at the same time.
		 */
		hist_field = create_field_var_hist(hist_data, system, event, var);
		if (IS_ERR(hist_field))
			goto free;
	}
 out:
	return hist_field;
 free:
	destroy_field_var(field_var);
	hist_field = NULL;
	goto out;
}

static int onmatch_create(struct hist_trigger_data *hist_data,
			  struct trace_event_file *file,
			  struct action_data *data)
{
	char *event_name, *param, *system = NULL;
	struct hist_field *hist_field, *var_ref;
	unsigned int i, var_ref_idx;
	unsigned int field_pos = 0;
	struct synth_event *event;
	int ret = 0;

	mutex_lock(&synth_event_mutex);
	event = find_synth_event(data->onmatch.synth_event_name);
	if (!event) {
		hist_err("onmatch: Couldn't find synthetic event: ", data->onmatch.synth_event_name);
		mutex_unlock(&synth_event_mutex);
		return -EINVAL;
	}
	event->ref++;
	mutex_unlock(&synth_event_mutex);

	var_ref_idx = hist_data->n_var_refs;

	for (i = 0; i < data->n_params; i++) {
		char *p;

		p = param = kstrdup(data->params[i], GFP_KERNEL);
		if (!param) {
			ret = -ENOMEM;
			goto err;
		}

		system = strsep(&param, ".");
		if (!param) {
			param = (char *)system;
			system = event_name = NULL;
		} else {
			event_name = strsep(&param, ".");
			if (!param) {
				kfree(p);
				ret = -EINVAL;
				goto err;
			}
		}

		if (param[0] == '$')
			hist_field = onmatch_find_var(hist_data, data, system,
						      event_name, param);
		else
			hist_field = onmatch_create_field_var(hist_data, data,
							      system,
							      event_name,
							      param);

		if (!hist_field) {
			kfree(p);
			ret = -EINVAL;
			goto err;
		}

		if (check_synth_field(event, hist_field, field_pos) == 0) {
			var_ref = create_var_ref(hist_field, system, event_name);
			if (!var_ref) {
				kfree(p);
				ret = -ENOMEM;
				goto err;
			}

			save_synth_var_ref(hist_data, var_ref);
			field_pos++;
			kfree(p);
			continue;
		}

		hist_err_event("onmatch: Param type doesn't match synthetic event field type: ",
			       system, event_name, param);
		kfree(p);
		ret = -EINVAL;
		goto err;
	}

	if (field_pos != event->n_fields) {
		hist_err("onmatch: Param count doesn't match synthetic event field count: ", event->name);
		ret = -EINVAL;
		goto err;
	}

	data->fn = action_trace;
	data->onmatch.synth_event = event;
	data->onmatch.var_ref_idx = var_ref_idx;
 out:
	return ret;
 err:
	mutex_lock(&synth_event_mutex);
	event->ref--;
	mutex_unlock(&synth_event_mutex);

	goto out;
}

static struct action_data *onmatch_parse(struct trace_array *tr, char *str)
{
	char *match_event, *match_event_system;
	char *synth_event_name, *params;
	struct action_data *data;
	int ret = -EINVAL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	match_event = strsep(&str, ")");
	if (!match_event || !str) {
		hist_err("onmatch: Missing closing paren: ", match_event);
		goto free;
	}

	match_event_system = strsep(&match_event, ".");
	if (!match_event) {
		hist_err("onmatch: Missing subsystem for match event: ", match_event_system);
		goto free;
	}

	if (IS_ERR(event_file(tr, match_event_system, match_event))) {
		hist_err_event("onmatch: Invalid subsystem or event name: ",
			       match_event_system, match_event, NULL);
		goto free;
	}

	data->onmatch.match_event = kstrdup(match_event, GFP_KERNEL);
	if (!data->onmatch.match_event) {
		ret = -ENOMEM;
		goto free;
	}

	data->onmatch.match_event_system = kstrdup(match_event_system, GFP_KERNEL);
	if (!data->onmatch.match_event_system) {
		ret = -ENOMEM;
		goto free;
	}

	strsep(&str, ".");
	if (!str) {
		hist_err("onmatch: Missing . after onmatch(): ", str);
		goto free;
	}

	synth_event_name = strsep(&str, "(");
	if (!synth_event_name || !str) {
		hist_err("onmatch: Missing opening paramlist paren: ", synth_event_name);
		goto free;
	}

	data->onmatch.synth_event_name = kstrdup(synth_event_name, GFP_KERNEL);
	if (!data->onmatch.synth_event_name) {
		ret = -ENOMEM;
		goto free;
	}

	params = strsep(&str, ")");
	if (!params || !str || (str && strlen(str))) {
		hist_err("onmatch: Missing closing paramlist paren: ", params);
		goto free;
	}

	ret = parse_action_params(params, data);
	if (ret)
		goto free;
 out:
	return data;
 free:
	onmatch_destroy(data);
	data = ERR_PTR(ret);
	goto out;
}

static int create_hitcount_val(struct hist_trigger_data *hist_data)
{
	hist_data->fields[HITCOUNT_IDX] =
		create_hist_field(hist_data, NULL, HIST_FIELD_FL_HITCOUNT, NULL);
	if (!hist_data->fields[HITCOUNT_IDX])
		return -ENOMEM;

	hist_data->n_vals++;
	hist_data->n_fields++;

	if (WARN_ON(hist_data->n_vals > TRACING_MAP_VALS_MAX))
		return -EINVAL;

	return 0;
}

static int __create_val_field(struct hist_trigger_data *hist_data,
			      unsigned int val_idx,
			      struct trace_event_file *file,
			      char *var_name, char *field_str,
			      unsigned long flags)
{
	struct hist_field *hist_field;
	int ret = 0;

	hist_field = parse_expr(hist_data, file, field_str, flags, var_name, 0);
	if (IS_ERR(hist_field)) {
		ret = PTR_ERR(hist_field);
		goto out;
	}

	hist_data->fields[val_idx] = hist_field;

	++hist_data->n_vals;
	++hist_data->n_fields;

	if (WARN_ON(hist_data->n_vals > TRACING_MAP_VALS_MAX + TRACING_MAP_VARS_MAX))
		ret = -EINVAL;
 out:
	return ret;
}

static int create_val_field(struct hist_trigger_data *hist_data,
			    unsigned int val_idx,
			    struct trace_event_file *file,
			    char *field_str)
{
	if (WARN_ON(val_idx >= TRACING_MAP_VALS_MAX))
		return -EINVAL;

	return __create_val_field(hist_data, val_idx, file, NULL, field_str, 0);
}

static int create_var_field(struct hist_trigger_data *hist_data,
			    unsigned int val_idx,
			    struct trace_event_file *file,
			    char *var_name, char *expr_str)
{
	unsigned long flags = 0;

	if (WARN_ON(val_idx >= TRACING_MAP_VALS_MAX + TRACING_MAP_VARS_MAX))
		return -EINVAL;

	if (find_var(hist_data, file, var_name) && !hist_data->remove) {
		hist_err("Variable already defined: ", var_name);
		return -EINVAL;
	}

	flags |= HIST_FIELD_FL_VAR;
	hist_data->n_vars++;
	if (WARN_ON(hist_data->n_vars > TRACING_MAP_VARS_MAX))
		return -EINVAL;

	return __create_val_field(hist_data, val_idx, file, var_name, expr_str, flags);
}

static int create_val_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	char *fields_str, *field_str;
	unsigned int i, j = 1;
	int ret;

	ret = create_hitcount_val(hist_data);
	if (ret)
		goto out;

	fields_str = hist_data->attrs->vals_str;
	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str)
		goto out;

	for (i = 0, j = 1; i < TRACING_MAP_VALS_MAX &&
		     j < TRACING_MAP_VALS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str)
			break;

		if (strcmp(field_str, "hitcount") == 0)
			continue;

		ret = create_val_field(hist_data, j++, file, field_str);
		if (ret)
			goto out;
	}

	if (fields_str && (strcmp(fields_str, "hitcount") != 0))
		ret = -EINVAL;
 out:
	return ret;
}

static int create_key_field(struct hist_trigger_data *hist_data,
			    unsigned int key_idx,
			    unsigned int key_offset,
			    struct trace_event_file *file,
			    char *field_str)
{
	struct hist_field *hist_field = NULL;

	unsigned long flags = 0;
	unsigned int key_size;
	int ret = 0;

	if (WARN_ON(key_idx >= HIST_FIELDS_MAX))
		return -EINVAL;

	flags |= HIST_FIELD_FL_KEY;

	if (strcmp(field_str, "stacktrace") == 0) {
		flags |= HIST_FIELD_FL_STACKTRACE;
		key_size = sizeof(unsigned long) * HIST_STACKTRACE_DEPTH;
		hist_field = create_hist_field(hist_data, NULL, flags, NULL);
	} else {
		hist_field = parse_expr(hist_data, file, field_str, flags,
					NULL, 0);
		if (IS_ERR(hist_field)) {
			ret = PTR_ERR(hist_field);
			goto out;
		}

		if (hist_field->flags & HIST_FIELD_FL_VAR_REF) {
			hist_err("Using variable references as keys not supported: ", field_str);
			destroy_hist_field(hist_field, 0);
			ret = -EINVAL;
			goto out;
		}

		key_size = hist_field->size;
	}

	hist_data->fields[key_idx] = hist_field;

	key_size = ALIGN(key_size, sizeof(u64));
	hist_data->fields[key_idx]->size = key_size;
	hist_data->fields[key_idx]->offset = key_offset;

	hist_data->key_size += key_size;

	if (hist_data->key_size > HIST_KEY_SIZE_MAX) {
		ret = -EINVAL;
		goto out;
	}

	hist_data->n_keys++;
	hist_data->n_fields++;

	if (WARN_ON(hist_data->n_keys > TRACING_MAP_KEYS_MAX))
		return -EINVAL;

	ret = key_size;
 out:
	return ret;
}

static int create_key_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	unsigned int i, key_offset = 0, n_vals = hist_data->n_vals;
	char *fields_str, *field_str;
	int ret = -EINVAL;

	fields_str = hist_data->attrs->keys_str;
	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str)
		goto out;

	for (i = n_vals; i < n_vals + TRACING_MAP_KEYS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str)
			break;
		ret = create_key_field(hist_data, i, key_offset,
				       file, field_str);
		if (ret < 0)
			goto out;
		key_offset += ret;
	}
	if (fields_str) {
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
 out:
	return ret;
}

static int create_var_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	unsigned int i, j = hist_data->n_vals;
	int ret = 0;

	unsigned int n_vars = hist_data->attrs->var_defs.n_vars;

	for (i = 0; i < n_vars; i++) {
		char *var_name = hist_data->attrs->var_defs.name[i];
		char *expr = hist_data->attrs->var_defs.expr[i];

		ret = create_var_field(hist_data, j++, file, var_name, expr);
		if (ret)
			goto out;
	}
 out:
	return ret;
}

static void free_var_defs(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->attrs->var_defs.n_vars; i++) {
		kfree(hist_data->attrs->var_defs.name[i]);
		kfree(hist_data->attrs->var_defs.expr[i]);
	}

	hist_data->attrs->var_defs.n_vars = 0;
}

static int parse_var_defs(struct hist_trigger_data *hist_data)
{
	char *s, *str, *var_name, *field_str;
	unsigned int i, j, n_vars = 0;
	int ret = 0;

	for (i = 0; i < hist_data->attrs->n_assignments; i++) {
		str = hist_data->attrs->assignment_str[i];
		for (j = 0; j < TRACING_MAP_VARS_MAX; j++) {
			field_str = strsep(&str, ",");
			if (!field_str)
				break;

			var_name = strsep(&field_str, "=");
			if (!var_name || !field_str) {
				hist_err("Malformed assignment: ", var_name);
				ret = -EINVAL;
				goto free;
			}

			if (n_vars == TRACING_MAP_VARS_MAX) {
				hist_err("Too many variables defined: ", var_name);
				ret = -EINVAL;
				goto free;
			}

			s = kstrdup(var_name, GFP_KERNEL);
			if (!s) {
				ret = -ENOMEM;
				goto free;
			}
			hist_data->attrs->var_defs.name[n_vars] = s;

			s = kstrdup(field_str, GFP_KERNEL);
			if (!s) {
				kfree(hist_data->attrs->var_defs.name[n_vars]);
				hist_data->attrs->var_defs.name[n_vars] = NULL;
				ret = -ENOMEM;
				goto free;
			}
			hist_data->attrs->var_defs.expr[n_vars++] = s;

			hist_data->attrs->var_defs.n_vars = n_vars;
		}
	}

	return ret;
 free:
	free_var_defs(hist_data);

	return ret;
}

static int create_hist_fields(struct hist_trigger_data *hist_data,
			      struct trace_event_file *file)
{
	int ret;

	ret = parse_var_defs(hist_data);
	if (ret)
		goto out;

	ret = create_val_fields(hist_data, file);
	if (ret)
		goto out;

	ret = create_var_fields(hist_data, file);
	if (ret)
		goto out;

	ret = create_key_fields(hist_data, file);
	if (ret)
		goto out;
 out:
	free_var_defs(hist_data);

	return ret;
}

static int is_descending(const char *str)
{
	if (!str)
		return 0;

	if (strcmp(str, "descending") == 0)
		return 1;

	if (strcmp(str, "ascending") == 0)
		return 0;

	return -EINVAL;
}

static int create_sort_keys(struct hist_trigger_data *hist_data)
{
	char *fields_str = hist_data->attrs->sort_key_str;
	struct tracing_map_sort_key *sort_key;
	int descending, ret = 0;
	unsigned int i, j, k;

	hist_data->n_sort_keys = 1; /* we always have at least one, hitcount */

	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < TRACING_MAP_SORT_KEYS_MAX; i++) {
		struct hist_field *hist_field;
		char *field_str, *field_name;
		const char *test_name;

		sort_key = &hist_data->sort_keys[i];

		field_str = strsep(&fields_str, ",");
		if (!field_str) {
			if (i == 0)
				ret = -EINVAL;
			break;
		}

		if ((i == TRACING_MAP_SORT_KEYS_MAX - 1) && fields_str) {
			ret = -EINVAL;
			break;
		}

		field_name = strsep(&field_str, ".");
		if (!field_name) {
			ret = -EINVAL;
			break;
		}

		if (strcmp(field_name, "hitcount") == 0) {
			descending = is_descending(field_str);
			if (descending < 0) {
				ret = descending;
				break;
			}
			sort_key->descending = descending;
			continue;
		}

		for (j = 1, k = 1; j < hist_data->n_fields; j++) {
			unsigned int idx;

			hist_field = hist_data->fields[j];
			if (hist_field->flags & HIST_FIELD_FL_VAR)
				continue;

			idx = k++;

			test_name = hist_field_name(hist_field, 0);

			if (strcmp(field_name, test_name) == 0) {
				sort_key->field_idx = idx;
				descending = is_descending(field_str);
				if (descending < 0) {
					ret = descending;
					goto out;
				}
				sort_key->descending = descending;
				break;
			}
		}
		if (j == hist_data->n_fields) {
			ret = -EINVAL;
			break;
		}
	}

	hist_data->n_sort_keys = i;
 out:
	return ret;
}

static void destroy_actions(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];

		if (data->fn == action_trace)
			onmatch_destroy(data);
		else if (data->fn == onmax_save)
			onmax_destroy(data);
		else
			kfree(data);
	}
}

static int parse_actions(struct hist_trigger_data *hist_data)
{
	struct trace_array *tr = hist_data->event_file->tr;
	struct action_data *data;
	unsigned int i;
	int ret = 0;
	char *str;

	for (i = 0; i < hist_data->attrs->n_actions; i++) {
		str = hist_data->attrs->action_str[i];

		if (strncmp(str, "onmatch(", strlen("onmatch(")) == 0) {
			char *action_str = str + strlen("onmatch(");

			data = onmatch_parse(tr, action_str);
			if (IS_ERR(data)) {
				ret = PTR_ERR(data);
				break;
			}
			data->fn = action_trace;
		} else if (strncmp(str, "onmax(", strlen("onmax(")) == 0) {
			char *action_str = str + strlen("onmax(");

			data = onmax_parse(action_str);
			if (IS_ERR(data)) {
				ret = PTR_ERR(data);
				break;
			}
			data->fn = onmax_save;
		} else {
			ret = -EINVAL;
			break;
		}

		hist_data->actions[hist_data->n_actions++] = data;
	}

	return ret;
}

static int create_actions(struct hist_trigger_data *hist_data,
			  struct trace_event_file *file)
{
	struct action_data *data;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < hist_data->attrs->n_actions; i++) {
		data = hist_data->actions[i];

		if (data->fn == action_trace) {
			ret = onmatch_create(hist_data, file, data);
			if (ret)
				return ret;
		} else if (data->fn == onmax_save) {
			ret = onmax_create(hist_data, data);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static void print_actions(struct seq_file *m,
			  struct hist_trigger_data *hist_data,
			  struct tracing_map_elt *elt)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];

		if (data->fn == onmax_save)
			onmax_print(m, hist_data, elt, data);
	}
}

static void print_onmax_spec(struct seq_file *m,
			     struct hist_trigger_data *hist_data,
			     struct action_data *data)
{
	unsigned int i;

	seq_puts(m, ":onmax(");
	seq_printf(m, "%s", data->onmax.var_str);
	seq_printf(m, ").%s(", data->onmax.fn_name);

	for (i = 0; i < hist_data->n_max_vars; i++) {
		seq_printf(m, "%s", hist_data->max_vars[i]->var->var.name);
		if (i < hist_data->n_max_vars - 1)
			seq_puts(m, ",");
	}
	seq_puts(m, ")");
}

static void print_onmatch_spec(struct seq_file *m,
			       struct hist_trigger_data *hist_data,
			       struct action_data *data)
{
	unsigned int i;

	seq_printf(m, ":onmatch(%s.%s).", data->onmatch.match_event_system,
		   data->onmatch.match_event);

	seq_printf(m, "%s(", data->onmatch.synth_event->name);

	for (i = 0; i < data->n_params; i++) {
		if (i)
			seq_puts(m, ",");
		seq_printf(m, "%s", data->params[i]);
	}

	seq_puts(m, ")");
}

static bool actions_match(struct hist_trigger_data *hist_data,
			  struct hist_trigger_data *hist_data_test)
{
	unsigned int i, j;

	if (hist_data->n_actions != hist_data_test->n_actions)
		return false;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];
		struct action_data *data_test = hist_data_test->actions[i];

		if (data->fn != data_test->fn)
			return false;

		if (data->n_params != data_test->n_params)
			return false;

		for (j = 0; j < data->n_params; j++) {
			if (strcmp(data->params[j], data_test->params[j]) != 0)
				return false;
		}

		if (data->fn == action_trace) {
			if (strcmp(data->onmatch.synth_event_name,
				   data_test->onmatch.synth_event_name) != 0)
				return false;
			if (strcmp(data->onmatch.match_event_system,
				   data_test->onmatch.match_event_system) != 0)
				return false;
			if (strcmp(data->onmatch.match_event,
				   data_test->onmatch.match_event) != 0)
				return false;
		} else if (data->fn == onmax_save) {
			if (strcmp(data->onmax.var_str,
				   data_test->onmax.var_str) != 0)
				return false;
			if (strcmp(data->onmax.fn_name,
				   data_test->onmax.fn_name) != 0)
				return false;
		}
	}

	return true;
}


static void print_actions_spec(struct seq_file *m,
			       struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];

		if (data->fn == action_trace)
			print_onmatch_spec(m, hist_data, data);
		else if (data->fn == onmax_save)
			print_onmax_spec(m, hist_data, data);
	}
}

static void destroy_field_var_hists(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_field_var_hists; i++) {
		kfree(hist_data->field_var_hists[i]->cmd);
		kfree(hist_data->field_var_hists[i]);
	}
}

static void destroy_hist_data(struct hist_trigger_data *hist_data)
{
	if (!hist_data)
		return;

	destroy_hist_trigger_attrs(hist_data->attrs);
	destroy_hist_fields(hist_data);
	tracing_map_destroy(hist_data->map);

	destroy_actions(hist_data);
	destroy_field_vars(hist_data);
	destroy_field_var_hists(hist_data);
	destroy_synth_var_refs(hist_data);

	kfree(hist_data);
}

static int create_tracing_map_fields(struct hist_trigger_data *hist_data)
{
	struct tracing_map *map = hist_data->map;
	struct ftrace_event_field *field;
	struct hist_field *hist_field;
	int i, idx = 0;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field->flags & HIST_FIELD_FL_KEY) {
			tracing_map_cmp_fn_t cmp_fn;

			field = hist_field->field;

			if (hist_field->flags & HIST_FIELD_FL_STACKTRACE)
				cmp_fn = tracing_map_cmp_none;
			else if (!field)
				cmp_fn = tracing_map_cmp_num(hist_field->size,
							     hist_field->is_signed);
			else if (is_string_field(field))
				cmp_fn = tracing_map_cmp_string;
			else
				cmp_fn = tracing_map_cmp_num(field->size,
							     field->is_signed);
			idx = tracing_map_add_key_field(map,
							hist_field->offset,
							cmp_fn);
		} else if (!(hist_field->flags & HIST_FIELD_FL_VAR))
			idx = tracing_map_add_sum_field(map);

		if (idx < 0)
			return idx;

		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			idx = tracing_map_add_var(map);
			if (idx < 0)
				return idx;
			hist_field->var.idx = idx;
			hist_field->var.hist_data = hist_data;
		}
	}

	return 0;
}

static struct hist_trigger_data *
create_hist_data(unsigned int map_bits,
		 struct hist_trigger_attrs *attrs,
		 struct trace_event_file *file,
		 bool remove)
{
	const struct tracing_map_ops *map_ops = NULL;
	struct hist_trigger_data *hist_data;
	int ret = 0;

	hist_data = kzalloc(sizeof(*hist_data), GFP_KERNEL);
	if (!hist_data)
		return ERR_PTR(-ENOMEM);

	hist_data->attrs = attrs;
	hist_data->remove = remove;
	hist_data->event_file = file;

	ret = parse_actions(hist_data);
	if (ret)
		goto free;

	ret = create_hist_fields(hist_data, file);
	if (ret)
		goto free;

	ret = create_sort_keys(hist_data);
	if (ret)
		goto free;

	map_ops = &hist_trigger_elt_data_ops;

	hist_data->map = tracing_map_create(map_bits, hist_data->key_size,
					    map_ops, hist_data);
	if (IS_ERR(hist_data->map)) {
		ret = PTR_ERR(hist_data->map);
		hist_data->map = NULL;
		goto free;
	}

	ret = create_tracing_map_fields(hist_data);
	if (ret)
		goto free;
 out:
	return hist_data;
 free:
	hist_data->attrs = NULL;

	destroy_hist_data(hist_data);

	hist_data = ERR_PTR(ret);

	goto out;
}

static void hist_trigger_elt_update(struct hist_trigger_data *hist_data,
				    struct tracing_map_elt *elt, void *rec,
				    struct ring_buffer_event *rbe,
				    u64 *var_ref_vals)
{
	struct hist_elt_data *elt_data;
	struct hist_field *hist_field;
	unsigned int i, var_idx;
	u64 hist_val;

	elt_data = elt->private_data;
	elt_data->var_ref_vals = var_ref_vals;

	for_each_hist_val_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		hist_val = hist_field->fn(hist_field, elt, rbe, rec);
		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			var_idx = hist_field->var.idx;
			tracing_map_set_var(elt, var_idx, hist_val);
			continue;
		}
		tracing_map_update_sum(elt, i, hist_val);
	}

	for_each_hist_key_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			hist_val = hist_field->fn(hist_field, elt, rbe, rec);
			var_idx = hist_field->var.idx;
			tracing_map_set_var(elt, var_idx, hist_val);
		}
	}

	update_field_vars(hist_data, elt, rbe, rec);
}

static inline void add_to_key(char *compound_key, void *key,
			      struct hist_field *key_field, void *rec)
{
	size_t size = key_field->size;

	if (key_field->flags & HIST_FIELD_FL_STRING) {
		struct ftrace_event_field *field;

		field = key_field->field;
		if (field->filter_type == FILTER_DYN_STRING)
			size = *(u32 *)(rec + field->offset) >> 16;
		else if (field->filter_type == FILTER_STATIC_STRING)
			size = field->size;

		/* ensure NULL-termination */
		if (size > key_field->size - 1)
			size = key_field->size - 1;

		strncpy(compound_key + key_field->offset, (char *)key, size);
	} else
		memcpy(compound_key + key_field->offset, key, size);
}

static void
hist_trigger_actions(struct hist_trigger_data *hist_data,
		     struct tracing_map_elt *elt, void *rec,
		     struct ring_buffer_event *rbe, u64 *var_ref_vals)
{
	struct action_data *data;
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		data = hist_data->actions[i];
		data->fn(hist_data, elt, rec, rbe, data, var_ref_vals);
	}
}

static void event_hist_trigger(struct event_trigger_data *data, void *rec,
			       struct ring_buffer_event *rbe)
{
	struct hist_trigger_data *hist_data = data->private_data;
	bool use_compound_key = (hist_data->n_keys > 1);
	unsigned long entries[HIST_STACKTRACE_DEPTH];
	u64 var_ref_vals[TRACING_MAP_VARS_MAX];
	char compound_key[HIST_KEY_SIZE_MAX];
	struct tracing_map_elt *elt = NULL;
	struct stack_trace stacktrace;
	struct hist_field *key_field;
	u64 field_contents;
	void *key = NULL;
	unsigned int i;

	memset(compound_key, 0, hist_data->key_size);

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (key_field->flags & HIST_FIELD_FL_STACKTRACE) {
			stacktrace.max_entries = HIST_STACKTRACE_DEPTH;
			stacktrace.entries = entries;
			stacktrace.nr_entries = 0;
			stacktrace.skip = HIST_STACKTRACE_SKIP;

			memset(stacktrace.entries, 0, HIST_STACKTRACE_SIZE);
			save_stack_trace(&stacktrace);

			key = entries;
		} else {
			field_contents = key_field->fn(key_field, elt, rbe, rec);
			if (key_field->flags & HIST_FIELD_FL_STRING) {
				key = (void *)(unsigned long)field_contents;
				use_compound_key = true;
			} else
				key = (void *)&field_contents;
		}

		if (use_compound_key)
			add_to_key(compound_key, key, key_field, rec);
	}

	if (use_compound_key)
		key = compound_key;

	if (hist_data->n_var_refs &&
	    !resolve_var_refs(hist_data, key, var_ref_vals, false))
		return;

	elt = tracing_map_insert(hist_data->map, key);
	if (!elt)
		return;

	hist_trigger_elt_update(hist_data, elt, rec, rbe, var_ref_vals);

	if (resolve_var_refs(hist_data, key, var_ref_vals, true))
		hist_trigger_actions(hist_data, elt, rec, rbe, var_ref_vals);
}

static void hist_trigger_stacktrace_print(struct seq_file *m,
					  unsigned long *stacktrace_entries,
					  unsigned int max_entries)
{
	char str[KSYM_SYMBOL_LEN];
	unsigned int spaces = 8;
	unsigned int i;

	for (i = 0; i < max_entries; i++) {
		if (stacktrace_entries[i] == ULONG_MAX)
			return;

		seq_printf(m, "%*c", 1 + spaces, ' ');
		sprint_symbol(str, stacktrace_entries[i]);
		seq_printf(m, "%s\n", str);
	}
}

static void
hist_trigger_entry_print(struct seq_file *m,
			 struct hist_trigger_data *hist_data, void *key,
			 struct tracing_map_elt *elt)
{
	struct hist_field *key_field;
	char str[KSYM_SYMBOL_LEN];
	bool multiline = false;
	const char *field_name;
	unsigned int i;
	u64 uval;

	seq_puts(m, "{ ");

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (i > hist_data->n_vals)
			seq_puts(m, ", ");

		field_name = hist_field_name(key_field, 0);

		if (key_field->flags & HIST_FIELD_FL_HEX) {
			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %llx", field_name, uval);
		} else if (key_field->flags & HIST_FIELD_FL_SYM) {
			uval = *(u64 *)(key + key_field->offset);
			sprint_symbol_no_offset(str, uval);
			seq_printf(m, "%s: [%llx] %-45s", field_name,
				   uval, str);
		} else if (key_field->flags & HIST_FIELD_FL_SYM_OFFSET) {
			uval = *(u64 *)(key + key_field->offset);
			sprint_symbol(str, uval);
			seq_printf(m, "%s: [%llx] %-55s", field_name,
				   uval, str);
		} else if (key_field->flags & HIST_FIELD_FL_EXECNAME) {
			struct hist_elt_data *elt_data = elt->private_data;
			char *comm;

			if (WARN_ON_ONCE(!elt_data))
				return;

			comm = elt_data->comm;

			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %-16s[%10llu]", field_name,
				   comm, uval);
		} else if (key_field->flags & HIST_FIELD_FL_SYSCALL) {
			const char *syscall_name;

			uval = *(u64 *)(key + key_field->offset);
			syscall_name = get_syscall_name(uval);
			if (!syscall_name)
				syscall_name = "unknown_syscall";

			seq_printf(m, "%s: %-30s[%3llu]", field_name,
				   syscall_name, uval);
		} else if (key_field->flags & HIST_FIELD_FL_STACKTRACE) {
			seq_puts(m, "stacktrace:\n");
			hist_trigger_stacktrace_print(m,
						      key + key_field->offset,
						      HIST_STACKTRACE_DEPTH);
			multiline = true;
		} else if (key_field->flags & HIST_FIELD_FL_LOG2) {
			seq_printf(m, "%s: ~ 2^%-2llu", field_name,
				   *(u64 *)(key + key_field->offset));
		} else if (key_field->flags & HIST_FIELD_FL_STRING) {
			seq_printf(m, "%s: %-50s", field_name,
				   (char *)(key + key_field->offset));
		} else {
			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %10llu", field_name, uval);
		}
	}

	if (!multiline)
		seq_puts(m, " ");

	seq_puts(m, "}");

	seq_printf(m, " hitcount: %10llu",
		   tracing_map_read_sum(elt, HITCOUNT_IDX));

	for (i = 1; i < hist_data->n_vals; i++) {
		field_name = hist_field_name(hist_data->fields[i], 0);

		if (hist_data->fields[i]->flags & HIST_FIELD_FL_VAR ||
		    hist_data->fields[i]->flags & HIST_FIELD_FL_EXPR)
			continue;

		if (hist_data->fields[i]->flags & HIST_FIELD_FL_HEX) {
			seq_printf(m, "  %s: %10llx", field_name,
				   tracing_map_read_sum(elt, i));
		} else {
			seq_printf(m, "  %s: %10llu", field_name,
				   tracing_map_read_sum(elt, i));
		}
	}

	print_actions(m, hist_data, elt);

	seq_puts(m, "\n");
}

static int print_entries(struct seq_file *m,
			 struct hist_trigger_data *hist_data)
{
	struct tracing_map_sort_entry **sort_entries = NULL;
	struct tracing_map *map = hist_data->map;
	int i, n_entries;

	n_entries = tracing_map_sort_entries(map, hist_data->sort_keys,
					     hist_data->n_sort_keys,
					     &sort_entries);
	if (n_entries < 0)
		return n_entries;

	for (i = 0; i < n_entries; i++)
		hist_trigger_entry_print(m, hist_data,
					 sort_entries[i]->key,
					 sort_entries[i]->elt);

	tracing_map_destroy_sort_entries(sort_entries, n_entries);

	return n_entries;
}

static void hist_trigger_show(struct seq_file *m,
			      struct event_trigger_data *data, int n)
{
	struct hist_trigger_data *hist_data;
	int n_entries;

	if (n > 0)
		seq_puts(m, "\n\n");

	seq_puts(m, "# event histogram\n#\n# trigger info: ");
	data->ops->print(m, data->ops, data);
	seq_puts(m, "#\n\n");

	hist_data = data->private_data;
	n_entries = print_entries(m, hist_data);
	if (n_entries < 0)
		n_entries = 0;

	seq_printf(m, "\nTotals:\n    Hits: %llu\n    Entries: %u\n    Dropped: %llu\n",
		   (u64)atomic64_read(&hist_data->map->hits),
		   n_entries, (u64)atomic64_read(&hist_data->map->drops));
}

static int hist_show(struct seq_file *m, void *v)
{
	struct event_trigger_data *data;
	struct trace_event_file *event_file;
	int n = 0, ret = 0;

	mutex_lock(&event_mutex);

	event_file = event_file_data(m->private);
	if (unlikely(!event_file)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	list_for_each_entry_rcu(data, &event_file->triggers, list) {
		if (data->cmd_ops->trigger_type == ETT_EVENT_HIST)
			hist_trigger_show(m, data, n++);
	}

	if (have_hist_err()) {
		seq_printf(m, "\nERROR: %s\n", hist_err_str);
		seq_printf(m, "  Last command: %s\n", last_hist_cmd);
	}

 out_unlock:
	mutex_unlock(&event_mutex);

	return ret;
}

static int event_hist_open(struct inode *inode, struct file *file)
{
	return single_open(file, hist_show, file);
}

const struct file_operations event_hist_fops = {
	.open = event_hist_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void hist_field_print(struct seq_file *m, struct hist_field *hist_field)
{
	const char *field_name = hist_field_name(hist_field, 0);

	if (hist_field->var.name)
		seq_printf(m, "%s=", hist_field->var.name);

	if (hist_field->flags & HIST_FIELD_FL_CPU)
		seq_puts(m, "common_cpu");
	else if (field_name) {
		if (hist_field->flags & HIST_FIELD_FL_VAR_REF ||
		    hist_field->flags & HIST_FIELD_FL_ALIAS)
			seq_putc(m, '$');
		seq_printf(m, "%s", field_name);
	} else if (hist_field->flags & HIST_FIELD_FL_TIMESTAMP)
		seq_puts(m, "common_timestamp");

	if (hist_field->flags) {
		if (!(hist_field->flags & HIST_FIELD_FL_VAR_REF) &&
		    !(hist_field->flags & HIST_FIELD_FL_EXPR)) {
			const char *flags = get_hist_field_flags(hist_field);

			if (flags)
				seq_printf(m, ".%s", flags);
		}
	}
}

static int event_hist_trigger_print(struct seq_file *m,
				    struct event_trigger_ops *ops,
				    struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct hist_field *field;
	bool have_var = false;
	unsigned int i;

	seq_puts(m, "hist:");

	if (data->name)
		seq_printf(m, "%s:", data->name);

	seq_puts(m, "keys=");

	for_each_hist_key_field(i, hist_data) {
		field = hist_data->fields[i];

		if (i > hist_data->n_vals)
			seq_puts(m, ",");

		if (field->flags & HIST_FIELD_FL_STACKTRACE)
			seq_puts(m, "stacktrace");
		else
			hist_field_print(m, field);
	}

	seq_puts(m, ":vals=");

	for_each_hist_val_field(i, hist_data) {
		field = hist_data->fields[i];
		if (field->flags & HIST_FIELD_FL_VAR) {
			have_var = true;
			continue;
		}

		if (i == HITCOUNT_IDX)
			seq_puts(m, "hitcount");
		else {
			seq_puts(m, ",");
			hist_field_print(m, field);
		}
	}

	if (have_var) {
		unsigned int n = 0;

		seq_puts(m, ":");

		for_each_hist_val_field(i, hist_data) {
			field = hist_data->fields[i];

			if (field->flags & HIST_FIELD_FL_VAR) {
				if (n++)
					seq_puts(m, ",");
				hist_field_print(m, field);
			}
		}
	}

	seq_puts(m, ":sort=");

	for (i = 0; i < hist_data->n_sort_keys; i++) {
		struct tracing_map_sort_key *sort_key;
		unsigned int idx, first_key_idx;

		/* skip VAR vals */
		first_key_idx = hist_data->n_vals - hist_data->n_vars;

		sort_key = &hist_data->sort_keys[i];
		idx = sort_key->field_idx;

		if (WARN_ON(idx >= HIST_FIELDS_MAX))
			return -EINVAL;

		if (i > 0)
			seq_puts(m, ",");

		if (idx == HITCOUNT_IDX)
			seq_puts(m, "hitcount");
		else {
			if (idx >= first_key_idx)
				idx += hist_data->n_vars;
			hist_field_print(m, hist_data->fields[idx]);
		}

		if (sort_key->descending)
			seq_puts(m, ".descending");
	}
	seq_printf(m, ":size=%u", (1 << hist_data->map->map_bits));
	if (hist_data->enable_timestamps)
		seq_printf(m, ":clock=%s", hist_data->attrs->clock);

	print_actions_spec(m, hist_data);

	if (data->filter_str)
		seq_printf(m, " if %s", data->filter_str);

	if (data->paused)
		seq_puts(m, " [paused]");
	else
		seq_puts(m, " [active]");

	seq_putc(m, '\n');

	return 0;
}

static int event_hist_trigger_init(struct event_trigger_ops *ops,
				   struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (!data->ref && hist_data->attrs->name)
		save_named_trigger(hist_data->attrs->name, data);

	data->ref++;

	return 0;
}

static void unregister_field_var_hists(struct hist_trigger_data *hist_data)
{
	struct trace_event_file *file;
	unsigned int i;
	char *cmd;
	int ret;

	for (i = 0; i < hist_data->n_field_var_hists; i++) {
		file = hist_data->field_var_hists[i]->hist_data->event_file;
		cmd = hist_data->field_var_hists[i]->cmd;
		ret = event_hist_trigger_func(&trigger_hist_cmd, file,
					      "!hist", "hist", cmd);
	}
}

static void event_hist_trigger_free(struct event_trigger_ops *ops,
				    struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	data->ref--;
	if (!data->ref) {
		if (data->name)
			del_named_trigger(data);

		trigger_data_free(data);

		remove_hist_vars(hist_data);

		unregister_field_var_hists(hist_data);

		destroy_hist_data(hist_data);
	}
}

static struct event_trigger_ops event_hist_trigger_ops = {
	.func			= event_hist_trigger,
	.print			= event_hist_trigger_print,
	.init			= event_hist_trigger_init,
	.free			= event_hist_trigger_free,
};

static int event_hist_trigger_named_init(struct event_trigger_ops *ops,
					 struct event_trigger_data *data)
{
	data->ref++;

	save_named_trigger(data->named_data->name, data);

	event_hist_trigger_init(ops, data->named_data);

	return 0;
}

static void event_hist_trigger_named_free(struct event_trigger_ops *ops,
					  struct event_trigger_data *data)
{
	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	event_hist_trigger_free(ops, data->named_data);

	data->ref--;
	if (!data->ref) {
		del_named_trigger(data);
		trigger_data_free(data);
	}
}

static struct event_trigger_ops event_hist_trigger_named_ops = {
	.func			= event_hist_trigger,
	.print			= event_hist_trigger_print,
	.init			= event_hist_trigger_named_init,
	.free			= event_hist_trigger_named_free,
};

static struct event_trigger_ops *event_hist_get_trigger_ops(char *cmd,
							    char *param)
{
	return &event_hist_trigger_ops;
}

static void hist_clear(struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (data->name)
		pause_named_trigger(data);

	tracepoint_synchronize_unregister();

	tracing_map_clear(hist_data->map);

	if (data->name)
		unpause_named_trigger(data);
}

static bool compatible_field(struct ftrace_event_field *field,
			     struct ftrace_event_field *test_field)
{
	if (field == test_field)
		return true;
	if (field == NULL || test_field == NULL)
		return false;
	if (strcmp(field->name, test_field->name) != 0)
		return false;
	if (strcmp(field->type, test_field->type) != 0)
		return false;
	if (field->size != test_field->size)
		return false;
	if (field->is_signed != test_field->is_signed)
		return false;

	return true;
}

static bool hist_trigger_match(struct event_trigger_data *data,
			       struct event_trigger_data *data_test,
			       struct event_trigger_data *named_data,
			       bool ignore_filter)
{
	struct tracing_map_sort_key *sort_key, *sort_key_test;
	struct hist_trigger_data *hist_data, *hist_data_test;
	struct hist_field *key_field, *key_field_test;
	unsigned int i;

	if (named_data && (named_data != data_test) &&
	    (named_data != data_test->named_data))
		return false;

	if (!named_data && is_named_trigger(data_test))
		return false;

	hist_data = data->private_data;
	hist_data_test = data_test->private_data;

	if (hist_data->n_vals != hist_data_test->n_vals ||
	    hist_data->n_fields != hist_data_test->n_fields ||
	    hist_data->n_sort_keys != hist_data_test->n_sort_keys)
		return false;

	if (!ignore_filter) {
		if ((data->filter_str && !data_test->filter_str) ||
		   (!data->filter_str && data_test->filter_str))
			return false;
	}

	for_each_hist_field(i, hist_data) {
		key_field = hist_data->fields[i];
		key_field_test = hist_data_test->fields[i];

		if (key_field->flags != key_field_test->flags)
			return false;
		if (!compatible_field(key_field->field, key_field_test->field))
			return false;
		if (key_field->offset != key_field_test->offset)
			return false;
		if (key_field->size != key_field_test->size)
			return false;
		if (key_field->is_signed != key_field_test->is_signed)
			return false;
		if (!!key_field->var.name != !!key_field_test->var.name)
			return false;
		if (key_field->var.name &&
		    strcmp(key_field->var.name, key_field_test->var.name) != 0)
			return false;
	}

	for (i = 0; i < hist_data->n_sort_keys; i++) {
		sort_key = &hist_data->sort_keys[i];
		sort_key_test = &hist_data_test->sort_keys[i];

		if (sort_key->field_idx != sort_key_test->field_idx ||
		    sort_key->descending != sort_key_test->descending)
			return false;
	}

	if (!ignore_filter && data->filter_str &&
	    (strcmp(data->filter_str, data_test->filter_str) != 0))
		return false;

	if (!actions_match(hist_data, hist_data_test))
		return false;

	return true;
}

static int hist_register_trigger(char *glob, struct event_trigger_ops *ops,
				 struct event_trigger_data *data,
				 struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;
	int ret = 0;

	if (hist_data->attrs->name) {
		named_data = find_named_trigger(hist_data->attrs->name);
		if (named_data) {
			if (!hist_trigger_match(data, named_data, named_data,
						true)) {
				hist_err("Named hist trigger doesn't match existing named trigger (includes variables): ", hist_data->attrs->name);
				ret = -EINVAL;
				goto out;
			}
		}
	}

	if (hist_data->attrs->name && !named_data)
		goto new;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			if (hist_data->attrs->pause)
				test->paused = true;
			else if (hist_data->attrs->cont)
				test->paused = false;
			else if (hist_data->attrs->clear)
				hist_clear(test);
			else {
				hist_err("Hist trigger already exists", NULL);
				ret = -EEXIST;
			}
			goto out;
		}
	}
 new:
	if (hist_data->attrs->cont || hist_data->attrs->clear) {
		hist_err("Can't clear or continue a nonexistent hist trigger", NULL);
		ret = -ENOENT;
		goto out;
	}

	if (hist_data->attrs->pause)
		data->paused = true;

	if (named_data) {
		data->private_data = named_data->private_data;
		set_named_trigger_data(data, named_data);
		data->ops = &event_hist_trigger_named_ops;
	}

	if (data->ops->init) {
		ret = data->ops->init(data->ops, data);
		if (ret < 0)
			goto out;
	}

	if (hist_data->enable_timestamps) {
		char *clock = hist_data->attrs->clock;

		ret = tracing_set_clock(file->tr, hist_data->attrs->clock);
		if (ret) {
			hist_err("Couldn't set trace_clock: ", clock);
			goto out;
		}

		tracing_set_time_stamp_abs(file->tr, true);
	}

	if (named_data)
		destroy_hist_data(hist_data);

	ret++;
 out:
	return ret;
}

static int hist_trigger_enable(struct event_trigger_data *data,
			       struct trace_event_file *file)
{
	int ret = 0;

	list_add_tail_rcu(&data->list, &file->triggers);

	update_cond_flag(file);

	if (trace_event_trigger_enable_disable(file, 1) < 0) {
		list_del_rcu(&data->list);
		update_cond_flag(file);
		ret--;
	}

	return ret;
}

static bool have_hist_trigger_match(struct event_trigger_data *data,
				    struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;
	bool match = false;

	if (hist_data->attrs->name)
		named_data = find_named_trigger(hist_data->attrs->name);

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (hist_trigger_match(data, test, named_data, false)) {
				match = true;
				break;
			}
		}
	}

	return match;
}

static bool hist_trigger_check_refs(struct event_trigger_data *data,
				    struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;

	if (hist_data->attrs->name)
		named_data = find_named_trigger(hist_data->attrs->name);

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			hist_data = test->private_data;
			if (check_var_refs(hist_data))
				return true;
			break;
		}
	}

	return false;
}

static void hist_unregister_trigger(char *glob, struct event_trigger_ops *ops,
				    struct event_trigger_data *data,
				    struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;
	bool unregistered = false;

	if (hist_data->attrs->name)
		named_data = find_named_trigger(hist_data->attrs->name);

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			unregistered = true;
			list_del_rcu(&test->list);
			trace_event_trigger_enable_disable(file, 0);
			update_cond_flag(file);
			break;
		}
	}

	if (unregistered && test->ops->free)
		test->ops->free(test->ops, test);

	if (hist_data->enable_timestamps) {
		if (!hist_data->remove || unregistered)
			tracing_set_time_stamp_abs(file->tr, false);
	}
}

static bool hist_file_check_refs(struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data;
	struct event_trigger_data *test;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			hist_data = test->private_data;
			if (check_var_refs(hist_data))
				return true;
		}
	}

	return false;
}

static void hist_unreg_all(struct trace_event_file *file)
{
	struct event_trigger_data *test, *n;
	struct hist_trigger_data *hist_data;
	struct synth_event *se;
	const char *se_name;

	if (hist_file_check_refs(file))
		return;

	list_for_each_entry_safe(test, n, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			hist_data = test->private_data;
			list_del_rcu(&test->list);
			trace_event_trigger_enable_disable(file, 0);

			mutex_lock(&synth_event_mutex);
			se_name = trace_event_name(file->event_call);
			se = find_synth_event(se_name);
			if (se)
				se->ref--;
			mutex_unlock(&synth_event_mutex);

			update_cond_flag(file);
			if (hist_data->enable_timestamps)
				tracing_set_time_stamp_abs(file->tr, false);
			if (test->ops->free)
				test->ops->free(test->ops, test);
		}
	}
}

static int event_hist_trigger_func(struct event_command *cmd_ops,
				   struct trace_event_file *file,
				   char *glob, char *cmd, char *param)
{
	unsigned int hist_trigger_bits = TRACING_MAP_BITS_DEFAULT;
	struct event_trigger_data *trigger_data;
	struct hist_trigger_attrs *attrs;
	struct event_trigger_ops *trigger_ops;
	struct hist_trigger_data *hist_data;
	struct synth_event *se;
	const char *se_name;
	bool remove = false;
	char *trigger, *p;
	int ret = 0;

	if (glob && strlen(glob)) {
		last_cmd_set(param);
		hist_err_clear();
	}

	if (!param)
		return -EINVAL;

	if (glob[0] == '!')
		remove = true;

	/*
	 * separate the trigger from the filter (k:v [if filter])
	 * allowing for whitespace in the trigger
	 */
	p = trigger = param;
	do {
		p = strstr(p, "if");
		if (!p)
			break;
		if (p == param)
			return -EINVAL;
		if (*(p - 1) != ' ' && *(p - 1) != '\t') {
			p++;
			continue;
		}
		if (p >= param + strlen(param) - strlen("if") - 1)
			return -EINVAL;
		if (*(p + strlen("if")) != ' ' && *(p + strlen("if")) != '\t') {
			p++;
			continue;
		}
		break;
	} while (p);

	if (!p)
		param = NULL;
	else {
		*(p - 1) = '\0';
		param = strstrip(p);
		trigger = strstrip(trigger);
	}

	attrs = parse_hist_trigger_attrs(trigger);
	if (IS_ERR(attrs))
		return PTR_ERR(attrs);

	if (attrs->map_bits)
		hist_trigger_bits = attrs->map_bits;

	hist_data = create_hist_data(hist_trigger_bits, attrs, file, remove);
	if (IS_ERR(hist_data)) {
		destroy_hist_trigger_attrs(attrs);
		return PTR_ERR(hist_data);
	}

	trigger_ops = cmd_ops->get_trigger_ops(cmd, trigger);

	trigger_data = kzalloc(sizeof(*trigger_data), GFP_KERNEL);
	if (!trigger_data) {
		ret = -ENOMEM;
		goto out_free;
	}

	trigger_data->count = -1;
	trigger_data->ops = trigger_ops;
	trigger_data->cmd_ops = cmd_ops;

	INIT_LIST_HEAD(&trigger_data->list);
	RCU_INIT_POINTER(trigger_data->filter, NULL);

	trigger_data->private_data = hist_data;

	/* if param is non-empty, it's supposed to be a filter */
	if (param && cmd_ops->set_filter) {
		ret = cmd_ops->set_filter(param, trigger_data, file);
		if (ret < 0)
			goto out_free;
	}

	if (remove) {
		if (!have_hist_trigger_match(trigger_data, file))
			goto out_free;

		if (hist_trigger_check_refs(trigger_data, file)) {
			ret = -EBUSY;
			goto out_free;
		}

		cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);

		mutex_lock(&synth_event_mutex);
		se_name = trace_event_name(file->event_call);
		se = find_synth_event(se_name);
		if (se)
			se->ref--;
		mutex_unlock(&synth_event_mutex);

		ret = 0;
		goto out_free;
	}

	ret = cmd_ops->reg(glob, trigger_ops, trigger_data, file);
	/*
	 * The above returns on success the # of triggers registered,
	 * but if it didn't register any it returns zero.  Consider no
	 * triggers registered a failure too.
	 */
	if (!ret) {
		if (!(attrs->pause || attrs->cont || attrs->clear))
			ret = -ENOENT;
		goto out_free;
	} else if (ret < 0)
		goto out_free;

	if (get_named_trigger_data(trigger_data))
		goto enable;

	if (has_hist_vars(hist_data))
		save_hist_vars(hist_data);

	ret = create_actions(hist_data, file);
	if (ret)
		goto out_unreg;

	ret = tracing_map_init(hist_data->map);
	if (ret)
		goto out_unreg;
enable:
	ret = hist_trigger_enable(trigger_data, file);
	if (ret)
		goto out_unreg;

	mutex_lock(&synth_event_mutex);
	se_name = trace_event_name(file->event_call);
	se = find_synth_event(se_name);
	if (se)
		se->ref++;
	mutex_unlock(&synth_event_mutex);

	/* Just return zero, not the number of registered triggers */
	ret = 0;
 out:
	if (ret == 0)
		hist_err_clear();

	return ret;
 out_unreg:
	cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);
 out_free:
	if (cmd_ops->set_filter)
		cmd_ops->set_filter(NULL, trigger_data, NULL);

	remove_hist_vars(hist_data);

	kfree(trigger_data);

	destroy_hist_data(hist_data);
	goto out;
}

static struct event_command trigger_hist_cmd = {
	.name			= "hist",
	.trigger_type		= ETT_EVENT_HIST,
	.flags			= EVENT_CMD_FL_NEEDS_REC,
	.func			= event_hist_trigger_func,
	.reg			= hist_register_trigger,
	.unreg			= hist_unregister_trigger,
	.unreg_all		= hist_unreg_all,
	.get_trigger_ops	= event_hist_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

__init int register_trigger_hist_cmd(void)
{
	int ret;

	ret = register_event_command(&trigger_hist_cmd);
	WARN_ON(ret < 0);

	return ret;
}

static void
hist_enable_trigger(struct event_trigger_data *data, void *rec,
		    struct ring_buffer_event *event)
{
	struct enable_trigger_data *enable_data = data->private_data;
	struct event_trigger_data *test;

	list_for_each_entry_rcu(test, &enable_data->file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (enable_data->enable)
				test->paused = false;
			else
				test->paused = true;
		}
	}
}

static void
hist_enable_count_trigger(struct event_trigger_data *data, void *rec,
			  struct ring_buffer_event *event)
{
	if (!data->count)
		return;

	if (data->count != -1)
		(data->count)--;

	hist_enable_trigger(data, rec, event);
}

static struct event_trigger_ops hist_enable_trigger_ops = {
	.func			= hist_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_enable_count_trigger_ops = {
	.func			= hist_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_disable_trigger_ops = {
	.func			= hist_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_disable_count_trigger_ops = {
	.func			= hist_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops *
hist_enable_get_trigger_ops(char *cmd, char *param)
{
	struct event_trigger_ops *ops;
	bool enable;

	enable = (strcmp(cmd, ENABLE_HIST_STR) == 0);

	if (enable)
		ops = param ? &hist_enable_count_trigger_ops :
			&hist_enable_trigger_ops;
	else
		ops = param ? &hist_disable_count_trigger_ops :
			&hist_disable_trigger_ops;

	return ops;
}

static void hist_enable_unreg_all(struct trace_event_file *file)
{
	struct event_trigger_data *test, *n;

	list_for_each_entry_safe(test, n, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_HIST_ENABLE) {
			list_del_rcu(&test->list);
			update_cond_flag(file);
			trace_event_trigger_enable_disable(file, 0);
			if (test->ops->free)
				test->ops->free(test->ops, test);
		}
	}
}

static struct event_command trigger_hist_enable_cmd = {
	.name			= ENABLE_HIST_STR,
	.trigger_type		= ETT_HIST_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.unreg_all		= hist_enable_unreg_all,
	.get_trigger_ops	= hist_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static struct event_command trigger_hist_disable_cmd = {
	.name			= DISABLE_HIST_STR,
	.trigger_type		= ETT_HIST_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.unreg_all		= hist_enable_unreg_all,
	.get_trigger_ops	= hist_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static __init void unregister_trigger_hist_enable_disable_cmds(void)
{
	unregister_event_command(&trigger_hist_enable_cmd);
	unregister_event_command(&trigger_hist_disable_cmd);
}

__init int register_trigger_hist_enable_disable_cmds(void)
{
	int ret;

	ret = register_event_command(&trigger_hist_enable_cmd);
	if (WARN_ON(ret < 0))
		return ret;
	ret = register_event_command(&trigger_hist_disable_cmd);
	if (WARN_ON(ret < 0))
		unregister_trigger_hist_enable_disable_cmds();

	return ret;
}

static __init int trace_events_hist_init(void)
{
	struct dentry *entry = NULL;
	struct dentry *d_tracer;
	int err = 0;

	d_tracer = tracing_init_dentry();
	if (IS_ERR(d_tracer)) {
		err = PTR_ERR(d_tracer);
		goto err;
	}

	entry = tracefs_create_file("synthetic_events", 0644, d_tracer,
				    NULL, &synth_events_fops);
	if (!entry) {
		err = -ENODEV;
		goto err;
	}

	return err;
 err:
	pr_warn("Could not create tracefs 'synthetic_events' entry\n");

	return err;
}

fs_initcall(trace_events_hist_init);
