/* Spindle: Co-reference aggregation engine
 *
 * Author: Mo McRoberts <mo.mcroberts@bbc.co.uk>
 *
 * Copyright (c) 2014-2015 BBC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "p_spindle-generate.h"

struct licenseentry_struct
{
	char *source;
	char *uri;
	struct spindle_license_struct *license;
	size_t len;
	const char *name;
	int score;
};

struct licenselist_struct
{
	struct licenseentry_struct *entries;
	size_t nentries;
};

static int spindle_license_apply_context_(SPINDLEENTRY *cache, struct licenselist_struct *list, librdf_node *context, librdf_node *licenseentry, const char *licenseentryname);
static int spindle_license_apply_st_(SPINDLEENTRY *cache, librdf_node *graph, const char *graphname, librdf_statement *statement, librdf_node *source, const char *sourcename, struct licenselist_struct *list);
static int spindle_license_cb_(const char *section, const char *key, void *data);
static struct spindle_license_struct *spindle_license_add_(SPINDLEGENERATE *generate, const char *name, size_t namelen);
static int spindle_license_add_uri_(struct spindle_license_struct *license, const char *uri);
static struct licenseentry_struct *spindle_license_list_add_(struct licenselist_struct *list, const char *uri, const char *source, struct spindle_license_struct *license);
static int spindle_license_list_free_(struct licenselist_struct *list);
static int spindle_license_label_(SPINDLEENTRY *cache, librdf_node *subject);
static int spindle_license_apply_list_(SPINDLEENTRY *cache, struct licenselist_struct *list, librdf_node *subject);

int
spindle_license_init(SPINDLEGENERATE *generate)
{
	char *pred;
	size_t c;

	pred = twine_config_geta("spindle:predicates:license", NS_DCTERMS "rights");
	for(c = 0; c < generate->rules->predcount; c++)
	{
		if(!strcmp(generate->rules->predicates[c].target, pred))
		{
			generate->licensepred = &(generate->rules->predicates[c]);
			break;
		}
	}
	if(!generate->licensepred)
	{
		twine_logf(LOG_DEBUG, PLUGIN_NAME ": failed to locate licensing predicate <%s> in rulebase\n", generate->licensepred);
	}
	free(pred);
	twine_config_get_all(NULL, NULL, spindle_license_cb_, (void *) generate);
	return 0;
}

/* Add information about a licence to the list */
static int
spindle_license_cb_(const char *key, const char *value, void *data)
{
	struct spindle_license_struct *entry;
	const char *section;
	SPINDLEGENERATE *generate;
	int i;

	generate = (SPINDLEGENERATE *) data;
	if(!value || strncmp(key, "spindle:licenses:", 17))
	{
		return 0;
	}
	section = key + 17;
	key = strchr(section, ':');
	if(!key)
	{
		return 0;
	}
	entry = spindle_license_add_(generate, section, key - section);
	if(!entry)
	{
		return -1;
	}
	key++;
	if(!strcmp(key, "title"))
	{
		if(entry->title)
		{
			twine_logf(LOG_WARNING, PLUGIN_NAME ": ignoring title '%s' for license %s which already has a title of '%s'\n", value, entry->name, entry->title);
		}
		else
		{
			entry->title = strdup(value);
			if(!entry->title)
			{
				twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to allocate memory for license title\n");
				return -1;
			}
		}
	}
	else if(!strcmp(key, "uri"))
	{
		if(spindle_license_add_uri_(entry, value))
		{
			return -1;
		}
	}
	else if(!strcmp(key, "score"))
	{
		i = atoi(value);
		if(i <= 0)
		{
			twine_logf(LOG_WARNING, PLUGIN_NAME ": invalid score for license %s: '%s'\n", entry->name, value);
		}
		else
		{
			entry->score = i;
		}
	}
	else
	{
		twine_logf(LOG_WARNING, PLUGIN_NAME ": ignoring unknown configuration key '%s' for license %s\n", key, entry->name);
	}
	return 0;
}

/* Add a license entry to the context */
static struct spindle_license_struct *
spindle_license_add_(SPINDLEGENERATE *generate, const char *name, size_t namelen)
{
	size_t c;
	struct spindle_license_struct *p;

	for(c = 0; c < generate->nlicenses; c++)
	{
		if(strlen(generate->licenses[c].name) == namelen &&
		   !strncmp(generate->licenses[c].name, name, namelen))
		{
			return &(generate->licenses[c]);
		}
	}
	p = (struct spindle_license_struct *) realloc(generate->licenses, sizeof(struct spindle_license_struct) * (generate->nlicenses + 1));
	if(!p)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to expand license list\n");
		return NULL;
	}
	generate->licenses = p;
	p = &(generate->licenses[generate->nlicenses]);
	memset(p, 0, sizeof(struct spindle_license_struct));
	p->name = (char *) malloc(namelen + 1);
	if(!p)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to allocate memory for license name\n");
		return NULL;
	}
	strncpy(p->name, name, namelen);
	p->name[namelen] = 0;
	/* A known license will always have a non-zero score */
	p->score = 1;
	generate->nlicenses++;
	return p;
}

static int 
spindle_license_add_uri_(struct spindle_license_struct *license, const char *uri)
{
	char **p;

	p = (char **) realloc(license->uris, sizeof(char *) * (license->uricount + 1));
	if(!p)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to resize license URI list\n");
		return -1;
	}
	license->uris = p;
	license->uris[license->uricount] = strdup(uri);
	if(!license->uris[license->uricount])
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to duplicate license URI\n");
		return -1;
	}
	license->uricount++;
	return 0;
}
	
/* For each of the source documents, find the licensing predicates and
 * add information about them to a licensing entry related to the proxy
 */
int
spindle_license_apply(SPINDLEENTRY *cache)
{	
	librdf_iterator *iter;
	librdf_node *node, *licenseentry;
	char *licenseentryname;
	struct licenselist_struct list;
	int r;

	memset(&list, 0, sizeof(struct licenselist_struct));
	licenseentryname = (char *) calloc(1, strlen(cache->docname) + 16);
	strcpy(licenseentryname, cache->docname);
	strcat(licenseentryname, "#rights");
	licenseentry = twine_rdf_node_createuri(licenseentryname);
	r = 0;

	iter = librdf_model_get_contexts(cache->sourcedata);
	for(; !librdf_iterator_end(iter); librdf_iterator_next(iter))
	{
		node = librdf_iterator_get_object(iter);
		if((r = spindle_license_apply_context_(cache, &list, node, licenseentry, licenseentryname)))
		{
			break;
		}
	}
	librdf_free_iterator(iter);
	
	if(!r)
	{
		r = spindle_license_apply_list_(cache, &list, licenseentry);
	}
	spindle_license_list_free_(&list);
	librdf_free_node(licenseentry);
	free(licenseentryname);
	return 0;
}

/* Look for licensing triples within an individual named graph */
static int
spindle_license_apply_context_(SPINDLEENTRY *cache, struct licenselist_struct *list, librdf_node *context, librdf_node *licenseentry, const char *licenseentryname)
{
	librdf_node *predicate, *object;
	librdf_uri *uri, *pred;	
	librdf_statement *query, *statement;
	librdf_stream *stream;
	const char *uristr, *preduri;
	size_t c;

	uri = librdf_node_get_uri(context);
	uristr = (const char *) librdf_uri_as_string(uri);
	query = twine_rdf_st_create();
	librdf_statement_set_subject(query, librdf_new_node_from_node(context));
	stream = librdf_model_find_statements_with_options(cache->sourcedata, query, context, NULL);
	for(; !librdf_stream_end(stream); librdf_stream_next(stream))
	{
		statement = librdf_stream_get_object(stream);
		predicate = librdf_statement_get_predicate(statement);
		object = librdf_statement_get_object(statement);
		if(!librdf_node_is_resource(predicate) || !librdf_node_is_resource(object))
		{
			continue;
		}
		pred = librdf_node_get_uri(predicate);
		preduri = (const char *) librdf_uri_as_string(pred);
		for(c = 0; c < cache->generate->licensepred->matchcount; c++)
		{
			if(cache->generate->licensepred->matches[c].onlyfor &&
			   strcmp(cache->generate->licensepred->matches[c].onlyfor,
					  NS_FOAF "Document"))
			{
				continue;
			}
			if(!strcmp(preduri, cache->generate->licensepred->matches[c].predicate))
			{
				if(spindle_license_apply_st_(cache, licenseentry, licenseentryname, statement, context, uristr, list))
				{
					librdf_free_stream(stream);
					librdf_free_statement(query);
					return -1;
				}
				break;
			}
		}
	}
	librdf_free_stream(stream);
	librdf_free_statement(query);
	return 0;
}

/* Take a list of licensing information and reflect it in the licensing graph */
static int
spindle_license_apply_list_(SPINDLEENTRY *cache, struct licenselist_struct *list, librdf_node *subject)
{
	size_t c, buflen;
	char *buf, *p;
	librdf_statement *statement;

	if(!list->nentries)
	{
		return 0;
	}
	buflen = 64;
	for(c = 0; c < list->nentries; c++)
	{
		buflen += 32 + list->entries[c].len;
	}
	buf = (char *) calloc(1, buflen);
	if(!buf)
	{
		twine_logf(LOG_CRIT, "failed to allocate licensing description buffer\n");
		return -1;
	}
	strcpy(buf, "Incorporates data ");
	p = strchr(buf, 0);
	for(c = 0; c < list->nentries; c++)
	{
		if(c && c + 1 == list->nentries)
		{
			strcpy(p, " and ");
			p += 5;
		}
		else if(c)
		{
			*p = ',';
			p++;
			*p = ' ';
			p++;
		}								
		p += sprintf(p, "from %s under the terms of %s",
					 list->entries[c].source, list->entries[c].name);
	}
	
	statement = twine_rdf_st_create();
	librdf_statement_set_subject(statement, librdf_new_node_from_node(subject));
	librdf_statement_set_predicate(statement, twine_rdf_node_createuri(NS_RDFS "comment"));
	librdf_statement_set_object(statement, librdf_new_node_from_literal(cache->spindle->world, (const unsigned char *) buf, "en", 0));
	twine_rdf_model_add_st(cache->proxydata, statement, cache->graph);
	librdf_free_statement(statement);
	
	statement = twine_rdf_st_create();
	librdf_statement_set_subject(statement, librdf_new_node_from_node(cache->doc));
	librdf_statement_set_predicate(statement, twine_rdf_node_createuri(NS_DCTERMS "rights"));
	librdf_statement_set_object(statement, librdf_new_node_from_node(subject));
	twine_rdf_model_add_st(cache->proxydata, statement, cache->graph);
	librdf_free_statement(statement);
	free(buf);
	
	return spindle_license_label_(cache, subject);
}

/* Add a label to the licensing graph */
static int
spindle_license_label_(SPINDLEENTRY *cache, librdf_node *subject)
{
	librdf_node *obj;
	librdf_statement *st;
	char *strbuf;
	const char *s;

	if(cache->title_en)
	{
		s = cache->title_en;
	}
	else
	{
		s = cache->title;
	}
	strbuf = (char *) calloc(1, strlen(s) + 64);
	if(!strbuf)
	{
		return -1;
	}
	sprintf(strbuf, "Rights information for '%s'", s);
	st = twine_rdf_st_create();
	if(!st)
	{
		free(strbuf);
		return -1;
	}
	obj = twine_rdf_node_clone(cache->doc);
	if(!obj)
	{
		twine_rdf_st_destroy(st);
		free(strbuf);
		return -1;
	}
	librdf_statement_set_subject(st, librdf_new_node_from_node(subject));
	obj = twine_rdf_node_createuri(NS_RDFS "label");
	if(!obj)
	{
		twine_rdf_st_destroy(st);
		free(strbuf);
		return -1;
	}
	librdf_statement_set_predicate(st, obj);
	obj = librdf_new_node_from_literal(cache->spindle->world, (const unsigned char *) strbuf, "en", 0);
	if(!obj)
	{
		twine_rdf_st_destroy(st);
		free(strbuf);
		return -1;
	}
	librdf_statement_set_object(st, obj);
	twine_rdf_model_add_st(cache->proxydata, st, cache->graph);
	twine_rdf_st_destroy(st);
	free(strbuf);
	return 0;
}

/* Invoked when a licensing predicate is encountered in a source document
 * graph.
 */
static int
spindle_license_apply_st_(SPINDLEENTRY *cache, librdf_node *graph, const char *graphname, librdf_statement *statement, librdf_node *source, const char *sourcename, struct licenselist_struct *list)
{
	librdf_node *object;
	librdf_uri *obj;
	const char *objuri;
	librdf_statement *st;
	struct spindle_license_struct *license;
	size_t c, d;
	URI *uri;
	URI_INFO *info;

	(void) source;
	(void) graphname;

	uri = uri_create_str(sourcename, NULL);
	if(uri)
	{
		info = uri_info(uri);
		if(!info || 
		   !info->host || !info->host[0] ||
		   !info->scheme || !info->scheme[0] ||
		   (strcasecmp(info->scheme, "http") &&
			strcasecmp(info->scheme, "https") &&
			strcasecmp(info->scheme, "ftp") &&
			strcasecmp(info->scheme, "ftps")))
		{
			uri_info_destroy(info);
			uri_destroy(uri);
			info = NULL;
			uri = NULL;
		}
	}
	else
	{
		info = NULL;
	}
	object = librdf_statement_get_object(statement);
	obj = librdf_node_get_uri(object);
	objuri = (const char *) librdf_uri_as_string(obj);

	license = NULL;
	for(c = 0; c < cache->generate->nlicenses; c++)
	{
		for(d = 0; d < cache->generate->licenses[c].uricount; d++)
		{
			if(!strcmp(objuri, cache->generate->licenses[c].uris[d]))
			{
				license = &(cache->generate->licenses[c]);
				break;
			}
		}
		if(license)
		{
			break;
		}
	}
	spindle_license_list_add_(list, objuri, info ? info->host : sourcename, license);
	/* Add <#license> rdfs:seeAlso <http://example.com/license> . */
	st = twine_rdf_st_create();
	librdf_statement_set_subject(st, librdf_new_node_from_node(graph));
	librdf_statement_set_predicate(st, twine_rdf_node_createuri(NS_RDFS "seeAlso"));
	librdf_statement_set_object(st, librdf_new_node_from_node(object));
	twine_rdf_model_add_st(cache->proxydata, st, cache->graph);
	twine_rdf_st_destroy(st);

	if(uri)
	{
		uri_info_destroy(info);
		uri_destroy(uri);
	}
	return 0;
}

/* Add a licensing entry to the discovered licensing information list.
 * - uri is the full URI of the license itself
 * - source is the hostname (if it can be determined) or full URI (otherwise)
 *   of the document to which the license applies
 * - if uri refers to a known license, then license will point to its entry
 *   in the global license list.
 */
static struct licenseentry_struct *
spindle_license_list_add_(struct licenselist_struct *list, const char *uri, const char *source, struct spindle_license_struct *license)
{
	size_t c;
	struct licenseentry_struct *p;
	char *s;

	for(c = 0; c < list->nentries; c++)
	{
		if(!strcmp(list->entries[c].source, source))
		{
			/* Only replace an entry with one of a higher score */
			if(license && license->score > list->entries[c].score)
			{
				p = &(list->entries[c]);
				/* A known item replaces an unknown one */
				s = strdup(uri);
				if(!s)
				{
					twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to duplicate license URI\n");
					return NULL;
				}
				free(p->uri);
				p->uri = s;
				p->license = license;
				p->name = license->title ? license->title : license->name;
				p->len = strlen(p->source) + strlen(p->name);
				p->score = license->score;
			}
			return &(list->entries[c]);
		}
	}
	p = (struct licenseentry_struct *) realloc(list->entries, sizeof(struct licenseentry_struct) * (list->nentries + 1));
	if(!p)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to expand license structure\n");
		return NULL;
	}
	list->entries = p;
	p = &(list->entries[list->nentries]);
	memset(p, 0, sizeof(struct licenseentry_struct));
	p->source = strdup(source);
	if(!p->source)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to duplicate source URI\n");
		return NULL;
	}
	p->uri = strdup(uri);
	if(!p->uri)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to duplicate license URI\n");
		free(p->source);
		return NULL;
	}
	p->license = license;
	if(license)
	{
		p->name = license->title ? license->title : license->name;
		p->score = license->score;
	}
	else
	{
		p->name = p->uri;
	}
	p->len = strlen(p->source) + strlen(p->name);
	list->nentries++;
	return p;
}

static int
spindle_license_list_free_(struct licenselist_struct *list)
{
	size_t c;

	for(c = 0; c < list->nentries; c++)
	{
		free(list->entries[c].source);
		free(list->entries[c].uri);
	}
	free(list->entries);
	memset(list, 0, sizeof(struct licenselist_struct));
	return 0;
}
