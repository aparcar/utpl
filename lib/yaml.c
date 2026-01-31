/*
 * Copyright (C) 2024 Paul Spooren <mail@aparcar.org>
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

/**
 * # YAML bindings
 *
 * The `yaml` module provides functions for encoding and decoding YAML data.
 *
 * @module yaml
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <yaml.h>

#include "ucode/module.h"

static int last_error = 0;

#define err_return(err) do { last_error = err; return NULL; } while(0)

static uc_value_t *
parse_yaml_value(uc_vm_t *vm, yaml_parser_t *parser, yaml_event_t *event);

static uc_value_t *
parse_yaml_scalar(yaml_event_t *event)
{
	const char *value = (const char *)event->data.scalar.value;
	size_t length = event->data.scalar.length;
	yaml_scalar_style_t style = event->data.scalar.style;
	const char *tag = (const char *)event->data.scalar.tag;
	char *endptr;
	int64_t intval;
	double dblval;

	/* Handle tagged scalars */
	if (tag) {
		if (strcmp(tag, YAML_NULL_TAG) == 0)
			return NULL;

		if (strcmp(tag, YAML_BOOL_TAG) == 0) {
			if (strcasecmp(value, "true") == 0 ||
			    strcasecmp(value, "yes") == 0 ||
			    strcasecmp(value, "on") == 0)
				return ucv_boolean_new(true);
			return ucv_boolean_new(false);
		}

		if (strcmp(tag, YAML_INT_TAG) == 0) {
			intval = strtoll(value, &endptr, 0);
			if (*endptr == '\0')
				return ucv_int64_new(intval);
		}

		if (strcmp(tag, YAML_FLOAT_TAG) == 0) {
			dblval = strtod(value, &endptr);
			if (*endptr == '\0')
				return ucv_double_new(dblval);
		}

		if (strcmp(tag, YAML_STR_TAG) == 0)
			return ucv_string_new_length(value, length);
	}

	/* Quoted strings are always strings */
	if (style == YAML_SINGLE_QUOTED_SCALAR_STYLE ||
	    style == YAML_DOUBLE_QUOTED_SCALAR_STYLE)
		return ucv_string_new_length(value, length);

	/* Check for null */
	if (length == 0 ||
	    strcmp(value, "~") == 0 ||
	    strcasecmp(value, "null") == 0)
		return NULL;

	/* Check for boolean */
	if (strcasecmp(value, "true") == 0 ||
	    strcasecmp(value, "yes") == 0 ||
	    strcasecmp(value, "on") == 0)
		return ucv_boolean_new(true);

	if (strcasecmp(value, "false") == 0 ||
	    strcasecmp(value, "no") == 0 ||
	    strcasecmp(value, "off") == 0)
		return ucv_boolean_new(false);

	/* Check for special float values (before strtod which may partially parse) */
	if (strcasecmp(value, ".inf") == 0 || strcasecmp(value, "+.inf") == 0)
		return ucv_double_new(INFINITY);

	if (strcasecmp(value, "-.inf") == 0)
		return ucv_double_new(-INFINITY);

	if (strcasecmp(value, ".nan") == 0)
		return ucv_double_new(NAN);

	/* Check for YAML 1.1 octal notation (0o prefix) */
	if (length > 2 && value[0] == '0' && (value[1] == 'o' || value[1] == 'O')) {
		intval = strtoll(value + 2, &endptr, 8);
		if (*endptr == '\0' && endptr != value + 2)
			return ucv_int64_new(intval);
	}

	/* Check for integer (decimal, hex, C-style octal) */
	intval = strtoll(value, &endptr, 0);
	if (*endptr == '\0' && endptr != value)
		return ucv_int64_new(intval);

	/* Check for float */
	dblval = strtod(value, &endptr);
	if (*endptr == '\0' && endptr != value)
		return ucv_double_new(dblval);

	/* Default: treat as string */
	return ucv_string_new_length(value, length);
}

static uc_value_t *
parse_yaml_sequence(uc_vm_t *vm, yaml_parser_t *parser)
{
	uc_value_t *arr = ucv_array_new(vm);
	yaml_event_t event;

	while (1) {
		if (!yaml_parser_parse(parser, &event)) {
			ucv_put(arr);
			return NULL;
		}

		if (event.type == YAML_SEQUENCE_END_EVENT) {
			yaml_event_delete(&event);
			break;
		}

		uc_value_t *item = parse_yaml_value(vm, parser, &event);
		ucv_array_push(arr, item);
	}

	return arr;
}

static uc_value_t *
parse_yaml_mapping(uc_vm_t *vm, yaml_parser_t *parser)
{
	uc_value_t *obj = ucv_object_new(vm);
	yaml_event_t key_event, value_event;
	char *key;

	while (1) {
		if (!yaml_parser_parse(parser, &key_event)) {
			ucv_put(obj);
			return NULL;
		}

		if (key_event.type == YAML_MAPPING_END_EVENT) {
			yaml_event_delete(&key_event);
			break;
		}

		if (key_event.type != YAML_SCALAR_EVENT) {
			yaml_event_delete(&key_event);
			ucv_put(obj);
			return NULL;
		}

		key = strndup((const char *)key_event.data.scalar.value,
		              key_event.data.scalar.length);
		yaml_event_delete(&key_event);

		if (!key) {
			ucv_put(obj);
			return NULL;
		}

		if (!yaml_parser_parse(parser, &value_event)) {
			free(key);
			ucv_put(obj);
			return NULL;
		}

		uc_value_t *value = parse_yaml_value(vm, parser, &value_event);
		ucv_object_add(obj, key, value);
		free(key);
	}

	return obj;
}

static uc_value_t *
parse_yaml_value(uc_vm_t *vm, yaml_parser_t *parser, yaml_event_t *event)
{
	uc_value_t *value = NULL;

	switch (event->type) {
	case YAML_SCALAR_EVENT:
		value = parse_yaml_scalar(event);
		yaml_event_delete(event);
		break;

	case YAML_SEQUENCE_START_EVENT:
		yaml_event_delete(event);
		value = parse_yaml_sequence(vm, parser);
		break;

	case YAML_MAPPING_START_EVENT:
		yaml_event_delete(event);
		value = parse_yaml_mapping(vm, parser);
		break;

	case YAML_ALIAS_EVENT:
		/* Aliases are not supported, treat as null */
		yaml_event_delete(event);
		value = NULL;
		break;

	default:
		yaml_event_delete(event);
		break;
	}

	return value;
}

/**
 * Parses a YAML string into a ucode value.
 *
 * Returns the parsed value (object, array, string, number, boolean, or null).
 *
 * Returns `null` if the input is not valid YAML.
 *
 * @function module:yaml#parse
 *
 * @param {string} str
 * The YAML string to parse.
 *
 * @returns {?*}
 *
 * @example
 * const yaml = require('yaml');
 *
 * // Parse a simple YAML document
 * const data = yaml.parse("name: John\nage: 30");
 * // data = { name: "John", age: 30 }
 *
 * // Parse a YAML array
 * const list = yaml.parse("- apple\n- banana\n- cherry");
 * // list = ["apple", "banana", "cherry"]
 */
static uc_value_t *
uc_yaml_parse(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *str = uc_fn_arg(0);
	yaml_parser_t parser;
	yaml_event_t event;
	uc_value_t *result = NULL;
	const char *input;
	size_t input_len;

	if (ucv_type(str) != UC_STRING)
		err_return(EINVAL);

	input = ucv_string_get(str);
	input_len = ucv_string_length(str);

	if (!yaml_parser_initialize(&parser))
		err_return(ENOMEM);

	yaml_parser_set_input_string(&parser,
	                             (const unsigned char *)input,
	                             input_len);

	/* Skip STREAM_START */
	if (!yaml_parser_parse(&parser, &event))
		goto done;

	if (event.type != YAML_STREAM_START_EVENT) {
		yaml_event_delete(&event);
		goto done;
	}
	yaml_event_delete(&event);

	/* Skip DOCUMENT_START */
	if (!yaml_parser_parse(&parser, &event))
		goto done;

	if (event.type != YAML_DOCUMENT_START_EVENT) {
		yaml_event_delete(&event);
		goto done;
	}
	yaml_event_delete(&event);

	/* Parse the actual content */
	if (!yaml_parser_parse(&parser, &event))
		goto done;

	result = parse_yaml_value(vm, &parser, &event);

done:
	yaml_parser_delete(&parser);
	return result;
}

static int
emit_yaml_value(yaml_emitter_t *emitter, uc_value_t *value);

static int
emit_yaml_array(yaml_emitter_t *emitter, uc_value_t *arr)
{
	yaml_event_t event;
	size_t i, len;

	if (!yaml_sequence_start_event_initialize(&event, NULL, NULL, 1,
	                                          YAML_ANY_SEQUENCE_STYLE))
		return 0;

	if (!yaml_emitter_emit(emitter, &event))
		return 0;

	len = ucv_array_length(arr);
	for (i = 0; i < len; i++) {
		if (!emit_yaml_value(emitter, ucv_array_get(arr, i)))
			return 0;
	}

	if (!yaml_sequence_end_event_initialize(&event))
		return 0;

	return yaml_emitter_emit(emitter, &event);
}

static int
emit_yaml_object(yaml_emitter_t *emitter, uc_value_t *obj)
{
	yaml_event_t event;

	if (!yaml_mapping_start_event_initialize(&event, NULL, NULL, 1,
	                                         YAML_ANY_MAPPING_STYLE))
		return 0;

	if (!yaml_emitter_emit(emitter, &event))
		return 0;

	ucv_object_foreach(obj, key, val) {
		/* Emit key */
		if (!yaml_scalar_event_initialize(&event, NULL, NULL,
		                                  (yaml_char_t *)key,
		                                  strlen(key), 1, 1,
		                                  YAML_ANY_SCALAR_STYLE))
			return 0;

		if (!yaml_emitter_emit(emitter, &event))
			return 0;

		/* Emit value */
		if (!emit_yaml_value(emitter, val))
			return 0;
	}

	if (!yaml_mapping_end_event_initialize(&event))
		return 0;

	return yaml_emitter_emit(emitter, &event);
}

static int
emit_yaml_value(yaml_emitter_t *emitter, uc_value_t *value)
{
	yaml_event_t event;
	char buf[64];
	const char *str;
	size_t len;
	yaml_scalar_style_t style = YAML_ANY_SCALAR_STYLE;

	switch (ucv_type(value)) {
	case UC_NULL:
		str = "null";
		len = 4;
		break;

	case UC_BOOLEAN:
		if (ucv_boolean_get(value)) {
			str = "true";
			len = 4;
		}
		else {
			str = "false";
			len = 5;
		}
		break;

	case UC_INTEGER:
		snprintf(buf, sizeof(buf), "%" PRId64, ucv_int64_get(value));
		str = buf;
		len = strlen(buf);
		break;

	case UC_DOUBLE:
		{
			double d = ucv_double_get(value);
			if (isnan(d)) {
				str = ".nan";
				len = 4;
			}
			else if (isinf(d)) {
				if (d > 0) {
					str = ".inf";
					len = 4;
				}
				else {
					str = "-.inf";
					len = 5;
				}
			}
			else {
				snprintf(buf, sizeof(buf), "%g", d);
				str = buf;
				len = strlen(buf);
			}
		}
		break;

	case UC_STRING:
		str = ucv_string_get(value);
		len = ucv_string_length(value);

		/* Use quoted style for strings that might be misinterpreted */
		if (len == 0 ||
		    strcasecmp(str, "true") == 0 ||
		    strcasecmp(str, "false") == 0 ||
		    strcasecmp(str, "yes") == 0 ||
		    strcasecmp(str, "no") == 0 ||
		    strcasecmp(str, "on") == 0 ||
		    strcasecmp(str, "off") == 0 ||
		    strcasecmp(str, "null") == 0 ||
		    strcasecmp(str, "~") == 0 ||
		    strcasecmp(str, ".inf") == 0 ||
		    strcasecmp(str, "-.inf") == 0 ||
		    strcasecmp(str, ".nan") == 0) {
			style = YAML_DOUBLE_QUOTED_SCALAR_STYLE;
		}
		else {
			/* Check if string looks like a number */
			char *endptr;
			strtod(str, &endptr);
			if (*endptr == '\0' && endptr != str)
				style = YAML_DOUBLE_QUOTED_SCALAR_STYLE;
		}
		break;

	case UC_ARRAY:
		return emit_yaml_array(emitter, value);

	case UC_OBJECT:
		return emit_yaml_object(emitter, value);

	default:
		/* Unsupported type, emit as null */
		str = "null";
		len = 4;
		break;
	}

	if (!yaml_scalar_event_initialize(&event, NULL, NULL,
	                                  (yaml_char_t *)str, len,
	                                  1, 1, style))
		return 0;

	return yaml_emitter_emit(emitter, &event);
}

static int
yaml_write_handler(void *data, unsigned char *buffer, size_t size)
{
	uc_stringbuf_t *buf = data;
	ucv_stringbuf_addstr(buf, (const char *)buffer, size);
	return 1;
}

/**
 * Converts a ucode value to a YAML string.
 *
 * Returns the YAML representation of the given value.
 *
 * Returns `null` if the value cannot be converted.
 *
 * @function module:yaml#stringify
 *
 * @param {*} value
 * The value to convert to YAML.
 *
 * @returns {?string}
 *
 * @example
 * const yaml = require('yaml');
 *
 * // Convert an object to YAML
 * const yamlStr = yaml.stringify({ name: "John", age: 30 });
 * // yamlStr = "name: John\nage: 30\n"
 *
 * // Convert an array to YAML
 * const listStr = yaml.stringify(["apple", "banana", "cherry"]);
 * // listStr = "- apple\n- banana\n- cherry\n"
 */
static uc_value_t *
uc_yaml_stringify(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *value = uc_fn_arg(0);
	yaml_emitter_t emitter;
	yaml_event_t event;
	uc_stringbuf_t *buf;
	uc_value_t *result = NULL;

	buf = ucv_stringbuf_new();
	if (!buf)
		err_return(ENOMEM);

	if (!yaml_emitter_initialize(&emitter)) {
		printbuf_free(buf);
		err_return(ENOMEM);
	}

	yaml_emitter_set_output(&emitter, yaml_write_handler, buf);
	yaml_emitter_set_unicode(&emitter, 1);

	/* Emit STREAM_START */
	if (!yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING))
		goto fail;

	if (!yaml_emitter_emit(&emitter, &event))
		goto fail;

	/* Emit DOCUMENT_START */
	if (!yaml_document_start_event_initialize(&event, NULL, NULL, NULL, 1))
		goto fail;

	if (!yaml_emitter_emit(&emitter, &event))
		goto fail;

	/* Emit the actual content */
	if (!emit_yaml_value(&emitter, value))
		goto fail;

	/* Emit DOCUMENT_END */
	if (!yaml_document_end_event_initialize(&event, 1))
		goto fail;

	if (!yaml_emitter_emit(&emitter, &event))
		goto fail;

	/* Emit STREAM_END */
	if (!yaml_stream_end_event_initialize(&event))
		goto fail;

	if (!yaml_emitter_emit(&emitter, &event))
		goto fail;

	result = ucv_stringbuf_finish(buf);
	buf = NULL;

fail:
	yaml_emitter_delete(&emitter);
	if (buf)
		printbuf_free(buf);

	return result;
}

/**
 * Queries error information.
 *
 * Returns a string containing a description of the last occurred error or
 * `null` if there is no error information.
 *
 * @function module:yaml#error
 *
 * @returns {?string}
 */
static uc_value_t *
uc_yaml_error(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *errmsg;

	if (!last_error)
		return NULL;

	errmsg = ucv_string_new(strerror(last_error));
	last_error = 0;
	return errmsg;
}

static const uc_function_list_t global_fns[] = {
	{ "parse",	uc_yaml_parse },
	{ "stringify",	uc_yaml_stringify },
	{ "error",	uc_yaml_error },
};

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, global_fns);
}
