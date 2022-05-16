// SPDX-License-Identifier: GPL-2.0
/* Copyright 2020 NXP */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "common.h"
#include "isochron.h"

typedef int isochron_prog_main_func_t(int argc, char *argv[]);

struct isochron_prog {
	const char *prog_name;
	const char *prog_func;
	isochron_prog_main_func_t *main;
};

static const struct isochron_prog progs[] = {
	{
		.prog_name = "isochron-send",
		.prog_func = "send",
		.main = isochron_send_main,
	}, {
		.prog_name = "isochron-rcv",
		.prog_func = "rcv",
		.main = isochron_rcv_main,
	}, {
		.prog_name = "isochron-report",
		.prog_func = "report",
		.main = isochron_report_main,
	},
};

static void isochron_usage(void)
{
	size_t i;

	fprintf(stderr, "isochron usage:\n");

	for (i = 0; i < ARRAY_SIZE(progs); i++)
		fprintf(stderr, "isochron %s ...\n", progs[i].prog_func);

	fprintf(stderr, "Run ");

	for (i = 0; i < ARRAY_SIZE(progs); i++)
		fprintf(stderr, "\"isochron %s --help\", ", progs[i].prog_func);

	fprintf(stderr, "for more details.\n");
}

static int isochron_parse_args(int *argc, char ***argv,
			       const struct isochron_prog **prog)
{
	char *prog_name;
	char *prog_func;
	size_t i;

	if (*argc < 2) {
		isochron_usage();
		return -EINVAL;
	}

	/* First try to match on program name */
	prog_name = *argv[0];
	(*argv)++;
	(*argc)--;

	for (i = 0; i < ARRAY_SIZE(progs); i++) {
		if (strcmp(prog_name, progs[i].prog_name) == 0) {
			*prog = &progs[i];
			return 0;
		}
	}

	/* Next try to match on function name */
	prog_func = (*argv)[0];
	(*argv)++;
	(*argc)--;

	if (!strcmp(prog_func, "-V") || !strcmp(prog_func, "--version")) {
		fprintf(stderr, "%s\n", VERSION);
		return -EINVAL;
	}

	if (!strcmp(prog_func, "-h") || !strcmp(prog_func, "--help")) {
		isochron_usage();
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(progs); i++) {
		if (strcmp(prog_func, progs[i].prog_func) == 0) {
			*prog = &progs[i];
			return 0;
		}
	}

	fprintf(stderr, "%s: unknown function %s, expected one of ",
		prog_name, prog_func);

	for (i = 0; i < ARRAY_SIZE(progs); i++)
		fprintf(stderr, "\"%s\", ", progs[i].prog_func);

	fprintf(stderr, "\n");

	return -EINVAL;
}

int main(int argc, char *argv[])
{
	const struct isochron_prog *prog;
	int rc;

	rc = isochron_parse_args(&argc, &argv, &prog);
	if (rc)
		return -rc;

	return prog->main(argc, argv);
}
