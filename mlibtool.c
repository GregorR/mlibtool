/* A miniscule version of libtool for sane systems. On insane systems, requires
 * that true libtool be installed. */

/*
 * Copyright (c) 2013 Gregor Richards
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _XOPEN_SOURCE 500

#define SF(into, func, bad, args) do { \
    (into) = func args; \
    if ((into) == (bad)) { \
        perror(#func); \
        exit(1); \
    } \
} while (0)

#define SANE_HEADER "# SYSTEM_IS_SANE\n"

/* make sure we're POSIX */
#if defined(unix) || defined(__unix) || defined(__unix__)
#include <unistd.h>
#endif

#ifndef _POSIX_VERSION
/* not even POSIX, this system can't possibly be sane */
int execv(const char *path, char *const argv[]);
int main(int argc, char **argv)
{
    int tmpi;
    SF(tmpi, execv, -1, (argv[1], argv + 1));
    return 1;
}
#else

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define SANE "__linux__ || " /* Linux */ \
             "__FreeBSD_kernel__ || __NetBSD__ || " \
             "__OpenBSD__ || __DragonFly__ || " /* BSD family */ \
             "__GNU__" /* GNU Hurd */

/* Is this system sane? */
static int systemIsSane(char *cc)
{
    pid_t pid;
    int pipei[2], pipeo[2];
    int tmpi, sane, insane = 0;
    size_t i, bufused;
    ssize_t rd;
#define BUFSZ 32
    char buf[BUFSZ];

    /* We determine if it's sane by asking the preprocessor */
    static const char *sanityCheck =
        "#if " SANE "\n"
        "SYSTEM_IS_SANE\n"
        "#endif";

    SF(tmpi, pipe, -1, (pipei));
    SF(tmpi, pipe, -1, (pipeo));
    SF(pid, fork, -1, ());
    if (pid == 0) {
        /* child process, read in preproc commands */
        SF(tmpi, dup2, -1, (pipei[0], 0));
        close(pipei[0]); close(pipei[1]);
        SF(tmpi, dup2, -1, (pipeo[1], 1));
        close(pipeo[0]); close(pipeo[1]);

        /* and spawn the preprocessor */
        SF(tmpi, execlp, -1, (cc, cc, "-E", "-", NULL));
        exit(1);
    }
    close(pipei[0]);
    close(pipeo[1]);

    /* now send it the check */
    i = strlen(sanityCheck);
    if (write(pipei[1], sanityCheck, i) != i)
        insane = 1;
    close(pipei[1]);

    /* and read its input */
    sane = 0;
    bufused = 0;
    while ((rd = read(pipeo[0], buf + bufused, BUFSZ - bufused)) > 0 ||
           bufused) {
        if (rd > 0)
            bufused += rd;

        if (!strncmp(buf, "SYSTEM_IS_SANE", 14))
            sane = 1;

        for (i = 0; i < bufused && buf[i] != '\n'; i++);
        if (i < bufused) i++;
        memcpy(buf, buf + i, bufused - i);
        bufused -= i;
    }
    close(pipeo[0]);

    /* then wait for it */
    if (waitpid(pid, &tmpi, 0) != pid)
        insane = 1;
    if (tmpi != 0)
        insane = 1;

    /* finished */
    if (insane) sane = 0;
    return sane;
}

enum Mode {
    MODE_UNKNOWN = 0,
    MODE_COMPILE,
    MODE_LINK,
    MODE_INSTALL
};


struct Options {
    int dryRun;
    char argc, **argv, **cmd;
};

/* generic function to spawn a child and wait for it, failing if the child fails */
static void spawn(struct Options *opt, char *const *cmd)
{
    size_t i;

    /* output the command */
    fprintf(stderr, "mlibtool:");
    for (i = 0; cmd[i]; i++)
        fprintf(stderr, " %s", cmd[i]);
    fprintf(stderr, "\n");

    if (!opt->dryRun) {
        pid_t pid;
        int tmpi;
        SF(pid, fork, -1, ());
        if (pid == 0) {
            SF(tmpi, execvp, -1, (cmd[0], cmd));
            exit(1);
        }
        if (waitpid(pid, &tmpi, 0) != pid) {
            perror(cmd[0]);
            exit(1);
        }
        if (tmpi != 0)
            exit(1);
    }
}

/* check for sanity by reading a .lo file */
static int checkLoSanity(struct Options *opt)
{
    int sane = 0;
    size_t i;

    /* look for a .lo file and check it for sanity */
    for (i = 1; opt->cmd[i]; i++) {
        char *arg = opt->cmd[i];
        if (arg[0] != '-') {
            char *ext = strrchr(arg, '.');
            if (ext && !strcmp(ext, ".lo")) {
                FILE *f;

                f = fopen(arg, "r");
                if (f) {
                    char buf[sizeof(SANE_HEADER)];
                    fgets(buf, sizeof(SANE_HEADER), f);
                    if (!strcmp(buf, SANE_HEADER)) sane = 1;
                    fclose(f);
                }

                break;
            }
        }
    }

    return sane;
}


static void usage();
static void compile(struct Options *);

int main(int argc, char **argv)
{
    int argi, tmpi;

    /* options */
    struct Options opt;
    int insane = 0;
    char *modeS = NULL;
    enum Mode mode = MODE_UNKNOWN;
    int sane = 0;
    memset(&opt, 0, sizeof(opt));

    /* first argument must be target libtool */

    /* collect arguments up to --mode */
    for (argi = 2; argi < argc; argi++) {
        char *arg = argv[argi];

        if (!strcmp(arg, "-n") || !strcmp(arg, "--dry-run")) {
            opt.dryRun = 1;

        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage();
            exit(0);

        } else if (!strncmp(arg, "--mode=", 7) && argi < argc - 1) {
            modeS = arg + 7;
            argi++;
            break;

        } else {
            insane = 1;

        }
    }
    opt.argc = argc;
    opt.argv = argv;
    opt.cmd = argv + argi;

    if (!modeS) {
        usage();
        exit(1);
    }

    /* check the mode */
    if (!strcmp(modeS, "compile")) {
        mode = MODE_COMPILE;
    } else if (!strcmp(modeS, "link")) {
        mode = MODE_LINK;
    }

    /* next argument is the compiler, use that to check for sanity */
    if (!insane) {
        if (mode == MODE_COMPILE) {
            sane = systemIsSane(opt.cmd[0]);
        } else if (mode == MODE_LINK) {
            sane = checkLoSanity(&opt);
        }
    }

    if (!sane) {
        /* just go to libtool */
        SF(tmpi, execvp, -1, (argv[1], argv + 1));
        exit(1);

    } else if (mode == MODE_COMPILE) {
        compile(&opt);

    } else {
        exit(1);

    }

    return 0;
}

static void usage()
{
    printf("Use: mlibtool <target-libtool> [options] --mode=<mode> <command>\n"
        "Options:\n"
        "\t-n|--dry-run: display commands without modifying any files\n"
        "\t--mode=<mode>: user operation mode <mode>\n"
        "\n"
        "<mode> must be one of the following:\n"
        "\tcompile: compile a source file into a libtool object\n"
        /*"\texecute: automatically set library path, then run a program\n"*/
        /*"\tinstall: install libraries or executables\n"*/
        "\tlink: create a library or an executable\n"
        "\n");
    printf("mlibtool is a mini version of libtool for sensible systems. If you're\n"
        "compiling for Linux or BSD with supported invocation commands,\n"
        "<target-libtool> will never be called.\n"
        "\n"
        "Unrecognized invocations will be redirected to <target-libtool>.\n");
}

static void compile(struct Options *opt)
{
    char **outCmd;
    size_t oci, i;
    char *ext;
    FILE *f;

    /* options */
    char *outName = NULL;
    char *inName = NULL;
    size_t outNamePos = 0;
    int preferPic = 0, preferNonPic = 0;
    int buildPic = 0, buildNonPic = 0;

    /* option derivatives */
    char *outDirC = NULL,
         *outDir = NULL,
         *libsDir = NULL,
         *outBaseC = NULL,
         *outBase = NULL,
         *picFile = NULL,
         *nonPicFile = NULL;

    /* allocate the output command */
    /* + 5: -fPIC -DPIC -o <name> \0 */
    SF(outCmd, calloc, NULL, ((opt->argc + 5) * sizeof(char *), 1));

    /* and copy it in */
    outCmd[0] = opt->cmd[0];
    oci = 1;
    for (i = 1; opt->cmd[i]; i++) {
        char *arg = opt->cmd[i];
        char *narg = opt->cmd[i+1];

        if (!strcmp(arg, "-o") && narg) {
            /* output name */
            outCmd[oci++] = arg;
            SF(outName, strdup, NULL, (narg));
            outNamePos = oci;
            outCmd[oci++] = narg;
            i++;

        } else if (!strcmp(arg, "-prefer-pic") ||
                   !strcmp(arg, "-shared")) {
            preferPic = 1;

        } else if (!strcmp(arg, "-prefer-non-pic") ||
                   !strcmp(arg, "-static")) {
            preferNonPic = 1;

        } else if (!strncmp(arg, "-Wc,", 4)) {
            outCmd[oci++] = arg + 4;

        } else if (!strcmp(arg, "-no-suppress")) {
            /* ignored for compatibility */

        } else if (arg[0] == '-') {
            outCmd[oci++] = arg;

        } else {
            inName = arg;
            outCmd[oci++] = arg;

        }
    }

    /* if we don't have an input name, fail */
    if (!inName) {
        fprintf(stderr, "error: --mode=compile with no input file\n");
        exit(1);
    }

    /* if both preferPic and preferNonPic were specified, neither were specified */
    if (preferPic && preferNonPic)
        preferPic = preferNonPic = 0;

    if (preferPic || !preferNonPic)
        buildPic = 1;
    if (preferNonPic || !preferPic)
        buildNonPic = 1;

    /* if we don't have an output name, guess */
    if (!outName) {
        /* + 4: .lo\0 */
        SF(outName, malloc, NULL, (strlen(inName) + 4));
        strcpy(outName, inName);

        if ((ext = strrchr(outName, '.'))) {
            strcpy(ext, ".lo");
        } else {
            strcat(outName, ".lo");
        }

        /* and add it to the command */
        outCmd[oci++] = "-o";
        outNamePos = oci;
        outCmd[oci++] = outName;

    } else {
        /* make sure the output name includes .lo */
        fprintf(stderr, "%s\n", outName);
        if ((ext = strrchr(outName, '.'))) {
            if (strcmp(ext, ".lo")) {
                fprintf(stderr, "error: --mode=compile used to compile something other than a .lo file\n");
                exit(1);
            }
        } else {
            fprintf(stderr, "error: --mode=compile used to compile an executable\n");
            exit(1);
        }

    }

    /* get the directory names */
    SF(outDirC, strdup, NULL, (outName));
    outDir = dirname(outDirC);
    SF(outBaseC, strdup, NULL, (outName));
    outBase = basename(outBaseC);
    ext = strrchr(outBase, '.');
    if (ext) *ext = '\0';

    /* make the .libs dir */
    SF(libsDir, malloc, NULL, (strlen(outDir) + 7));
    sprintf(libsDir, "%s/.libs", outDir);
    if (!opt->dryRun) mkdir(libsDir, 0777); /* ignore errors */

    /* and generate the pic/non-pic names */
    SF(picFile, malloc, NULL, (strlen(libsDir) + strlen(outBase) + 7));
    SF(nonPicFile, malloc, NULL, (strlen(libsDir) + strlen(outBase) + 7));
    sprintf(picFile, "%s/%s.sh.o", libsDir, outBase);
    sprintf(nonPicFile, "%s/%s.st.o", libsDir, outBase);

    /* now do the actual building */
    if (buildNonPic) {
        outCmd[outNamePos] = nonPicFile;
        spawn(opt, outCmd);

        if (!buildPic && !opt->dryRun)
            link(nonPicFile, picFile);

    }

    if (buildPic) {
        outCmd[oci++] = "-fPIC";
        outCmd[oci++] = "-DPIC";
        outCmd[outNamePos] = picFile;
        spawn(opt, outCmd);

        if (!buildNonPic && !opt->dryRun)
            link(picFile, nonPicFile);
    }

    /* and finally, write the .lo file */
    f = fopen(outName, "w");
    if (!f) {
        perror(outName);
        exit(1);
    }
    fprintf(f, SANE_HEADER
               "pic_object='.libs/%s.sh.o'\n"
               "non_pic_object='.libs/%s.st.o'\n",
               outBase, outBase);
    fclose(f);

    free(nonPicFile);
    free(picFile);
    free(libsDir);
    free(outBaseC);
    free(outDirC);
    free(outName);
}

#endif /* _POSIX_VERSION */
