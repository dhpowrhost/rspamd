/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "util.h"
#include "rspamd.h"
#include "message.h"
#include "cfg_file.h"
#include "libutil/regexp.h"
#include "html.h"
#include "images.h"
#include "utlist.h"
#include "tokenizers/tokenizers.h"

#ifdef WITH_SNOWBALL
#include "libstemmer.h"
#endif

#include <iconv.h>

#define RECURSION_LIMIT 5
#define UTF8_CHARSET "UTF-8"
#define GTUBE_SYMBOL "GTUBE"

#define SET_PART_RAW(part) ((part)->flags &= ~RSPAMD_MIME_PART_FLAG_UTF)
#define SET_PART_UTF(part) ((part)->flags |= RSPAMD_MIME_PART_FLAG_UTF)

static const gchar gtube_pattern[] = "XJS*C4JDBQADN1.NSBN3*2IDNEN*"
		"GTUBE-STANDARD-ANTI-UBE-TEST-EMAIL*C.34X";
static rspamd_regexp_t *utf_compatible_re = NULL;

static GQuark
rspamd_message_quark (void)
{
	return g_quark_from_static_string ("mime-error");
}

static void
parse_qmail_recv (rspamd_mempool_t * pool,
	gchar *line,
	struct received_header *r)
{
	gchar *s, *p, t;

	/* We are interested only with received from network headers */
	if ((p = strstr (line, "from network")) == NULL) {
		r->is_error = 2;
		return;
	}

	p += sizeof ("from network") - 1;
	while (g_ascii_isspace (*p) || *p == '[') {
		p++;
	}
	/* format is ip/host */
	s = p;
	if (*p) {
		while (g_ascii_isdigit (*++p) || *p == '.') ;
		if (*p != '/') {
			r->is_error = 1;
			return;
		}
		else {
			*p = '\0';
			r->real_ip = rspamd_mempool_strdup (pool, s);
			*p = '/';
			/* Now try to parse hostname */
			s = ++p;
			while (g_ascii_isalnum (*p) || *p == '.' || *p == '-' || *p ==
				'_') {
				p++;
			}
			t = *p;
			*p = '\0';
			r->real_hostname = rspamd_mempool_strdup (pool, s);
			*p = t;
		}
	}
}

static void
parse_recv_header (rspamd_mempool_t * pool,
	struct raw_header *rh,
	struct received_header *r)
{
	gchar *p, *s, t, **res = NULL;
	gchar *line;
	enum {
		RSPAMD_RECV_STATE_INIT = 0,
		RSPAMD_RECV_STATE_FROM,
		RSPAMD_RECV_STATE_IP_BLOCK,
		RSPAMD_RECV_STATE_BRACES_BLOCK,
		RSPAMD_RECV_STATE_BY_BLOCK,
		RSPAMD_RECV_STATE_PARSE_IP,
		RSPAMD_RECV_STATE_PARSE_IP6,
		RSPAMD_RECV_STATE_SKIP_SPACES,
		RSPAMD_RECV_STATE_ERROR
	} state = RSPAMD_RECV_STATE_INIT, next_state = RSPAMD_RECV_STATE_INIT;
	gboolean is_exim = FALSE;

	line = rh->decoded;
	if (line == NULL) {
		return;
	}

	g_strstrip (line);
	p = line;
	s = line;

	while (*p) {
		switch (state) {
		/* Initial state, search for from */
		case RSPAMD_RECV_STATE_INIT:
			if (*p == 'f' || *p == 'F') {
				if (g_ascii_tolower (*++p) == 'r' && g_ascii_tolower (*++p) ==
					'o' && g_ascii_tolower (*++p) == 'm') {
					p++;
					state = RSPAMD_RECV_STATE_SKIP_SPACES;
					next_state = RSPAMD_RECV_STATE_FROM;
				}
			}
			else if (g_ascii_tolower (*p) == 'b' &&
				g_ascii_tolower (*(p + 1)) == 'y') {
				state = RSPAMD_RECV_STATE_IP_BLOCK;
			}
			else {
				/* This can be qmail header, parse it separately */
				parse_qmail_recv (pool, line, r);
				return;
			}
			break;
		/* Read hostname */
		case RSPAMD_RECV_STATE_FROM:
			if (*p == '[') {
				/* This should be IP address */
				res = &r->from_ip;
				state = RSPAMD_RECV_STATE_PARSE_IP;
				next_state = RSPAMD_RECV_STATE_IP_BLOCK;
				s = ++p;
			}
			else if (g_ascii_isalnum (*p) || *p == '.' || *p == '-' || *p ==
				'_') {
				p++;
			}
			else {
				t = *p;
				*p = '\0';
				r->from_hostname = rspamd_mempool_strdup (pool, s);
				*p = t;
				state = RSPAMD_RECV_STATE_SKIP_SPACES;
				next_state = RSPAMD_RECV_STATE_IP_BLOCK;
			}
			break;
		/* Try to extract additional info */
		case RSPAMD_RECV_STATE_IP_BLOCK:
			/* Try to extract ip or () info or by */
			if (g_ascii_tolower (*p) == 'b' && g_ascii_tolower (*(p + 1)) ==
				'y') {
				p += 2;
				/* Skip spaces after by */
				state = RSPAMD_RECV_STATE_SKIP_SPACES;
				next_state = RSPAMD_RECV_STATE_BY_BLOCK;
			}
			else if (*p == '(') {
				state = RSPAMD_RECV_STATE_SKIP_SPACES;
				next_state = RSPAMD_RECV_STATE_BRACES_BLOCK;
				p++;
			}
			else if (*p == '[') {
				/* Got ip before '(' so extract it */
				s = ++p;
				res = &r->from_ip;
				state = RSPAMD_RECV_STATE_PARSE_IP;
				next_state = RSPAMD_RECV_STATE_IP_BLOCK;
			}
			else {
				p++;
			}
			break;
		/* We are in () block. Here can be found real hostname and real ip, this is written by some MTA */
		case RSPAMD_RECV_STATE_BRACES_BLOCK:
			/* End of block */
			if (g_ascii_isalnum (*p) || *p == '.' || *p == '-' ||
				*p == '_' || *p == ':') {
				p++;
			}
			else if (*p == '[') {
				s = ++p;
				state = RSPAMD_RECV_STATE_PARSE_IP;
				res = &r->real_ip;
				next_state = RSPAMD_RECV_STATE_BRACES_BLOCK;
			}
			else {
				if (p > s) {
					/* Got some real hostname */
					/* check whether it is helo or p is not space symbol */
					if (!g_ascii_isspace (*p) || *(p + 1) != '[') {
						/* Exim style ([ip]:port helo=hostname) */
						if (*s == ':' && (g_ascii_isspace (*p) || *p == ')')) {
							/* Ip ending */
							is_exim = TRUE;
							state = RSPAMD_RECV_STATE_SKIP_SPACES;
							next_state = RSPAMD_RECV_STATE_BRACES_BLOCK;
						}
						else if (p - s == 4 && memcmp (s, "helo=", 5) == 0) {
							p++;
							is_exim = TRUE;
							if (r->real_hostname == NULL && r->from_hostname !=
								NULL) {
								r->real_hostname = r->from_hostname;
							}
							s = p;
							while (*p != ')' && !g_ascii_isspace (*p) && *p !=
								'\0') {
								p++;
							}
							if (p > s) {
								r->from_hostname = rspamd_mempool_alloc (pool,
										p - s + 1);
								rspamd_strlcpy (r->from_hostname, s, p - s + 1);
							}
						}
						else if (p - s == 4 && memcmp (s, "port=", 5) == 0) {
							p++;
							is_exim = TRUE;
							while (g_ascii_isdigit (*p)) {
								p++;
							}
							state = RSPAMD_RECV_STATE_SKIP_SPACES;
							next_state = RSPAMD_RECV_STATE_BRACES_BLOCK;
						}
						else if (*p == '=' && is_exim) {
							/* Just skip unknown pairs */
							p++;
							while (!g_ascii_isspace (*p) && *p != ')' && *p !=
								'\0') {
								p++;
							}
							state = RSPAMD_RECV_STATE_SKIP_SPACES;
							next_state = RSPAMD_RECV_STATE_BRACES_BLOCK;
						}
						else {
							/* skip all  */
							while (*p++ != ')' && *p != '\0') ;
							state = RSPAMD_RECV_STATE_IP_BLOCK;
						}
					}
					else {
						/* Postfix style (hostname [ip]) */
						t = *p;
						*p = '\0';
						r->real_hostname = rspamd_mempool_strdup (pool, s);
						*p = t;
						/* Now parse ip */
						p += 2;
						s = p;
						res = &r->real_ip;
						state = RSPAMD_RECV_STATE_PARSE_IP;
						next_state = RSPAMD_RECV_STATE_BRACES_BLOCK;
						continue;
					}
					if (*p == ')') {
						p++;
						state = RSPAMD_RECV_STATE_SKIP_SPACES;
						next_state = RSPAMD_RECV_STATE_IP_BLOCK;
					}
				}
				else if (*p == ')') {
					p++;
					state = RSPAMD_RECV_STATE_SKIP_SPACES;
					next_state = RSPAMD_RECV_STATE_IP_BLOCK;
				}
				else {
					r->is_error = 1;
					return;
				}
			}
			break;
		/* Got by word */
		case RSPAMD_RECV_STATE_BY_BLOCK:
			/* Here can be only hostname */
			if ((g_ascii_isalnum (*p) || *p == '.' || *p == '-'
				|| *p == '_') && p[1] != '\0') {
				p++;
			}
			else {
				/* We got something like hostname */
				if (p[1] != '\0') {
					t = *p;
					*p = '\0';
					r->by_hostname = rspamd_mempool_strdup (pool, s);
					*p = t;
				}
				else {
					r->by_hostname = rspamd_mempool_strdup (pool, s);
				}
				/* Now end of parsing */
				if (is_exim) {
					/* Adjust for exim received */
					if (r->real_ip == NULL && r->from_ip != NULL) {
						r->real_ip = r->from_ip;
					}
					else if (r->from_ip == NULL && r->real_ip != NULL) {
						r->from_ip = r->real_ip;
						if (r->real_hostname == NULL && r->from_hostname !=
							NULL) {
							r->real_hostname = r->from_hostname;
						}
					}
				}
				return;
			}
			break;

		/* Extract ip */
		case RSPAMD_RECV_STATE_PARSE_IP:
			if (*p == 'I') {
				/* IPv6: */
				state = RSPAMD_RECV_STATE_PARSE_IP6;
			}
			else {
				while (g_ascii_isxdigit (*p) || *p == '.' || *p == ':') {
					p++;
				}
				if (*p != ']') {
					/* Not an ip in fact */
					state = RSPAMD_RECV_STATE_SKIP_SPACES;
					p++;
				}
				else {
					*p = '\0';
					*res = rspamd_mempool_strdup (pool, s);
					*p = ']';
					p++;
					state = RSPAMD_RECV_STATE_SKIP_SPACES;
				}
			}
			break;
		case RSPAMD_RECV_STATE_PARSE_IP6:
			if (g_ascii_strncasecmp (p, "IPv6:", sizeof ("IPv6") - 1) == 0) {
				p += sizeof ("IPv6") - 1;
				s = p;
				state = RSPAMD_RECV_STATE_PARSE_IP;
			}
			else {
				state = RSPAMD_RECV_STATE_SKIP_SPACES;
			}
			break;
		/* Skip spaces */
		case RSPAMD_RECV_STATE_SKIP_SPACES:
			if (!g_ascii_isspace (*p)) {
				state = next_state;
				s = p;
			}
			else {
				p++;
			}
			break;
		default:
			r->is_error = 1;
			return;
			break;
		}
	}

	r->is_error = 1;
	return;
}

static void
append_raw_header (struct rspamd_task *task,
		GHashTable *target, struct raw_header *rh)
{
	struct raw_header *lp;

	rh->next = NULL;
	rh->prev = rh;
	if ((lp =
			g_hash_table_lookup (target, rh->name)) != NULL) {
		DL_APPEND (lp, rh);
	}
	else {
		g_hash_table_insert (target, rh->name, rh);
	}
	msg_debug_task ("add raw header %s: %s", rh->name, rh->value);
}

/* Convert raw headers to a list of struct raw_header * */
static void
process_raw_headers (struct rspamd_task *task, GHashTable *target,
		const gchar *in, gsize len)
{
	struct raw_header *new = NULL;
	const gchar *p, *c, *end;
	gchar *tmp, *tp;
	gint state = 0, l, next_state = 100, err_state = 100, t_state;
	gboolean valid_folding = FALSE;

	p = in;
	end = p + len;
	c = p;

	while (p < end) {
		/* FSM for processing headers */
		switch (state) {
		case 0:
			/* Begin processing headers */
			if (!g_ascii_isalpha (*p)) {
				/* We have some garbage at the beginning of headers, skip this line */
				state = 100;
				next_state = 0;
			}
			else {
				state = 1;
				c = p;
			}
			break;
		case 1:
			/* We got something like header's name */
			if (*p == ':') {
				new =
					rspamd_mempool_alloc0 (task->task_pool,
						sizeof (struct raw_header));
				new->prev = new;
				l = p - c;
				tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
				rspamd_strlcpy (tmp, c, l + 1);
				new->name = tmp;
				new->empty_separator = TRUE;
				p++;
				state = 2;
				c = p;
			}
			else if (g_ascii_isspace (*p)) {
				/* Not header but some garbage */
				task->flags |= RSPAMD_TASK_FLAG_BROKEN_HEADERS;
				state = 100;
				next_state = 0;
			}
			else {
				p++;
			}
			break;
		case 2:
			/* We got header's name, so skip any \t or spaces */
			if (*p == '\t') {
				new->tab_separated = TRUE;
				new->empty_separator = FALSE;
				p++;
			}
			else if (*p == ' ') {
				new->empty_separator = FALSE;
				p++;
			}
			else if (*p == '\n' || *p == '\r') {
				/* Process folding */
				state = 99;
				l = p - c;
				if (l > 0) {
					tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
					rspamd_strlcpy (tmp, c, l + 1);
					new->separator = tmp;
				}
				next_state = 3;
				err_state = 5;
				c = p;
			}
			else {
				/* Process value */
				l = p - c;
				if (l >= 0) {
					tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
					rspamd_strlcpy (tmp, c, l + 1);
					new->separator = tmp;
				}
				c = p;
				state = 3;
			}
			break;
		case 3:
			if (*p == '\r' || *p == '\n') {
				/* Hold folding */
				state = 99;
				next_state = 3;
				err_state = 4;
			}
			else if (p + 1 == end) {
				state = 4;
			}
			else {
				p++;
			}
			break;
		case 4:
			/* Copy header's value */
			l = p - c;
			tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
			tp = tmp;
			t_state = 0;
			while (l--) {
				if (t_state == 0) {
					/* Before folding */
					if (*c == '\n' || *c == '\r') {
						t_state = 1;
						c++;
						*tp++ = ' ';
					}
					else {
						*tp++ = *c++;
					}
				}
				else if (t_state == 1) {
					/* Inside folding */
					if (g_ascii_isspace (*c)) {
						c++;
					}
					else {
						t_state = 0;
						*tp++ = *c++;
					}
				}
			}
			/* Strip last space that can be added by \r\n parsing */
			if (*(tp - 1) == ' ') {
				tp--;
			}

			*tp = '\0';
			/* Strip the initial spaces that could also be added by folding */
			while (*tmp != '\0' && g_ascii_isspace (*tmp)) {
				tmp ++;
			}

			new->value = tmp;
			new->decoded = g_mime_utils_header_decode_text (new->value);

			if (new->decoded != NULL) {
				rspamd_mempool_add_destructor (task->task_pool,
						(rspamd_mempool_destruct_t)g_free, new->decoded);
			}
			else {
				new->decoded = "";
			}

			append_raw_header (task, target, new);
			state = 0;
			break;
		case 5:
			/* Header has only name, no value */
			new->value = "";
			new->decoded = "";
			append_raw_header (task, target, new);
			state = 0;
			break;
		case 99:
			/* Folding state */
			if (p + 1 == end) {
				state = err_state;
			}
			else {
				if (*p == '\r' || *p == '\n') {
					p++;
					valid_folding = FALSE;
				}
				else if (*p == '\t' || *p == ' ') {
					/* Valid folding */
					p++;
					valid_folding = TRUE;
				}
				else {
					if (valid_folding) {
						debug_task ("go to state: %d->%d", state, next_state);
						state = next_state;
					}
					else {
						/* Fall back */
						debug_task ("go to state: %d->%d", state, err_state);
						state = err_state;
					}
				}
			}
			break;
		case 100:
			/* Fail state, skip line */

			if (*p == '\r') {
				if (*(p + 1) == '\n') {
					p++;
				}
				p++;
				state = next_state;
			}
			else if (*p == '\n') {
				if (*(p + 1) == '\r') {
					p++;
				}
				p++;
				state = next_state;
			}
			else if (p + 1 == end) {
				state = next_state;
				p++;
			}
			else {
				p++;
			}
			break;
		}
	}
}

static void
free_byte_array_callback (void *pointer)
{
	GByteArray *arr = (GByteArray *) pointer;
	g_byte_array_free (arr, TRUE);
}

static gboolean
charset_validate (rspamd_mempool_t *pool, const gchar *in, gchar **out)
{
	/*
	 * This is a simple routine to validate input charset
	 * we just check that charset starts with alphanumeric and ends
	 * with alphanumeric
	 */
	const gchar *begin, *end;
	gboolean changed = FALSE, to_uppercase = FALSE;

	begin = in;

	while (!g_ascii_isalnum (*begin)) {
		begin ++;
		changed = TRUE;
	}
	if (!g_ascii_islower(*begin)) {
		changed = TRUE;
		to_uppercase = TRUE;
	}
	end = begin + strlen (begin) - 1;
	while (!g_ascii_isalnum (*end)) {
		end --;
		changed = TRUE;
	}

	if (!changed) {
		*out = (gchar *)in;
	}
	else {
		*out = rspamd_mempool_alloc (pool, end - begin + 2);
		if (to_uppercase) {
			gchar *o = *out;

			while (begin != end + 1) {
				if (g_ascii_islower (*begin)) {
					*o++ = g_ascii_toupper (*begin ++);
				}
				else {
					*o++ = *begin++;
				}
			}
			*o = '\0';
		}
		else {
			rspamd_strlcpy (*out, begin, end - begin + 2);
		}
	}

	return TRUE;
}

static GQuark
converter_error_quark (void)
{
	return g_quark_from_static_string ("conversion error");
}

static gchar *
rspamd_text_to_utf8 (struct rspamd_task *task,
		gchar *input, gsize len, const gchar *in_enc,
		gsize *olen, GError **err)
{
	gchar *s, *d;
	gsize outlen;
	iconv_t ic;
	rspamd_fstring_t *dst;
	gsize remain, ret, inremain = len;

	ic = iconv_open (UTF8_CHARSET, in_enc);

	if (ic == (iconv_t)-1) {
		g_set_error (err, converter_error_quark(), EINVAL,
				"cannot open iconv for: %s", in_enc);
		return NULL;
	}

	/* Preallocate for half of characters to be converted */
	outlen = len + len / 2 + 1;
	dst = rspamd_fstring_sized_new (outlen);
	s = input;
	d = dst->str;
	remain = outlen - 1;

	while (inremain > 0 && remain > 0) {
		ret = iconv (ic, &s, &inremain, &d, &remain);
		dst->len = d - dst->str;

		if (ret == (gsize)-1) {
			switch (errno) {
			case E2BIG:
				/* Enlarge string */
				if (inremain > 0) {
					dst = rspamd_fstring_grow (dst, inremain * 2);
					d = dst->str + dst->len;
					remain = dst->allocated - dst->len - 1;
				}
				break;
			case EILSEQ:
			case EINVAL:
				/* Ignore bad characters */
				if (remain > 0 && inremain > 0) {
					*d++ = '?';
					s++;
					inremain --;
					remain --;
				}
				break;
			}
		}
		else if (ret == 0) {
			break;
		}
	}

	*d = '\0';
	*olen = dst->len;
	iconv_close (ic);
	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t)rspamd_fstring_free, dst);
	msg_info_task ("converted from %s to UTF-8 inlen: %z, outlen: %z",
			in_enc, len, dst->len);

	return dst->str;
}


static GByteArray *
convert_text_to_utf (struct rspamd_task *task,
	GByteArray * part_content,
	GMimeContentType * type,
	struct mime_text_part *text_part)
{
	GError *err = NULL;
	gsize write_bytes;
	const gchar *charset;
	gchar *res_str, *ocharset;
	GByteArray *result_array;

	if (task->cfg->raw_mode) {
		SET_PART_RAW (text_part);
		return part_content;
	}

	if (utf_compatible_re == NULL) {
		utf_compatible_re = rspamd_regexp_new (
			"^(?:utf-?8.*)|(?:us-ascii)|(?:ascii)|(?:us)|(?:ISO-8859-1)|"
			"(?:latin.*)|(?:CSASCII)$",
			"i", NULL);
	}

	if ((charset =
		g_mime_content_type_get_parameter (type, "charset")) == NULL) {
		SET_PART_RAW (text_part);
		return part_content;
	}
	if (!charset_validate (task->task_pool, charset, &ocharset)) {
		msg_info_task (
			"<%s>: has invalid charset",
			task->message_id);
		SET_PART_RAW (text_part);
		return part_content;
	}

	if (rspamd_regexp_match (utf_compatible_re, ocharset, strlen (ocharset), TRUE)) {
		if (g_utf8_validate (part_content->data, part_content->len, NULL)) {
			SET_PART_UTF (text_part);
			return part_content;
		}
		else {
			msg_info_task (
				"<%s>: contains invalid utf8 characters, assume it as raw",
				task->message_id);
			SET_PART_RAW (text_part);
			return part_content;
		}
	}
	else {
		res_str = rspamd_text_to_utf8 (task, part_content->data,
				part_content->len,
				ocharset,
				&write_bytes,
				&err);
		if (res_str == NULL) {
			msg_warn_task ("<%s>: cannot convert from %s to utf8: %s",
					task->message_id,
					ocharset,
					err ? err->message : "unknown problem");
			SET_PART_RAW (text_part);
			g_error_free (err);
			return part_content;
		}
	}

	result_array = rspamd_mempool_alloc (task->task_pool, sizeof (GByteArray));
	result_array->data = res_str;
	result_array->len = write_bytes;
	SET_PART_UTF (text_part);

	return result_array;
}

struct language_match {
	const char *code;
	const char *name;
	GUnicodeScript script;
};

static int
language_elts_cmp (const void *a, const void *b)
{
	GUnicodeScript sc = *(const GUnicodeScript *)a;
	const struct language_match *bb = (const struct language_match *)b;

	return (sc - bb->script);
}

static void
detect_text_language (struct mime_text_part *part)
{
	/* Keep sorted */
	static const struct language_match language_codes[] = {
			{ "", "english", G_UNICODE_SCRIPT_COMMON },
			{ "", "", G_UNICODE_SCRIPT_INHERITED },
			{ "ar", "arabic", G_UNICODE_SCRIPT_ARABIC },
			{ "hy", "armenian", G_UNICODE_SCRIPT_ARMENIAN },
			{ "bn", "chineese", G_UNICODE_SCRIPT_BENGALI },
			{ "", "", G_UNICODE_SCRIPT_BOPOMOFO },
			{ "chr", "", G_UNICODE_SCRIPT_CHEROKEE },
			{ "cop", "",  G_UNICODE_SCRIPT_COPTIC  },
			{ "ru", "russian",  G_UNICODE_SCRIPT_CYRILLIC },
			/* Deseret was used to write English */
			{ "", "",  G_UNICODE_SCRIPT_DESERET },
			{ "hi", "",  G_UNICODE_SCRIPT_DEVANAGARI },
			{ "am", "",  G_UNICODE_SCRIPT_ETHIOPIC },
			{ "ka", "",  G_UNICODE_SCRIPT_GEORGIAN },
			{ "", "",  G_UNICODE_SCRIPT_GOTHIC },
			{ "el", "greek",  G_UNICODE_SCRIPT_GREEK },
			{ "gu", "",  G_UNICODE_SCRIPT_GUJARATI },
			{ "pa", "",  G_UNICODE_SCRIPT_GURMUKHI },
			{ "han", "chineese",  G_UNICODE_SCRIPT_HAN },
			{ "ko", "",  G_UNICODE_SCRIPT_HANGUL },
			{ "he", "hebrew",  G_UNICODE_SCRIPT_HEBREW },
			{ "ja", "",  G_UNICODE_SCRIPT_HIRAGANA },
			{ "kn", "",  G_UNICODE_SCRIPT_KANNADA },
			{ "ja", "",  G_UNICODE_SCRIPT_KATAKANA },
			{ "km", "",  G_UNICODE_SCRIPT_KHMER },
			{ "lo", "",  G_UNICODE_SCRIPT_LAO },
			{ "en", "english",  G_UNICODE_SCRIPT_LATIN },
			{ "ml", "",  G_UNICODE_SCRIPT_MALAYALAM },
			{ "mn", "",  G_UNICODE_SCRIPT_MONGOLIAN },
			{ "my", "",  G_UNICODE_SCRIPT_MYANMAR },
			/* Ogham was used to write old Irish */
			{ "", "",  G_UNICODE_SCRIPT_OGHAM },
			{ "", "",  G_UNICODE_SCRIPT_OLD_ITALIC },
			{ "or", "",  G_UNICODE_SCRIPT_ORIYA },
			{ "", "",  G_UNICODE_SCRIPT_RUNIC },
			{ "si", "",  G_UNICODE_SCRIPT_SINHALA },
			{ "syr", "",  G_UNICODE_SCRIPT_SYRIAC },
			{ "ta", "",  G_UNICODE_SCRIPT_TAMIL },
			{ "te", "",  G_UNICODE_SCRIPT_TELUGU },
			{ "dv", "",  G_UNICODE_SCRIPT_THAANA },
			{ "th", "",  G_UNICODE_SCRIPT_THAI },
			{ "bo", "",  G_UNICODE_SCRIPT_TIBETAN },
			{ "iu", "",  G_UNICODE_SCRIPT_CANADIAN_ABORIGINAL },
			{ "", "",  G_UNICODE_SCRIPT_YI },
			{ "tl", "",  G_UNICODE_SCRIPT_TAGALOG },
			/* Phillipino languages/scripts */
			{ "hnn", "",  G_UNICODE_SCRIPT_HANUNOO },
			{ "bku", "",  G_UNICODE_SCRIPT_BUHID },
			{ "tbw", "",  G_UNICODE_SCRIPT_TAGBANWA },

			{ "", "",  G_UNICODE_SCRIPT_BRAILLE },
			{ "", "",  G_UNICODE_SCRIPT_CYPRIOT },
			{ "", "",  G_UNICODE_SCRIPT_LIMBU },
			/* Used for Somali (so) in the past */
			{ "", "",  G_UNICODE_SCRIPT_OSMANYA },
			/* The Shavian alphabet was designed for English */
			{ "", "",  G_UNICODE_SCRIPT_SHAVIAN },
			{ "", "",  G_UNICODE_SCRIPT_LINEAR_B },
			{ "", "",  G_UNICODE_SCRIPT_TAI_LE },
			{ "uga", "",  G_UNICODE_SCRIPT_UGARITIC },
			{ "", "",  G_UNICODE_SCRIPT_NEW_TAI_LUE },
			{ "bug", "",  G_UNICODE_SCRIPT_BUGINESE },
			{ "", "",  G_UNICODE_SCRIPT_GLAGOLITIC },
			/* Used for for Berber (ber), but Arabic script is more common */
			{ "", "",  G_UNICODE_SCRIPT_TIFINAGH },
			{ "syl", "",  G_UNICODE_SCRIPT_SYLOTI_NAGRI },
			{ "peo", "",  G_UNICODE_SCRIPT_OLD_PERSIAN },
			{ "", "",  G_UNICODE_SCRIPT_KHAROSHTHI },
			{ "", "",  G_UNICODE_SCRIPT_UNKNOWN },
			{ "", "",  G_UNICODE_SCRIPT_BALINESE },
			{ "", "",  G_UNICODE_SCRIPT_CUNEIFORM },
			{ "", "",  G_UNICODE_SCRIPT_PHOENICIAN },
			{ "", "",  G_UNICODE_SCRIPT_PHAGS_PA },
			{ "nqo", "", G_UNICODE_SCRIPT_NKO }
	};
	const struct language_match *lm;
	const int max_chars = 32;

	if (part != NULL) {
		if (IS_PART_UTF (part)) {
			/* Try to detect encoding by several symbols */
			const gchar *p, *pp;
			gunichar c;
			gint32 remain = part->content->len, max = 0, processed = 0;
			gint32 scripts[G_N_ELEMENTS (language_codes)];
			GUnicodeScript scc, sel = G_UNICODE_SCRIPT_COMMON;

			p = part->content->data;
			memset (scripts, 0, sizeof (scripts));

			while (remain > 0 && processed < max_chars) {
				c = g_utf8_get_char_validated (p, remain);
				if (c == (gunichar) -2 || c == (gunichar) -1) {
					break;
				}
				if (g_unichar_isalpha (c)) {
					scc = g_unichar_get_script (c);
					if (scc < (gint)G_N_ELEMENTS (scripts)) {
						scripts[scc]++;
					}
					processed ++;
				}
				pp = g_utf8_next_char (p);
				remain -= pp - p;
				p = pp;
			}
			for (remain = 0; remain < (gint)G_N_ELEMENTS (scripts); remain++) {
				if (scripts[remain] > max) {
					max = scripts[remain];
					sel = remain;
				}
			}
			part->script = sel;
			lm = bsearch (&sel, language_codes, G_N_ELEMENTS (language_codes),
					sizeof (language_codes[0]), &language_elts_cmp);

			if (lm != NULL) {
				part->lang_code = lm->code;
				part->language = lm->name;
			}
		}
	}
}

static void
rspamd_normalize_text_part (struct rspamd_task *task,
		struct mime_text_part *part)
{
#ifdef WITH_SNOWBALL
	struct sb_stemmer *stem = NULL;
#endif
	rspamd_ftok_t *w;
	const guchar *r, *p, *c, *end;
	gchar *temp_word;
	guint i, nlen;

#ifdef WITH_SNOWBALL
	if (part->language && part->language[0] != '\0' && IS_PART_UTF (part)) {
		stem = sb_stemmer_new (part->language, "UTF_8");
		if (stem == NULL) {
			msg_info_task ("<%s> cannot create lemmatizer for %s language",
				task->message_id, part->language);
		}
	}
#endif
	/* Strip newlines */
	part->stripped_content = g_byte_array_sized_new (part->content->len);
	p = part->content->data;
	c = p;
	end = p + part->content->len;

	while (p < end) {
		if (*p == '\r' || *p == '\n') {
			if (p > c) {
				g_byte_array_append (part->stripped_content, c, p - c);
			}

			while (p < end && (*p == '\r' || *p == '\n')) {
				p ++;
			}
			c = p;
		}
		else {
			p ++;
		}
	}

	if (p > c) {
		g_byte_array_append (part->stripped_content, c, p - c);
	}

	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) free_byte_array_callback,
			part->stripped_content);

	/* Ugly workaround */
	part->normalized_words = rspamd_tokenize_text (part->content->data,
			part->content->len, IS_PART_UTF (part), task->cfg,
			part->urls_offset, FALSE,
			NULL);

	if (part->normalized_words) {
		for (i = 0; i < part->normalized_words->len; i ++) {
			w = &g_array_index (part->normalized_words, rspamd_ftok_t, i);
			r = NULL;
#ifdef WITH_SNOWBALL
			if (stem) {
				r = sb_stemmer_stem (stem, w->begin, w->len);
			}
#endif

			if (w->len > 0 && !(w->len == 6 && memcmp (w->begin, "!!EX!!", 6) == 0)) {
				if (r != NULL) {
					nlen = strlen (r);
					nlen = MIN (nlen, w->len);
					temp_word = rspamd_mempool_alloc (task->task_pool, nlen);
					memcpy (temp_word, r, nlen);
					w->begin = temp_word;
					w->len = nlen;
				}
				else {
					temp_word = rspamd_mempool_alloc (task->task_pool, w->len);
					memcpy (temp_word, w->begin, w->len);

					if (IS_PART_UTF (part)) {
						rspamd_str_lc_utf8 (temp_word, w->len);
					}
					else {
						rspamd_str_lc (temp_word, w->len);
					}

					w->begin = temp_word;
				}
			}
		}
	}
#ifdef WITH_SNOWBALL
	if (stem != NULL) {
		sb_stemmer_delete (stem);
	}
#endif
}

#define MIN3(a, b, c) ((a) < (b) ? ((a) < (c) ? (a) : (c)) : ((b) < (c) ? (b) : (c)))

static guint
rspamd_words_levenshtein_distance (struct rspamd_task *task,
		GArray *w1, GArray *w2)
{
	guint s1len, s2len, x, y, lastdiag, olddiag;
	guint *column;
	rspamd_ftok_t *s1, *s2;
	gint eq;
	static const guint max_words = 8192;

	s1len = w1->len;
	s2len = w2->len;

	if (s1len > max_words) {
		msg_err_task ("cannot compare parts with more than %ud words: %ud",
				max_words, s1len);
		return 0;
	}

	column = g_alloca ((s1len + 1) * sizeof (guint));

	for (y = 1; y <= s1len; y++) {
		column[y] = y;
	}

	for (x = 1; x <= s2len; x++) {
		column[0] = x;

		for (y = 1, lastdiag = x - 1; y <= s1len; y++) {
			olddiag = column[y];
			s1 = &g_array_index (w1, rspamd_ftok_t, y - 1);
			s2 = &g_array_index (w2, rspamd_ftok_t, x - 1);
			eq = rspamd_ftok_cmp (s1, s2) == 0 ? 0 : 1;
			column[y] = MIN3 (column[y] + 1, column[y - 1] + 1,
					lastdiag + (eq));
			lastdiag = olddiag;
		}
	}

	return column[s1len];
}

static gboolean
rspamd_check_gtube (struct rspamd_task *task, struct mime_text_part *part)
{
	static const gsize max_check_size = 4 * 1024;
	g_assert (part != NULL);

	if (part->content && part->content->len > sizeof (gtube_pattern) &&
			part->content->len <= max_check_size) {
		if (rspamd_substring_search_twoway (part->content->data,
				part->content->len,
				gtube_pattern, sizeof (gtube_pattern) - 1) != -1) {
			task->flags |= RSPAMD_TASK_FLAG_SKIP;
			task->flags |= RSPAMD_TASK_FLAG_GTUBE;
			msg_info_task ("<%s>: gtube pattern has been found in part of length %ud",
					task->message_id, part->content->len);

			return TRUE;
		}
	}

	return FALSE;
}

static void
process_text_part (struct rspamd_task *task,
	GByteArray *part_content,
	GMimeContentType *type,
	struct mime_part *mime_part,
	GMimeObject *parent,
	gboolean is_empty)
{
	struct mime_text_part *text_part;
	const gchar *cd, *p, *c;
	guint remain;

	/* Skip attachments */
#ifndef GMIME24
	cd = g_mime_part_get_content_disposition (GMIME_PART (mime_part->mime));
	if (cd &&
		g_ascii_strcasecmp (cd,
		"attachment") == 0 && !task->cfg->check_text_attachements) {
		debug_task ("skip attachments for checking as text parts");
		return;
	}
#else
	cd = g_mime_object_get_disposition (GMIME_OBJECT (mime_part->mime));
	if (cd &&
		g_ascii_strcasecmp (cd,
		GMIME_DISPOSITION_ATTACHMENT) == 0 &&
		!task->cfg->check_text_attachements) {
		debug_task ("skip attachments for checking as text parts");
		return;
	}
#endif

	if (g_mime_content_type_is_type (type, "text",
		"html") || g_mime_content_type_is_type (type, "text", "xhtml")) {

		text_part =
			rspamd_mempool_alloc0 (task->task_pool,
				sizeof (struct mime_text_part));
		text_part->flags |= RSPAMD_MIME_PART_FLAG_HTML;
		if (is_empty) {
			text_part->flags |= RSPAMD_MIME_PART_FLAG_EMPTY;
			text_part->orig = NULL;
			text_part->content = NULL;
			g_ptr_array_add (task->text_parts, text_part);
			return;
		}
		text_part->orig = part_content;
		part_content = convert_text_to_utf (task,
				text_part->orig,
				type,
				text_part);
		text_part->html = rspamd_mempool_alloc0 (task->task_pool,
				sizeof (*text_part->html));
		text_part->parent = parent;
		text_part->mime_part = mime_part;

		text_part->flags |= RSPAMD_MIME_PART_FLAG_BALANCED;
		text_part->content = rspamd_html_process_part_full (
				task->task_pool,
				text_part->html,
				part_content,
				&text_part->urls_offset,
				task->urls,
				task->emails);

		if (text_part->content->len == 0) {
			text_part->flags |= RSPAMD_MIME_PART_FLAG_EMPTY;
		}

		/* Handle offsets of this part */
		if (text_part->urls_offset != NULL) {
			text_part->urls_offset = g_list_reverse (text_part->urls_offset);
			rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t) g_list_free, text_part->urls_offset);
		}

		rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) free_byte_array_callback,
			text_part->content);
		g_ptr_array_add (task->text_parts, text_part);
	}
	else if (g_mime_content_type_is_type (type, "text", "*")) {

		text_part =
			rspamd_mempool_alloc0 (task->task_pool,
				sizeof (struct mime_text_part));
		text_part->parent = parent;
		text_part->mime_part = mime_part;

		if (is_empty) {
			text_part->flags |= RSPAMD_MIME_PART_FLAG_EMPTY;
			text_part->orig = NULL;
			text_part->content = NULL;
			g_ptr_array_add (task->text_parts, text_part);
			return;
		}

		text_part->content = convert_text_to_utf (task,
				part_content,
				type,
				text_part);
		text_part->orig = part_content;
		rspamd_url_text_extract (task->task_pool, task, text_part, FALSE);
		g_ptr_array_add (task->text_parts, text_part);
	}
	else {
		return;
	}

	if (rspamd_check_gtube (task, text_part)) {
		struct metric_result *mres;

		mres = rspamd_create_metric_result (task, DEFAULT_METRIC);

		if (mres != NULL) {
			mres->score = mres->metric->actions[METRIC_ACTION_REJECT].score;
			mres->action = METRIC_ACTION_REJECT;
		}

		task->pre_result.action = METRIC_ACTION_REJECT;
		task->pre_result.str = "Gtube pattern";
		rspamd_task_insert_result (task, GTUBE_SYMBOL, 0, NULL);

		return;
	}

	/* Post process part */
	detect_text_language (text_part);
	rspamd_normalize_text_part (task, text_part);

	/* Calculate number of lines */
	p = text_part->content->data;
	remain = text_part->content->len;
	c = p;

	while (p != NULL && remain > 0) {
		p = memchr (c, '\n', remain);

		if (p != NULL) {
			text_part->nlines ++;
			remain -= p - c + 1;
			c = p + 1;
		}
	}
}

struct mime_foreach_data {
	struct rspamd_task *task;
	guint parser_recursion;
	GMimeObject *parent;
};

#ifdef GMIME24
static void
mime_foreach_callback (GMimeObject * parent,
	GMimeObject * part,
	gpointer user_data)
#else
static void
mime_foreach_callback (GMimeObject * part, gpointer user_data)
#endif
{
	struct mime_foreach_data *md = user_data;
	struct rspamd_task *task;
	struct mime_part *mime_part;
	GMimeContentType *type;
	GMimeDataWrapper *wrapper;
	GMimeStream *part_stream;
	GByteArray *part_content;
	gchar *hdrs;

	task = md->task;
	/* 'part' points to the current part node that g_mime_message_foreach_part() is iterating over */

	/* find out what class 'part' is... */
	if (GMIME_IS_MESSAGE_PART (part)) {
		/* message/rfc822 or message/news */
		GMimeMessage *message;

		/* g_mime_message_foreach_part() won't descend into
		   child message parts, so if we want to count any
		   subparts of this child message, we'll have to call
		   g_mime_message_foreach_part() again here. */

		message = g_mime_message_part_get_message ((GMimeMessagePart *) part);
		if (md->parser_recursion++ < RECURSION_LIMIT) {
#ifdef GMIME24
			g_mime_message_foreach (message, mime_foreach_callback, md);
#else
			g_mime_message_foreach_part (message, mime_foreach_callback, md);
#endif
		}
		else {
			msg_err_task ("too deep mime recursion detected: %d", md->parser_recursion);
			return;
		}
#ifndef GMIME24
		g_object_unref (message);
#endif
	}
	else if (GMIME_IS_MESSAGE_PARTIAL (part)) {
		/* message/partial */

		/* this is an incomplete message part, probably a
		   large message that the sender has broken into
		   smaller parts and is sending us bit by bit. we
		   could save some info about it so that we could
		   piece this back together again once we get all the
		   parts? */
	}
	else if (GMIME_IS_MULTIPART (part)) {
		/* multipart/mixed, multipart/alternative, multipart/related, multipart/signed, multipart/encrypted, etc... */
#ifndef GMIME24
		debug_task ("detected multipart part");
		/* we'll get to finding out if this is a signed/encrypted multipart later... */
		if (task->parser_recursion++ < RECURSION_LIMIT) {
			g_mime_multipart_foreach ((GMimeMultipart *) part,
				mime_foreach_callback,
				md);
		}
		else {
			msg_err_task ("endless recursion detected: %d", task->parser_recursion);
			return;
		}
#endif
		type = (GMimeContentType *) g_mime_object_get_content_type (GMIME_OBJECT (
				part));
		mime_part = rspamd_mempool_alloc0 (task->task_pool,
				sizeof (struct mime_part));

		hdrs = g_mime_object_get_headers (GMIME_OBJECT (part));
		mime_part->raw_headers = g_hash_table_new (rspamd_strcase_hash,
				rspamd_strcase_equal);

		if (hdrs != NULL) {
			process_raw_headers (task, mime_part->raw_headers,
					hdrs, strlen (hdrs));
			mime_part->raw_headers_str = hdrs;
		}

		mime_part->type = type;
		/* XXX: we don't need it, but it's sometimes dereferenced */
		mime_part->content = g_byte_array_new ();
		mime_part->parent = md->parent;
		mime_part->filename = NULL;
		mime_part->mime = part;
		mime_part->boundary = g_mime_multipart_get_boundary (GMIME_MULTIPART (part));

		debug_task ("found part with content-type: %s/%s",
				type->type,
				type->subtype);
		g_ptr_array_add (task->parts, mime_part);

		md->parent = part;
	}
	else if (GMIME_IS_PART (part)) {
		/* a normal leaf part, could be text/plain or image/jpeg etc */
#ifdef GMIME24
		type = (GMimeContentType *) g_mime_object_get_content_type (GMIME_OBJECT (
					part));
#else
		type =
			(GMimeContentType *) g_mime_part_get_content_type (GMIME_PART (part));
#endif

		if (type == NULL) {
			msg_warn_task ("type of part is unknown, assume text/plain");
			type = g_mime_content_type_new ("text", "plain");
#ifdef GMIME24
			rspamd_mempool_add_destructor (task->task_pool,
				(rspamd_mempool_destruct_t) g_object_unref,				 type);
#else
			rspamd_mempool_add_destructor (task->task_pool,
				(rspamd_mempool_destruct_t) g_mime_content_type_destroy, type);
#endif
		}
		wrapper = g_mime_part_get_content_object (GMIME_PART (part));
#ifdef GMIME24
		if (wrapper != NULL && GMIME_IS_DATA_WRAPPER (wrapper)) {
#else
		if (wrapper != NULL) {
#endif
			part_stream = g_mime_stream_mem_new ();
			if (g_mime_data_wrapper_write_to_stream (wrapper,
				part_stream) != -1) {

				g_mime_stream_mem_set_owner (GMIME_STREAM_MEM (
						part_stream), FALSE);
				part_content = g_mime_stream_mem_get_byte_array (GMIME_STREAM_MEM (
							part_stream));
				g_object_unref (part_stream);
				mime_part =
					rspamd_mempool_alloc0 (task->task_pool,
						sizeof (struct mime_part));

				hdrs = g_mime_object_get_headers (GMIME_OBJECT (part));
				mime_part->raw_headers = g_hash_table_new (rspamd_strcase_hash,
						rspamd_strcase_equal);

				if (hdrs != NULL) {
					process_raw_headers (task, mime_part->raw_headers,
							hdrs, strlen (hdrs));
					mime_part->raw_headers_str = hdrs;
				}

				mime_part->type = type;
				mime_part->content = part_content;
				mime_part->parent = md->parent;
				mime_part->filename = g_mime_part_get_filename (GMIME_PART (
							part));
				mime_part->mime = part;

				debug_task ("found part with content-type: %s/%s",
					type->type,
					type->subtype);
				g_ptr_array_add (task->parts, mime_part);
				/* Skip empty parts */
				process_text_part (task,
					part_content,
					type,
					mime_part,
					md->parent,
					(part_content->len <= 0));
			}
			else {
				msg_warn_task ("write to stream failed: %d, %s", errno,
					strerror (errno));
			}
#ifndef GMIME24
			g_object_unref (wrapper);
#endif
		}
		else {
			msg_warn_task ("cannot get wrapper for mime part, type of part: %s/%s",
				type->type,
				type->subtype);
		}
	}
	else {
		g_assert_not_reached ();
	}
}

static void
destroy_message (void *pointer)
{
	GMimeMessage *msg = pointer;

	g_object_unref (msg);
}

/* Creates message from various data using libmagic to detect type */
static void
rspamd_message_from_data (struct rspamd_task *task, GByteArray *data,
		GMimeStream *stream)
{
	GMimeMessage *message;
	GMimePart *part;
	GMimeDataWrapper *wrapper;
	GMimeContentType *ct = NULL;
	const char *mb = NULL;
	gchar *mid;
	rspamd_ftok_t srch, *tok;

	g_assert (data != NULL);

	message = g_mime_message_new (TRUE);
	task->message = message;
	if (task->from_envelope) {
		g_mime_message_set_sender (task->message,
				rspamd_task_get_sender (task));
	}

	srch.begin = "Content-Type";
	srch.len = sizeof ("Content-Type") - 1;
	tok = g_hash_table_lookup (task->request_headers, &srch);

	if (tok) {
		/* We have Content-Type defined */
		gchar *ct_cpy = g_malloc (tok->len + 1);

		rspamd_strlcpy (ct_cpy, tok->begin, tok->len + 1);
		ct = g_mime_content_type_new_from_string (ct_cpy);
		g_free (ct_cpy);
	}
	else if (task->cfg->libs_ctx) {
		/* Try to predict it by content (slow) */
		mb = magic_buffer (task->cfg->libs_ctx->libmagic,
				data->data,
				data->len);

		if (mb) {
			ct = g_mime_content_type_new_from_string (mb);
		}
	}

	msg_warn_task ("construct fake mime of type: %s", mb);

	part = g_mime_part_new ();

	if (ct != NULL) {
		g_mime_object_set_content_type (GMIME_OBJECT (part), ct);
		g_object_unref (ct);
	}

#ifdef GMIME24
	wrapper = g_mime_data_wrapper_new_with_stream (stream,
			GMIME_CONTENT_ENCODING_8BIT);
#else
	wrapper = g_mime_data_wrapper_new_with_stream (stream,
				GMIME_PART_ENCODING_8BIT);
#endif

	g_mime_part_set_content_object (part, wrapper);
	g_mime_message_set_mime_part (task->message, GMIME_OBJECT (part));
	/* Register destructors */
	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) g_object_unref, wrapper);
	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) g_object_unref, part);
	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) destroy_message, task->message);

	/* Generate message ID */
	mid = g_mime_utils_generate_message_id ("localhost.localdomain");
	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) g_free, mid);
	g_mime_message_set_message_id (task->message, mid);
	task->message_id = mid;
	task->queue_id = mid;

	/* Set headers for message */
	if (task->subject) {
		g_mime_message_set_subject (task->message, task->subject);
	}
}

gboolean
rspamd_message_parse (struct rspamd_task *task)
{
	GMimeMessage *message;
	GMimeParser *parser;
	GMimeStream *stream;
	GByteArray *tmp;
	GList *first, *cur;
	GMimeObject *parent;
	const GMimeContentType *ct;
	struct raw_header *rh;
	struct mime_text_part *p1, *p2;
	struct mime_foreach_data md;
	struct received_header *recv, *trecv;
	const gchar *p;
	gsize len;
	goffset hdr_pos;
	gint i;
	gdouble diff, *pdiff;
	guint tw, *ptw, dw;

	tmp = rspamd_mempool_alloc (task->task_pool, sizeof (GByteArray));
	p = task->msg.begin;
	len = task->msg.len;

	/* Skip any space characters to avoid some bad messages to be unparsed */
	while (len > 0 && g_ascii_isspace (*p)) {
		p ++;
		len --;
	}

	tmp->data = (guint8 *)p;
	tmp->len = len;

	stream = g_mime_stream_mem_new_with_byte_array (tmp);
	/*
	 * This causes g_mime_stream not to free memory by itself as it is memory allocated by
	 * pool allocator
	 */
	g_mime_stream_mem_set_owner (GMIME_STREAM_MEM (stream), FALSE);

	if (task->flags & RSPAMD_TASK_FLAG_MIME) {

		debug_task ("construct mime parser from string length %d",
				(gint) task->msg.len);
		/* create a new parser object to parse the stream */
		parser = g_mime_parser_new_with_stream (stream);

		/* parse the message from the stream */
		message = g_mime_parser_construct_message (parser);

		if (message == NULL) {
			if (!task->cfg->allow_raw_input) {
				msg_err_task ("cannot construct mime from stream");
				g_set_error (&task->err,
						rspamd_message_quark (),
						RSPAMD_FILTER_ERROR, \

						"cannot parse MIME in the message");
				/* TODO: backport to 0.9 */
				g_object_unref (parser);
				return FALSE;
			}
			else {
				task->flags &= ~RSPAMD_TASK_FLAG_MIME;
				rspamd_message_from_data (task, tmp, stream);
			}
		}
		else {
			GString str;

			task->message = message;
			rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t) destroy_message, task->message);
			str.str = tmp->data;
			str.len = tmp->len;

			hdr_pos = rspamd_string_find_eoh (&str);

			if (hdr_pos > 0 && hdr_pos < tmp->len) {
				task->raw_headers_content.begin = (gchar *) (p);
				task->raw_headers_content.len = (guint64) (hdr_pos);

				if (task->raw_headers_content.len > 0) {
					process_raw_headers (task, task->raw_headers,
							task->raw_headers_content.begin,
							task->raw_headers_content.len);
				}
			}
		}

		/* free the parser (and the stream) */
		g_object_unref (stream);
		g_object_unref (parser);
	}
	else {
		task->flags &= ~RSPAMD_TASK_FLAG_MIME;
		rspamd_message_from_data (task, tmp, stream);
		g_object_unref (stream);
	}


	/* Save message id for future use */
	task->message_id = g_mime_message_get_message_id (task->message);
	if (task->message_id == NULL) {
		task->message_id = "undef";
	}

	memset (&md, 0, sizeof (md));
	md.task = task;
#ifdef GMIME24
	g_mime_message_foreach (task->message, mime_foreach_callback, &md);
#else
	/*
	 * This is rather strange, but gmime 2.2 do NOT pass top-level part to foreach callback
	 * so we need to set up parent part by hands
	 */
	md.parent = g_mime_message_get_mime_part (task->message);
	g_object_unref (md.parent);
	g_mime_message_foreach_part (task->message, mime_foreach_callback, &md);
#endif

	debug_task ("found %ud parts in message", task->parts->len);
	if (task->queue_id == NULL) {
		task->queue_id = "undef";
	}

	rspamd_images_process (task);

	/* Parse received headers */
	first =
			rspamd_message_get_header (task, "Received", FALSE);

	for (cur = first, i = 0; cur != NULL; cur = g_list_next (cur), i ++) {
		recv =
				rspamd_mempool_alloc0 (task->task_pool,
						sizeof (struct received_header));
		parse_recv_header (task->task_pool, cur->data, recv);

		/*
		 * For the first header we must ensure that
		 * received is consistent with the IP that we obtain through
		 * client.
		 */
		if (i == 0) {
			gboolean need_recv_correction = FALSE;

			if (recv->real_ip == NULL || task->cfg->ignore_received) {
				need_recv_correction = TRUE;
			}
			else if (!(task->flags & RSPAMD_TASK_FLAG_NO_IP) && task->from_addr) {
				rspamd_inet_addr_t *raddr = NULL;

				if (!rspamd_parse_inet_address (&raddr, recv->real_ip, 0)) {
					need_recv_correction = TRUE;
				}
				else {
					if (rspamd_inet_address_compare (raddr, task->from_addr) != 0) {
						need_recv_correction = TRUE;
					}

					rspamd_inet_address_destroy (raddr);
				}

			}

			if (need_recv_correction && !(task->flags & RSPAMD_TASK_FLAG_NO_IP)
					&& task->from_addr) {
				msg_debug_task ("the first received seems to be"
						" not ours, replace it with fake one");

				trecv = rspamd_mempool_alloc0 (task->task_pool,
								sizeof (struct received_header));
				trecv->real_ip = rspamd_mempool_strdup (task->task_pool,
						rspamd_inet_address_to_string (task->from_addr));
				trecv->from_ip = trecv->real_ip;

				if (task->hostname) {
					trecv->real_hostname = task->hostname;
					trecv->from_hostname = trecv->real_hostname;
				}
			}
		}

		g_ptr_array_add (task->received, recv);
	}

	/* Extract data from received header if we were not given IP */
	if (task->received->len > 0 && (task->flags & RSPAMD_TASK_FLAG_NO_IP) &&
			!task->cfg->ignore_received) {
		recv = g_ptr_array_index (task->received, 0);
		if (recv->real_ip) {
			if (!rspamd_parse_inet_address (&task->from_addr,
					recv->real_ip,
					0)) {
				msg_warn_task ("cannot get IP from received header: '%s'",
						recv->real_ip);
				task->from_addr = NULL;
			}
		}
		if (recv->real_hostname) {
			task->hostname = recv->real_hostname;
		}
	}

	if (task->from_envelope == NULL) {
		first = rspamd_message_get_header (task, "Return-Path", FALSE);

		if (first) {
			rh = first->data;
			task->from_envelope = internet_address_list_parse_string (rh->value);
			if (task->from_envelope) {
#ifdef GMIME24
				rspamd_mempool_add_destructor (task->task_pool,
						(rspamd_mempool_destruct_t) g_object_unref,
						task->from_envelope);
#else
				rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t) internet_address_list_destroy,
					task->from_envelope);
#endif
			}
		}
	}

	if (task->deliver_to == NULL) {
		first = rspamd_message_get_header (task, "Delivered-To", FALSE);

		if (first) {
			rh = first->data;
			task->deliver_to = rspamd_mempool_strdup (task->task_pool, rh->decoded);
		}
	}

	/* Set mime recipients and sender for the task */
	task->rcpt_mime = g_mime_message_get_all_recipients (task->message);
	if (task->rcpt_mime) {
#ifdef GMIME24
		rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) g_object_unref,
			task->rcpt_mime);
#else
		rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) internet_address_list_destroy,
			task->rcpt_mime);
#endif
	}
	first = rspamd_message_get_header (task, "From", FALSE);

	if (first) {
		rh = first->data;
		task->from_mime = internet_address_list_parse_string (rh->value);
		if (task->from_mime) {
#ifdef GMIME24
			rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t) g_object_unref,
					task->from_mime);
#else
			rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t) internet_address_list_destroy,
					task->from_mime);
#endif
		}
	}

	/* Parse urls inside Subject header */
	cur = rspamd_message_get_header (task, "Subject", FALSE);

	for (; cur != NULL; cur = g_list_next (cur)) {
		rh = cur->data;
		p = rh->decoded;
		len = strlen (p);
		rspamd_url_find_multiple (task->task_pool, p, len, FALSE,
				rspamd_url_task_callback, task);
	}

	/* Calculate distance for 2-parts messages */
	if (task->text_parts->len == 2) {
		p1 = g_ptr_array_index (task->text_parts, 0);
		p2 = g_ptr_array_index (task->text_parts, 1);

		/* First of all check parent object */
		if (p1->parent && p1->parent == p2->parent) {
			parent = p1->parent;
			ct = g_mime_object_get_content_type (parent);
			if (ct == NULL ||
					!g_mime_content_type_is_type ((GMimeContentType *)ct,
							"multipart", "alternative")) {
				debug_task (
						"two parts are not belong to multipart/alternative container, skip check");
			}
			else {
				if (!IS_PART_EMPTY (p1) && !IS_PART_EMPTY (p2) &&
						p1->normalized_words && p2->normalized_words) {

					tw = p1->normalized_words->len + p2->normalized_words->len;

					if (tw > 0) {
						dw = rspamd_words_levenshtein_distance (task,
								p1->normalized_words,
								p2->normalized_words);
						diff = (2.0 * (gdouble)dw) / (gdouble)tw;

						msg_debug_task (
								"different words: %d, total words: %d, "
								"got diff between parts of %.2f",
								dw, tw,
								diff);

						pdiff = rspamd_mempool_alloc (task->task_pool,
								sizeof (gdouble));
						*pdiff = diff;
						rspamd_mempool_set_variable (task->task_pool,
								"parts_distance",
								pdiff,
								NULL);
						ptw = rspamd_mempool_alloc (task->task_pool,
								sizeof (gint));
						*ptw = tw;
						rspamd_mempool_set_variable (task->task_pool,
								"total_words",
								ptw,
								NULL);
					}
				}
			}
		}
		else {
			debug_task (
					"message contains two parts but they are in different multi-parts");
		}
	}
	else {
		debug_task (
				"message has too many text parts, so do not try to compare "
				"them with each other");
	}

	if (task->queue_id) {
		msg_info_task ("loaded message; id: <%s>; queue-id: <%s>",
				task->message_id, task->queue_id);
	}
	else {
		msg_info_task ("loaded message; id: <%s>",
				task->message_id);
	}

	return TRUE;
}

GList *
rspamd_message_get_header (struct rspamd_task *task,
	const gchar *field,
	gboolean strong)
{
	GList *gret = NULL;
	struct raw_header *rh;

	rh = g_hash_table_lookup (task->raw_headers, field);

	if (rh == NULL) {
		return NULL;
	}

	while (rh) {
		if (strong) {
			if (strcmp (rh->name, field) == 0) {
				gret = g_list_prepend (gret, rh);
			}
		}
		else {
			gret = g_list_prepend (gret, rh);
		}
		rh = rh->next;
	}

	if (gret != NULL) {
		gret = g_list_reverse (gret);
		rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t)g_list_free, gret);
	}

	return gret;
}

GPtrArray *
rspamd_message_get_header_array (struct rspamd_task *task,
		const gchar *field,
		gboolean strong)
{
	GPtrArray *ret;
	struct raw_header *rh, *cur;
	guint nelems = 0;

	rh = g_hash_table_lookup (task->raw_headers, field);

	if (rh == NULL) {
		return NULL;
	}

	LL_FOREACH (rh, cur) {
		nelems ++;
	}

	ret = g_ptr_array_sized_new (nelems);

	LL_FOREACH (rh, cur) {
		if (strong) {
			if (strcmp (rh->name, field) != 0) {
				continue;
			}
		}

		g_ptr_array_add (ret, cur);
	}

	rspamd_mempool_add_destructor (task->task_pool,
				(rspamd_mempool_destruct_t)rspamd_ptr_array_free_hard, ret);

	return ret;
}

GPtrArray *
rspamd_message_get_mime_header_array (struct rspamd_task *task,
		const gchar *field,
		gboolean strong)
{
	GPtrArray *ret;
	struct raw_header *rh, *cur;
	guint nelems = 0, i;
	struct mime_part *mp;

	for (i = 0; i < task->parts->len; i ++) {
		mp = g_ptr_array_index (task->parts, i);
		rh = g_hash_table_lookup (mp->raw_headers, field);

		if (rh == NULL) {
			continue;
		}

		LL_FOREACH (rh, cur) {
			nelems ++;
		}
	}

	if (nelems == 0) {
		return NULL;
	}

	ret = g_ptr_array_sized_new (nelems);

	for (i = 0; i < task->parts->len; i ++) {
		mp = g_ptr_array_index (task->parts, i);
		rh = g_hash_table_lookup (mp->raw_headers, field);

		LL_FOREACH (rh, cur) {
			if (strong) {
				if (strcmp (rh->name, field) != 0) {
					continue;
				}
			}

			g_ptr_array_add (ret, cur);
		}
	}

	rspamd_mempool_add_destructor (task->task_pool,
		(rspamd_mempool_destruct_t)rspamd_ptr_array_free_hard, ret);

	return ret;
}
