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
#ifndef SRC_LIBMIME_CONTENT_TYPE_H_
#define SRC_LIBMIME_CONTENT_TYPE_H_

#include "config.h"
#include "libutil/fstring.h"
#include "libutil/mem_pool.h"

struct rspamd_content_type_param {
	rspamd_ftok_t name;
	rspamd_ftok_t value;
	struct rspamd_content_type_param *prev, *next;
};

struct rspamd_content_type {
	gchar *lc_data;
	rspamd_ftok_t type;
	rspamd_ftok_t subtype;
	rspamd_ftok_t charset;
	GHashTable *attrs; /* Can be empty */
};

/**
 * Adds new parameter to content type structure
 * @param ct
 * @param name_start
 * @param name_end
 * @param value_start
 * @param value_end
 */
void
rspamd_content_type_add_param (rspamd_mempool_t *pool,
		struct rspamd_content_type *ct,
		const gchar *name_start, const gchar *name_end,
		const gchar *value_start, const gchar *value_end);

/**
 * Parse content type from the header (performs copy + lowercase)
 * @param in
 * @param len
 * @param pool
 * @return
 */
struct rspamd_content_type * rspamd_content_type_parse (const gchar *in,
		gsize len, rspamd_mempool_t *pool);

#endif /* SRC_LIBMIME_CONTENT_TYPE_H_ */
