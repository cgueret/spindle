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

#include "p_spindle.h"

struct s3_upload_struct
{
	char *buf;
	size_t bufsize;
	size_t pos;
};

static int spindle_precompose_init_s3_(SPINDLE *spindle, const char *bucketname);
static int spindle_precompose_init_file_(SPINDLE *spindle, const char *path);
static int spindle_precompose_file_(SPINDLECACHE *data, char *quadbuf, size_t bufsize);
static int spindle_precompose_s3_(SPINDLECACHE *data, char *quadbuf, size_t bufsize);
static size_t spindle_precompose_s3_read_(char *buffer, size_t size, size_t nitems, void *userdata);

int
spindle_precompose_init(SPINDLE *spindle)
{
	char *t;
	URI *base, *uri;
	URI_INFO *info;
	int r;
	
	t = twine_config_geta("spindle:cache", NULL);
	if(t)
	{
		base = uri_create_cwd();
		uri = uri_create_str(t, base);
		free(t);
		info = uri_info(uri);
		uri_destroy(uri);
		uri_destroy(base);
		if(!strcmp(info->scheme, "s3"))
		{
			r = spindle_precompose_init_s3_(spindle, info->host);
		}
		else if(!strcmp(info->scheme, "file"))
		{
			r = spindle_precompose_init_file_(spindle, info->path);
		}
		else
		{
			twine_logf(LOG_CRIT, PLUGIN_NAME ": cache type '%s' is not supported\n", info->scheme);
			r = -1;
		}
		uri_info_destroy(info);
		return r;
	}
	/* For compatibility, specifying spindle:bucket=NAME is equivalent to
	 * spindle:cache=s3://NAME
	 */
	t = twine_config_geta("spindle:bucket", NULL);
	if(t)
	{
		r = spindle_precompose_init_s3_(spindle, t);
		free(t);
		return r;
	}
	/* No cache configured */
	return 0;
}

/* Generate a set of pre-composed N-Quads representing an entity we have
 * indexed and write it to a location for the Quilt module to be able to
 * read.
 */

int
spindle_precompose(SPINDLECACHE *data)
{
	char *proxy, *source, *extra, *buf;
	size_t bufsize, proxylen, sourcelen, extralen, l;
	int r;
	
	/* If there's no S3 bucket nor cache-path, this is a no-op */
	if(!data->spindle->bucket &&
		!data->spindle->cachepath)
	{
		return 0;
	}
	if(data->spindle->multigraph)
	{
		/* Remove the root graph from the proxy data model, if it's present */
		librdf_model_context_remove_statements(data->proxydata, data->spindle->rootgraph);
	}
	proxy = twine_rdf_model_nquads(data->proxydata, &proxylen);
	if(!proxy)
	{
		twine_logf(LOG_ERR, PLUGIN_NAME ": failed to serialise proxy model as N-Quads\n");
		return -1;
	}
	source = twine_rdf_model_nquads(data->sourcedata, &sourcelen);
	if(!source)
	{
		twine_logf(LOG_ERR, PLUGIN_NAME ": failed to serialise source model as N-Quads\n");
		librdf_free_memory(proxy);
		return -1;
	}

	extra = twine_rdf_model_nquads(data->extradata, &extralen);
	if(!extra)
	{
		twine_logf(LOG_ERR, PLUGIN_NAME ": failed to serialise extra model as N-Quads\n");
		librdf_free_memory(proxy);
		librdf_free_memory(source);
		return -1;
	}
	bufsize = proxylen + sourcelen + extralen + 3 + 128;
	buf = (char *) calloc(1, bufsize + 1);
	if(!buf)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to allocate buffer for consolidated N-Quads\n");
		librdf_free_memory(proxy);
		librdf_free_memory(source);
		librdf_free_memory(extra);
		return -1;
	}
	strcpy(buf, "## Proxy:\n");
	l = strlen(buf);

	if(proxylen)
	{
		memcpy(&(buf[l]), proxy, proxylen);
	}
	strcpy(&(buf[l + proxylen]), "\n## Source:\n");
	l = strlen(buf);

	if(sourcelen)
	{
		memcpy(&(buf[l]), source, sourcelen);
	}
	strcpy(&(buf[l + sourcelen]), "\n## Extra:\n");
	l = strlen(buf);
	
	if(extralen)
	{
		memcpy(&(buf[l]), extra, extralen);
	}
	strcpy(&(buf[l + extralen]), "\n## End\n");
	bufsize = strlen(buf);

	librdf_free_memory(proxy);
	librdf_free_memory(source);
	librdf_free_memory(extra);
	
	if(data->spindle->bucket)
	{
		r = spindle_precompose_s3_(data, buf, bufsize);
	}
	else if(data->spindle->cachepath)
	{
		r = spindle_precompose_file_(data, buf, bufsize);
	}
	else
	{
		r = 0;
	}
	free(buf);
	return r;
}

/* Initialise an S3-based cache for pre-composed N-Quads */
static int
spindle_precompose_init_s3_(SPINDLE *spindle, const char *bucketname)
{
	char *t;
	
	spindle->bucket = s3_create(bucketname);
	if(!spindle->bucket)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to create S3 bucket object for <s3://%s>\n", bucketname);
		return -1;
	}
	s3_set_logger(spindle->bucket, twine_vlogf);
	if((t = twine_config_geta("s3:endpoint", NULL)))
	{
		s3_set_endpoint(spindle->bucket, t);
		free(t);
	}
	if((t = twine_config_geta("s3:access", NULL)))
	{
		s3_set_access(spindle->bucket, t);
		free(t);
	}
	if((t = twine_config_geta("s3:secret", NULL)))
	{
		s3_set_secret(spindle->bucket, t);
		free(t);
	}
	spindle->s3_verbose = twine_config_get_bool("s3:verbose", 0);
	return 0;
}

/* Initialise a filesystem-based cache for pre-composed N-Quads */
static int
spindle_precompose_init_file_(SPINDLE *spindle, const char *path)
{
	char *t;
	
	if(mkdir(path, 0777))
	{
		if(errno != EEXIST)
		{
			twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to create cache directory: %s: %s\n", path, strerror(errno));
			return -1;
		}
	}
	spindle->cachepath = (char *) calloc(1, strlen(path) + 2);
	if(!spindle->cachepath)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to allocate memory for path buffer\n");
		return -1;
	}
	strcpy(spindle->cachepath, path);
	if(path[0])
	{
		t = strchr(spindle->cachepath, 0);
		t--;
		if(*t != '/')
		{
			t++;
			*t = '/';
			t++;
			*t = 0;
		}
	}
	return 0;
}

/* Store pre-composed N-Quads in an S3 (or RADOS) bucket */
static int
spindle_precompose_s3_(SPINDLECACHE *data, char *quadbuf, size_t bufsize)
{
	char *t;
	char *urlbuf;
	char nqlenstr[256];
	S3REQUEST *req;
	CURL *ch;
	struct curl_slist *headers;
	struct s3_upload_struct s3data;
	int r, e;
	long status;
	
	s3data.buf = quadbuf;
	s3data.bufsize = bufsize;
	urlbuf = (char *) malloc(1 + strlen(data->localname) + 4 + 1);
	if(!urlbuf)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to allocate memory for URL\n");
		return -1;
	}
	urlbuf[0] = '/';
	if((t = strrchr(data->localname, '/')))
	{
		t++;
	}
	else
	{
		t = (char *) data->localname;
	}
	strcpy(&(urlbuf[1]), t);
	if((t = strchr(urlbuf, '#')))
	{
		*t = 0;
	}
	req = s3_request_create(data->spindle->bucket, urlbuf, "PUT");
	ch = s3_request_curl(req);
	curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(ch, CURLOPT_VERBOSE, data->spindle->s3_verbose);
	curl_easy_setopt(ch, CURLOPT_READFUNCTION, spindle_precompose_s3_read_);
	curl_easy_setopt(ch, CURLOPT_READDATA, &s3data);
	curl_easy_setopt(ch, CURLOPT_INFILESIZE, (long) s3data.bufsize);
	curl_easy_setopt(ch, CURLOPT_UPLOAD, 1);
	headers = curl_slist_append(s3_request_headers(req), "Expect: 100-continue");
	headers = curl_slist_append(headers, "Content-Type: application/nquads");
	headers = curl_slist_append(headers, "x-amz-acl: public-read");
	sprintf(nqlenstr, "Content-Length: %u", (unsigned) s3data.bufsize);
	headers = curl_slist_append(headers, nqlenstr);
	s3_request_set_headers(req, headers);
	r = 0;
	if((e = s3_request_perform(req)))
	{
		twine_logf(LOG_ERR, PLUGIN_NAME ": failed to upload N-Quads to bucket at <%s>: %s\n", urlbuf, curl_easy_strerror(e));
		r = -1;
	}
	else
	{
		curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &status);
		if(status != 200)
		{
			twine_logf(LOG_ERR, PLUGIN_NAME ": failed to upload N-Quads to bucket at <%s> (HTTP status %ld)\n", urlbuf, status);
			r = -1;
		}
	}
	s3_request_destroy(req);
	free(urlbuf);
	return r;
}

static size_t
spindle_precompose_s3_read_(char *buffer, size_t size, size_t nitems, void *userdata)
{
	struct s3_upload_struct *data;

	data = (struct s3_upload_struct *) userdata;
	size *= nitems;
	if(size > data->bufsize - data->pos)
	{
		size = data->bufsize - data->pos;
	}
	memcpy(buffer, &(data->buf[data->pos]), size);
	data->pos += size;
	return size;
}

/* Store pre-composed N-Quads in a file */
static int
spindle_precompose_file_(SPINDLECACHE *data, char *quadbuf, size_t bufsize)
{
	const char *s;
	char *path, *t;
	FILE *f;
	int r;
	
	path = (char *) calloc(1, strlen(data->spindle->cachepath) + strlen(data->localname) + 8);
	if(!path)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to allocate memory for local path buffer\n");
		return -1;
	}
	strcpy(path, data->spindle->cachepath);
	if((s = strrchr(data->localname, '/')))
	{
		s++;
	}
	else
	{
		s = (char *) data->localname;
	}
	strcat(path, s);
	t = strchr(path, '#');
	if(t)
	{
		*t = 0;
	}
	f = fopen(path, "wb");
	if(!f)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to open cache file for writing: %s: %s\n", path, strerror(errno));
		free(path);
		return -1;
	}
	r = 0;
	while(bufsize)
	{
		if(bufsize > 1024)
		{
			if(fwrite(quadbuf, 1024, 1, f) != 1)
			{
				r = -1;
				break;
			}
			quadbuf += 1024;
			bufsize -= 1024;
		}
		else
		{
			if(fwrite(quadbuf, bufsize, 1, f) != 1)
			{
				r = -1;
				break;
			}
			bufsize = 0;
		}
	}
	if(r)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to write to cache file: %s: %s\n", path, strerror(errno));
		fclose(f);
		free(path);
		return -1;
	}
	fclose(f);
	free(path);
	return 0;
}