/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#include <sched.h> /* IWYU pragma: keep */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNIT_TESTING
#include <cmocka.h>

#include <isc/atomic.h>
#include <isc/buffer.h>
#include <isc/loop.h>
#include <isc/task.h>
#include <isc/timer.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/name.h>
#include <dns/view.h>
#include <dns/zone.h>
#include <dns/zt.h>

#include <tests/dns.h>

static dns_db_t *db = NULL;
static FILE *zonefile, *origfile;
static dns_view_t *view = NULL;

static isc_result_t
count_zone(dns_zone_t *zone, void *uap) {
	int *nzones = (int *)uap;

	UNUSED(zone);

	*nzones += 1;
	return (ISC_R_SUCCESS);
}

/* apply a function to a zone table */
ISC_LOOP_TEST_IMPL(apply) {
	isc_result_t result;
	dns_zone_t *zone = NULL;
	int nzones = 0;

	result = dns_test_makezone("foo", &zone, NULL, true);
	assert_int_equal(result, ISC_R_SUCCESS);

	view = dns_zone_getview(zone);
	assert_non_null(view->zonetable);

	assert_int_equal(nzones, 0);
	result = dns_zt_apply(view->zonetable, isc_rwlocktype_read, false, NULL,
			      count_zone, &nzones);
	assert_int_equal(result, ISC_R_SUCCESS);
	assert_int_equal(nzones, 1);

	/* These steps are necessary so the zone can be detached properly */
	result = dns_test_setupzonemgr();
	assert_int_equal(result, ISC_R_SUCCESS);
	result = dns_test_managezone(zone);
	assert_int_equal(result, ISC_R_SUCCESS);
	dns_test_releasezone(zone);
	dns_test_closezonemgr();

	/* The view was left attached in dns_test_makezone() */
	dns_view_detach(&view);
	dns_zone_detach(&zone);
	isc_loopmgr_shutdown(loopmgr);
}

static isc_result_t
load_done_last(dns_zt_t *zt, dns_zone_t *zone, isc_task_t *task) {
	isc_result_t result;

	UNUSED(zt);
	UNUSED(zone);
	UNUSED(task);

	/* The zone should now be loaded; test it */
	result = dns_zone_getdb(zone, &db);
	assert_int_equal(result, ISC_R_SUCCESS);

	assert_non_null(db);
	if (db != NULL) {
		dns_db_detach(&db);
	}

	dns_test_releasezone(zone);
	dns_test_closezonemgr();

	dns_zone_detach(&zone);
	dns_view_detach(&view);

	isc_loopmgr_shutdown(loopmgr);

	return (ISC_R_SUCCESS);
}

static isc_result_t
load_done_new_only(dns_zt_t *zt, dns_zone_t *zone, isc_task_t *task) {
	isc_result_t result;

	UNUSED(zt);
	UNUSED(zone);
	UNUSED(task);

	/* The zone should now be loaded; test it */
	result = dns_zone_getdb(zone, &db);
	assert_int_equal(result, ISC_R_SUCCESS);
	dns_db_detach(&db);

	dns_zone_asyncload(zone, true, load_done_last, NULL);

	return (ISC_R_SUCCESS);
}

static isc_result_t
load_done_first(dns_zt_t *zt, dns_zone_t *zone, isc_task_t *task) {
	atomic_bool *done = (atomic_bool *)zt;
	isc_result_t result;

	UNUSED(zone);
	UNUSED(task);

	/* The zone should now be loaded; test it */
	result = dns_zone_getdb(zone, &db);
	assert_int_equal(result, ISC_R_SUCCESS);
	dns_db_detach(&db);

	/*
	 * Add something to zone file, reload zone with newonly - it should
	 * not be reloaded.
	 */
	fprintf(zonefile, "\nb in b 1.2.3.4\n");
	fflush(zonefile);
	fclose(zonefile);

	dns_zone_asyncload(zone, true, load_done_new_only, &done);

	return (ISC_R_SUCCESS);
}

/* asynchronous zone load */
ISC_LOOP_TEST_IMPL(asyncload_zone) {
	isc_result_t result;
	int n;
	dns_zone_t *zone = NULL;
	char buf[4096];
	atomic_bool done;

	atomic_init(&done, false);

	result = dns_test_makezone("foo", &zone, NULL, true);
	assert_int_equal(result, ISC_R_SUCCESS);

	result = dns_test_setupzonemgr();
	assert_int_equal(result, ISC_R_SUCCESS);
	result = dns_test_managezone(zone);
	assert_int_equal(result, ISC_R_SUCCESS);

	view = dns_zone_getview(zone);
	assert_non_null(view->zonetable);

	assert_false(dns__zone_loadpending(zone));
	assert_false(atomic_load(&done));
	zonefile = fopen("./zone.data", "wb");
	assert_non_null(zonefile);
	origfile = fopen(TESTS_DIR "/testdata/zt/zone1.db", "r+b");
	assert_non_null(origfile);
	n = fread(buf, 1, 4096, origfile);
	fclose(origfile);
	fwrite(buf, 1, n, zonefile);
	fflush(zonefile);

	dns_zone_setfile(zone, "./zone.data", dns_masterformat_text,
			 &dns_master_style_default);

	dns_zone_asyncload(zone, false, load_done_first, &done);
}

dns_zone_t *zone1 = NULL, *zone2 = NULL, *zone3 = NULL;

static isc_result_t
all_done(void *arg __attribute__((__unused__))) {
	isc_result_t result;

	/* Both zones should now be loaded; test them */
	result = dns_zone_getdb(zone1, &db);
	assert_int_equal(result, ISC_R_SUCCESS);
	assert_non_null(db);
	if (db != NULL) {
		dns_db_detach(&db);
	}

	result = dns_zone_getdb(zone2, &db);
	assert_int_equal(result, ISC_R_SUCCESS);
	assert_non_null(db);
	if (db != NULL) {
		dns_db_detach(&db);
	}

	dns_test_releasezone(zone3);
	dns_test_releasezone(zone2);
	dns_test_releasezone(zone1);
	dns_test_closezonemgr();

	dns_zone_detach(&zone1);
	dns_zone_detach(&zone2);
	dns_zone_detach(&zone3);
	dns_view_detach(&view);

	isc_loopmgr_shutdown(loopmgr);
	return (ISC_R_SUCCESS);
}

/* asynchronous zone table load */
ISC_LOOP_TEST_IMPL(asyncload_zt) {
	isc_result_t result;
	dns_zt_t *zt = NULL;
	atomic_bool done;

	atomic_init(&done, false);

	result = dns_test_makezone("foo", &zone1, NULL, true);
	assert_int_equal(result, ISC_R_SUCCESS);
	dns_zone_setfile(zone1, TESTS_DIR "/testdata/zt/zone1.db",
			 dns_masterformat_text, &dns_master_style_default);
	view = dns_zone_getview(zone1);

	result = dns_test_makezone("bar", &zone2, view, false);
	assert_int_equal(result, ISC_R_SUCCESS);
	dns_zone_setfile(zone2, TESTS_DIR "/testdata/zt/zone1.db",
			 dns_masterformat_text, &dns_master_style_default);

	/* This one will fail to load */
	result = dns_test_makezone("fake", &zone3, view, false);
	assert_int_equal(result, ISC_R_SUCCESS);
	dns_zone_setfile(zone3, TESTS_DIR "/testdata/zt/nonexistent.db",
			 dns_masterformat_text, &dns_master_style_default);

	zt = view->zonetable;
	assert_non_null(zt);

	result = dns_test_setupzonemgr();
	assert_int_equal(result, ISC_R_SUCCESS);
	result = dns_test_managezone(zone1);
	assert_int_equal(result, ISC_R_SUCCESS);
	result = dns_test_managezone(zone2);
	assert_int_equal(result, ISC_R_SUCCESS);
	result = dns_test_managezone(zone3);
	assert_int_equal(result, ISC_R_SUCCESS);

	assert_false(dns__zone_loadpending(zone1));
	assert_false(dns__zone_loadpending(zone2));
	assert_false(atomic_load(&done));

	dns_zt_asyncload(zt, false, all_done, NULL);
}

ISC_TEST_LIST_START
ISC_TEST_ENTRY_CUSTOM(apply, setup_managers, teardown_managers)
ISC_TEST_ENTRY_CUSTOM(asyncload_zone, setup_managers, teardown_managers)
ISC_TEST_ENTRY_CUSTOM(asyncload_zt, setup_managers, teardown_managers)
ISC_TEST_LIST_END

ISC_TEST_MAIN
