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

#include "p_spindle-strip.h"

static SPINDLERULES *rulebase;

/* Twine plug-in entry-point */
int
twine_plugin_init(void)
{
	twine_logf(LOG_DEBUG, PLUGIN_NAME " plug-in: initialising\n");
	rulebase = spindle_rulebase_create(NULL, NULL);
	if(!rulebase)
	{
		twine_logf(LOG_CRIT, PLUGIN_NAME ": failed to load rule-base\n");
		return -1;
	}
	twine_preproc_register("spindle", spindle_strip, rulebase);
	twine_graph_register(PLUGIN_NAME, spindle_strip, rulebase);
	return 0;
}

int
twine_plugin_done(void)
{
	twine_logf(LOG_DEBUG, PLUGIN_NAME " plug-in: cleaning up\n");
	if(rulebase)
	{
		spindle_rulebase_destroy(rulebase);
	}
	return 0;
}
