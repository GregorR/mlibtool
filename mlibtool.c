/*
 * mlibtool: The libtool accelerator
 *
 * A mini version of libtool that does the right thing on simple, sane systems.
 * On insane systems, mlibtool simply requires that true libtool be installed,
 * and calls it transparently.
 *
 * Call as
 *  $ mlibtool libtool <libtool options>
 * Or, for the truly committed,
 *  $ mlibtool false <libtool options>
 *
 * http://bitbucket.org/GregorR/mlibtool
 * http://github.com/GregorR/mlibtool
 */
#define MLIBTOOL_VERSION "0.1"

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

/* headers used in written .l* files */
#define SANE_HEADER "# SYSTEM_IS_SANE\n"
#define PACKAGE "libtool (mlibtool) " MLIBTOOL_VERSION
#define PACKAGE_HEADER "# Generated by " PACKAGE "\n"

/* systems we define as sane, based on preprocessor macros on the /target/ (not
 * this build) */
#define SANE "__linux__ || " /* Linux */ \
             "__FreeBSD_kernel__ || __FreeBSD__ || __NetBSD__ || " \
             "__OpenBSD__ || __DragonFly__ || " /* BSD family */ \
             "__GNU__" /* GNU Hurd */

/* macro to fail with perror if a function fails */
#define ORX(into, func, bad, args) do { \
    (into) = func args; \
    if ((into) == (bad)) { \
        perror("mlibtool: " #func); \
        exit(1); \
    } \
} while (0)

/* macro to fail to libtool if a function fails (must have struct Options *opt
 * as a local variable) */
#define ORL(into, func, bad, args) do { \
    (into) = func args; \
    if ((into) == bad) { \
        perror("mlibtool: " #func); \
        execLibtool(opt); \
    } \
} while (0)

/* make sure we're POSIX */
#if defined(unix) || defined(__unix) || defined(__unix__)
#include <unistd.h>
#endif

#ifndef _POSIX_VERSION
/* Not even POSIX. This system can't possibly be sane, so there's no point in
 * trying to handle builds. */
int execv(const char *path, char *const argv[]);
int main(int argc, char **argv)
{
    execv(argv[1], argv + 1);
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

/* a simple buffer type for our persistent char ** commands */
struct Buffer {
    char **buf;
    size_t bufused, bufsz;
};

#define BUFFER_DEFAULT_SZ 8

#define INIT_BUFFER(ubuf) do { \
    struct Buffer *buf_ = &(ubuf); \
    ORX(buf_->buf, malloc, NULL, (BUFFER_DEFAULT_SZ * sizeof(char *))); \
    buf_->bufused = 0; \
    buf_->bufsz = BUFFER_DEFAULT_SZ; \
} while (0)

#define WRITE_BUFFER(ubuf, val) do { \
    struct Buffer *buf_ = &(ubuf); \
    while (buf_->bufused >= buf_->bufsz) { \
        buf_->bufsz *= 2; \
        ORX(buf_->buf, realloc, NULL, (buf_->buf, buf_->bufsz * sizeof(char *))); \
    } \
    buf_->buf[buf_->bufused++] = (val); \
} while (0)

#define FREE_BUFFER(ubuf) do { \
    free((ubuf).buf); \
} while (0)


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

    ORX(tmpi, pipe, -1, (pipei));
    ORX(tmpi, pipe, -1, (pipeo));
    ORX(pid, fork, -1, ());
    if (pid == 0) {
        /* child process, read in preproc commands */
        ORX(tmpi, dup2, -1, (pipei[0], 0));
        close(pipei[0]); close(pipei[1]);
        ORX(tmpi, dup2, -1, (pipeo[1], 1));
        close(pipeo[0]); close(pipeo[1]);

        /* and spawn the preprocessor */
        execlp(cc, cc, "-E", "-", NULL);
        perror(cc);
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


/* our modes */
enum Mode {
    MODE_UNKNOWN = 0,
    MODE_COMPILE,
    MODE_LINK,
    MODE_INSTALL
};

/* options necessary to handle modes */
struct Options {
    int dryRun, quiet, retryIfFail;
    int argc;
    char **argv, **cmd;
};

/* redirect to libtool */
static void execLibtool(struct Options *opt)
{
    if (!opt->quiet)
        fprintf(stderr, "mlibtool: unsupported configuration, trying libtool (%s)\n", opt->argv[1]);
    execvp(opt->argv[1], opt->argv + 1);
    perror(opt->argv[1]);
    exit(1);
}

/* Generic function to spawn a child and wait for it, exiting if the child
 * fails. */
static void spawn(struct Options *opt, char *const *cmd)
{
    size_t i;
    int fail = 0;

    /* output the command */
    if (!opt->quiet) {
        fprintf(stderr, "mlibtool:");
        for (i = 0; cmd[i]; i++)
            fprintf(stderr, " %s", cmd[i]);
        fprintf(stderr, "\n");
    }

    /* and run it */
    if (!opt->dryRun) {
        pid_t pid;
        int tmpi;
        ORL(pid, fork, -1, ());
        if (pid == 0) {
            execvp(cmd[0], cmd);
            perror(cmd[0]);
            exit(1);
        }
        if (waitpid(pid, &tmpi, 0) != pid) {
            perror(cmd[0]);
            fail = 1;
        } else if (tmpi != 0)
            fail = 1;
    }

    if (fail) {
        if (opt->retryIfFail) {
            execLibtool(opt);
        } else {
            exit(1);
        }
    }
}

/* Check for sanity by reading a .lo file. If cc is provided, fall back to that
 * if no .lo files are found. */
static int checkLoSanity(struct Options *opt, char *cc)
{
    int sane = 0, foundlo = 0;
    size_t i;

    /* look for a .lo file and check it for sanity */
    for (i = 1; opt->cmd[i]; i++) {
        char *arg = opt->cmd[i];
        if (arg[0] != '-') {
            char *ext = strrchr(arg, '.');
            if (ext && (!strcmp(ext, ".lo") || !strcmp(ext, ".la"))) {
                FILE *f;

                foundlo = 1;
                f = fopen(arg, "r");
                if (f) {
                    char buf[sizeof(SANE_HEADER)];
                    fgets(buf, sizeof(SANE_HEADER), f);
                    if (!strcmp(buf, SANE_HEADER)) sane = 1;
                    fclose(f);
                    break;
                }
            }
        }
    }

    if (!foundlo && cc)
        return systemIsSane(cc);

    return sane;
}


static void usage(enum Mode mode);

/* mode functions */
static void ltcompile(struct Options *);
static void ltlink(struct Options *);
static void ltinstall(struct Options *);

int main(int argc, char **argv)
{
    int argi;

    /* options */
    struct Options opt;
    int insane = 0;
    char *modeS = NULL;
    enum Mode mode = MODE_UNKNOWN;
    int sane = 0;
    memset(&opt, 0, sizeof(opt));

    /* first argument must be target libtool */
    if (argc < 2 || argv[1][0] == '-') {
        usage(MODE_UNKNOWN);
        exit(1);
    }
    for (argi = 1; argi < argc && argv[argi][0] != '-'; argi++);

    /* collect arguments up to --mode */
    for (; argi < argc; argi++) {
        char *arg = argv[argi];

        if (!strcmp(arg, "-n") || !strcmp(arg, "--dry-run")) {
            opt.dryRun = 1;

        } else if (!strcmp(arg, "--quiet") ||
                   !strcmp(arg, "--silent")) {
            opt.quiet = 1;

        } else if (!strcmp(arg, "--no-quiet") ||
                   !strcmp(arg, "--no-silent")) {
            opt.quiet = 0;

        } else if (!strcmp(arg, "--version")) {
            printf("%s\n", PACKAGE);
            exit(0);

        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(MODE_UNKNOWN);
            exit(0);

        } else if (!strncmp(arg, "--mode=", 7) && argi < argc - 1) {
            modeS = arg + 7;
            argi++;
            break;

        } else if (!strncmp(arg, "--tag=", 6) ||
                   !strcmp(arg, "-v") ||
                   !strcmp(arg, "--verbose") ||
                   !strcmp(arg, "--no-verbose")) {
            /* ignored for compatibility */

        } else {
            insane = 1;

        }
    }
    opt.argc = argc;
    opt.argv = argv;
    opt.cmd = argv + argi;

    if (!modeS) {
        usage(MODE_UNKNOWN);
        exit(1);
    }

    /* check the mode */
    if (!strcmp(modeS, "compile")) {
        mode = MODE_COMPILE;
    } else if (!strcmp(modeS, "link")) {
        mode = MODE_LINK;
    } else if (!strcmp(modeS, "install")) {
        mode = MODE_INSTALL;
    }

    /* if they're asking for mode help, give it to them */
    if (argi < argc) {
        if (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h")) {
            usage(mode);
            exit(0);
        }
    }

    /* next argument is the compiler, use that to check for sanity */
    if (!insane) {
        if (mode == MODE_COMPILE) {
            sane = systemIsSane(opt.cmd[0]);
        } else if (mode == MODE_LINK) {
            sane = checkLoSanity(&opt, opt.cmd[0]);
        } else if (mode == MODE_INSTALL) {
            /* we can always do something here */
            sane = 1;
        }
    }

    if (!sane) {
        /* just go to libtool */
        execLibtool(&opt);

    } else if (mode == MODE_COMPILE) {
        ltcompile(&opt);

    } else if (mode == MODE_LINK) {
        ltlink(&opt);

    } else if (mode == MODE_INSTALL) {
        ltinstall(&opt);

    } else {
        execLibtool(&opt);

    }

    return 0;
}

static void usage(enum Mode mode)
{
    printf("Use: mlibtool <target-libtool> [options] --mode=<mode> <command>\n"
        "Options:\n"
        "\t-n|--dry-run: display commands without modifying any files\n"
        "\t--mode=<mode>: user operation mode <mode>\n"
        "\n"
        "<mode> must be one of the following:\n"
        "\tcompile: compile a source file into a libtool object\n"
        /*"\texecute: automatically set library path, then run a program\n"*/
        "\tinstall: install libraries or executables\n"
        "\tlink: create a library or an executable\n"
        "\n");

    if (mode != MODE_UNKNOWN)
        printf("Recognized mode options:\n");

    if (mode == MODE_COMPILE) {
        printf("\t-o <name>: set the output file name to <name>\n"
               "\t-prefer-pic|-shared: build only a PIC file\n"
               "\t-prefer-non-pic|-static: build only a non-PIC file\n"
               "\t-Wc,<flag>: pass flag directly to cc\n"
               "\n");

    } else if (mode == MODE_LINK) {
        printf("\t-o <name>: set the output file name to <name>\n"
               "\t-all-static: create a static binary/library\n"
               "\t-avoid-version: avoid adding version info to library names\n"
               "\t-export-dynamic: cc -rdynamic\n"
               "\t-L<dir>: search both <dir> and <dir>/.libs\n"
               "\t-module: build a module suitable for dlopen\n"
               "\t-rpath <dir>: build a shared library to be installed to <dir>\n"
               "\t              (note: this flag is REQUIRED to build a shared\n"
               "\t               library, but does NOT set an RPATH in the\n"
               "\t               resultant library)\n");
        printf("\t-version-info <current>:<rev>:<age>: set version info\n"
               "\t-Wc,<flag>|-Xcompiler <flag>|-XCClinker <flag>: pass <flag>\n"
               "\t                                                to cc\n"
               "\n");

        printf("Mode options ignored for GNU libtool compatibility:\n"
               "\t-bindir <dir>\n"
               "\n");

        printf("Unsupported mode options:\n"
               "\t-dlopen, -dlpreopen, -export-symbols, -export-symbols-regex,\n"
               "\t-objectlist, -precious-files-regex, -release, -shared,\n"
               "\t-shrext, -static, -static-libtool-libs, -weak\n"
               "\n");

    } else if (mode == MODE_INSTALL) {
        printf("\t(none)\n\n");

    }

    printf("mlibtool is a mini version of libtool for sensible systems. If you're\n"
        "compiling for Linux or BSD with supported invocation commands,\n"
        "<target-libtool> will never be called.\n"
        "\n"
        "Unrecognized invocations will be redirected to <target-libtool>.\n");
}

static void ltcompile(struct Options *opt)
{
    struct Buffer outCmd;
    size_t i;
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
    INIT_BUFFER(outCmd);

    /* and copy it in */
    WRITE_BUFFER(outCmd, opt->cmd[0]);
    for (i = 1; opt->cmd[i]; i++) {
        char *arg = opt->cmd[i];
        char *narg = opt->cmd[i+1];

        if (arg[0] == '-') {
            if (!strcmp(arg, "-o") && narg) {
                /* output name */
                WRITE_BUFFER(outCmd, arg);
                ORL(outName, strdup, NULL, (narg));
                outNamePos = outCmd.bufused;
                WRITE_BUFFER(outCmd, narg);
                i++;

            } else if (!strcmp(arg, "-prefer-pic") ||
                    !strcmp(arg, "-shared")) {
                preferPic = 1;

            } else if (!strcmp(arg, "-prefer-non-pic") ||
                    !strcmp(arg, "-static")) {
                preferNonPic = 1;

            } else if (!strncmp(arg, "-Wc,", 4)) {
                WRITE_BUFFER(outCmd, arg + 4);

            } else if (!strcmp(arg, "-no-suppress")) {
                /* ignored for compatibility */

            } else {
                WRITE_BUFFER(outCmd, arg);

            }

        } else {
            inName = arg;
            WRITE_BUFFER(outCmd, arg);

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
        ORL(outName, malloc, NULL, (strlen(inName) + 4));
        strcpy(outName, inName);

        if ((ext = strrchr(outName, '.'))) {
            strcpy(ext, ".lo");
        } else {
            strcat(outName, ".lo");
        }

        /* and add it to the command */
        WRITE_BUFFER(outCmd, "-o");
        outNamePos = outCmd.bufused;
        WRITE_BUFFER(outCmd, outName);

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
    ORL(outDirC, strdup, NULL, (outName));
    outDir = dirname(outDirC);
    ORL(outBaseC, strdup, NULL, (outName));
    outBase = basename(outBaseC);
    ext = strrchr(outBase, '.');
    if (ext) *ext = '\0';

    /* make the .libs dir */
    ORL(libsDir, malloc, NULL, (strlen(outDir) + 7));
    sprintf(libsDir, "%s/.libs", outDir);
    if (!opt->dryRun) mkdir(libsDir, 0777); /* ignore errors */

    /* and generate the pic/non-pic names */
    ORL(picFile, malloc, NULL, (strlen(libsDir) + strlen(outBase) + 7));
    ORL(nonPicFile, malloc, NULL, (strlen(libsDir) + strlen(outBase) + 7));
    sprintf(picFile, "%s/%s.sh.o", libsDir, outBase);
    sprintf(nonPicFile, "%s/%s.st.o", libsDir, outBase);

    /* now do the actual building */
    if (buildNonPic) {
        outCmd.buf[outNamePos] = nonPicFile;
        WRITE_BUFFER(outCmd, NULL);
        spawn(opt, outCmd.buf);
        outCmd.bufused--;

        if (!buildPic && !opt->dryRun)
            link(nonPicFile, picFile);

    }

    if (buildPic) {
        WRITE_BUFFER(outCmd, "-fPIC");
        WRITE_BUFFER(outCmd, "-DPIC");
        outCmd.buf[outNamePos] = picFile;

        WRITE_BUFFER(outCmd, NULL);
        spawn(opt, outCmd.buf);
        outCmd.bufused--;

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
               PACKAGE_HEADER
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

    FREE_BUFFER(outCmd);
}

/* the most complicated part of linking is linking in .la files */
static void linkLaFile(struct Options *opt,
                       int buildLib,
                       struct Buffer *outCmd,
                       struct Buffer *dependencyLibs,
                       struct Buffer *tofree,
                       char *arg)
{
    /* link to this library */
    char *laDirC, *laDir, *laBaseC, *laBase, *aarg, *ext;
    int wholeArchive = 0;
    FILE *f;

    /* OK, it's a .la file, figure out the .libs name */
    ORL(laDirC, strdup, NULL, (arg));
    laDir = dirname(laDirC);
    ORL(laBaseC, strdup, NULL, (arg));
    laBase = basename(laBaseC);

    /* get the -l name */
    ext = strrchr(laBase, '.');
    if (ext) *ext = '\0';

    /* add -L for the .libs path */
    ORL(aarg, malloc, NULL, (strlen(laDir) + 9));
    sprintf(aarg, "-L%s/.libs", laDir);
    WRITE_BUFFER(*outCmd, aarg);
    WRITE_BUFFER(*tofree, aarg);

    /* if there's only a .a, libtool specifies we bring in the whole archive */
    if (buildLib) {
        ORL(aarg, malloc, NULL, (strlen(laDir) + strlen(laBase) + 11));
        sprintf(aarg, "%s/.libs/%s.so", laDir, laBase);
        if (access(aarg, F_OK) != 0)
            wholeArchive = 1;
        free(aarg);
    }
    if (wholeArchive) {
        /* this is GNU-ld-specific, so retry if it doesn't work */
        opt->retryIfFail = 1;
        WRITE_BUFFER(*outCmd, "-Wl,--whole-archive");

    } else {
        /* if we're not linking in the whole archive, then this becomes a
         * dependency */
        if (dependencyLibs) {
            char *realla;
            if ((realla = realpath(arg, NULL))) {
                WRITE_BUFFER(*dependencyLibs, realla);
                WRITE_BUFFER(*tofree, realla);
            } else {
                WRITE_BUFFER(*dependencyLibs, arg);
            }
        }

    }

    /* and add -l<lib name> */
    if (!strncmp(laBase, "lib", 3)) laBase += 3;
    ORL(aarg, malloc, NULL, (strlen(laBase) + 3));
    sprintf(aarg, "-l%s", laBase);
    WRITE_BUFFER(*outCmd, aarg);
    WRITE_BUFFER(*tofree, aarg);

    if (wholeArchive) {
        WRITE_BUFFER(*outCmd, "-Wl,--no-whole-archive");
    }

    free(laBaseC);
    free(laDirC);

    /* then add any dependencies */
    f = fopen(arg, "r");
    if (f) {
        char *lbuf;
        size_t lbufsz, lbufused;
        lbufsz = 32;
        ORL(lbuf, malloc, NULL, (lbufsz));

        while (fgets(lbuf, lbufsz, f)) {
            lbufused = strlen(lbuf);

            /* read in the remainder of the line */
            while (lbuf[lbufused-1] != '\n') {
                lbufsz *= 2;
                ORL(lbuf, realloc, NULL, (lbuf, lbufsz));
                if (!fgets(lbuf + lbufused, lbufsz - lbufused, f)) break;
                lbufused = strlen(lbuf);
            }

            /* is this a dependency_libs line? */
            if (!strncmp(lbuf, "dependency_libs='", 17)) {
                char *part, *saveptr;
                char *dlibs = lbuf + 17;
                char *end = strrchr(dlibs, '\'');
                if (end) *end = '\0';

                /* now go one by one through the libs */
                part = strtok_r(dlibs, " ", &saveptr);
                while (part) {
                    /* if this is a .la file, need to recurse */
                    char *ext = strrchr(part, '.');
                    if (ext && !strcmp(ext, ".la")) {
                        linkLaFile(opt, buildLib, outCmd, NULL, tofree, part);

                    } else {
                        /* otherwise, just add it */
                        char *pdup;
                        ORL(pdup, strdup, NULL, (part));
                        WRITE_BUFFER(*outCmd, pdup);
                        WRITE_BUFFER(*tofree, pdup);

                    }

                    part = strtok_r(NULL, " ", &saveptr);
                }
            }
        }

        free(lbuf);
        fclose(f);
    }

}

static void ltlink(struct Options *opt)
{
    struct Buffer outCmd, outAr, dependencyLibs, tofree;
    size_t i;
    char *ext;
    int tmpi;

    /* options */
    int major = 0,
        minor = 0,
        revision = 0,
        module = 0,
        avoidVersion = 0,
        insane = 0;
    char *outName = NULL,
         *rpath = NULL;
    size_t outNamePos = 0;

    /* option derivatives */
    int buildBinary = 0,
        buildLib = 0,
        buildSo = 0,
        buildA = 0;
    char *outDirC = NULL,
         *outDir = NULL,
         *libsDir = NULL,
         *outBaseC = NULL,
         *outBase = NULL,
         *afile = NULL,
         *soname = NULL,
         *longname = NULL,
         *linkname = NULL;


    /* before we can even start, we have to figure out what we're building to
     * know whether to build the command out of .st.o or .sh.o files */
    for (i = 1; opt->cmd[i]; i++) {
        if (!strcmp(opt->cmd[i], "-o") && opt->cmd[i+1]) {
            outName = opt->cmd[i+1];
            break;
        }
    }

    if (outName) {
        ext = strrchr(outName, '.');
        if (ext && !strcmp(ext, ".la")) {
            /* it's a libtool library */
            buildLib = buildA = 1;

        } else {
            /* it's a binary */
            buildBinary = 1;

        }
    }

    /* allocate our buffers */
    INIT_BUFFER(outCmd);
    INIT_BUFFER(outAr);
    INIT_BUFFER(dependencyLibs);
    INIT_BUFFER(tofree);

    WRITE_BUFFER(outCmd, opt->cmd[0]);
    WRITE_BUFFER(outCmd, "-L.libs");
    WRITE_BUFFER(outAr, "ar");
    WRITE_BUFFER(outAr, "rc");
    WRITE_BUFFER(outAr, "a.a"); /* to be replaced */

    /* read in the command */
    for (i = 1; opt->cmd[i]; i++) {
        char *arg = opt->cmd[i];
        char *narg = opt->cmd[i+1];

        if (arg[0] == '-') {
            if (!strcmp(arg, "-all-static")) {
                WRITE_BUFFER(outCmd, "-static");

            } else if (!strcmp(arg, "-avoid-version")) {
                avoidVersion = 1;

            } else if (!strcmp(arg, "-export-dynamic")) {
                WRITE_BUFFER(outCmd, "-rdynamic");

            } else if (!strncmp(arg, "-L", 2)) {
                char *llibs;

                /* need both the -L path specified and .../.libs */
                WRITE_BUFFER(outCmd, arg);
                WRITE_BUFFER(dependencyLibs, arg);

                ORL(llibs, malloc, NULL, (strlen(arg) + 7));
                sprintf(llibs, "%s/.libs", arg);

                WRITE_BUFFER(outCmd, llibs);
                WRITE_BUFFER(dependencyLibs, llibs);
                WRITE_BUFFER(tofree, llibs);

            } else if (!strncmp(arg, "-l", 2)) {
                WRITE_BUFFER(outCmd, arg);
                WRITE_BUFFER(dependencyLibs, arg);

            } else if (!strcmp(arg, "-module")) {
                module = 1;

            } else if (!strcmp(arg, "-o") && narg) {
                WRITE_BUFFER(outCmd, arg);
                outNamePos = outCmd.bufused;
                WRITE_BUFFER(outCmd, narg);
                i++;

            } else if (!strcmp(arg, "-rpath") && narg) {
                rpath = narg;
                i++;

            } else if (!strcmp(arg, "-version-info") && narg) {
                /* current:revision:age instead of major.minor.revision */
                int current = 0;
                revision = 0;

                /* if the format fails in any way, just use 0 */
                sscanf(narg, "%d:%d:%d", &current, &revision, &minor);
                if (minor > current) minor = current;
                major = current - minor;

                i++;

            } else if (!strncmp(arg, "-Wc,", 4)) {
                WRITE_BUFFER(outCmd, arg + 4);

            } else if (narg &&
                       (!strcmp(arg, "-Xcompiler") ||
                        !strcmp(arg, "-XCClinker"))) {
                WRITE_BUFFER(outCmd, narg);
                i++;

            } else if (!strcmp(arg, "-dlopen") ||
                       !strcmp(arg, "-dlpreopen") ||
                       !strcmp(arg, "-export-symbols") ||
                       !strcmp(arg, "-export-symbols-regex") ||
                       !strcmp(arg, "-objectlist") ||
                       !strcmp(arg, "-precious-files-regex") ||
                       !strcmp(arg, "-release") ||
                       !strcmp(arg, "-shared") ||
                       !strcmp(arg, "-shrext") ||
                       !strcmp(arg, "-static") ||
                       !strcmp(arg, "-static-libtool-libs") ||
                       !strcmp(arg, "-weak")) {
                /* unsupported */
                insane = 1;

            } else if (!strcmp(arg, "-bindir") && narg) {
                /* ignored for compatibility */
                i++;

            } else if (!strcmp(arg, "-no-fast-install") ||
                       !strcmp(arg, "-no-install") ||
                       !strcmp(arg, "-no-undefined")) {
                /* ignored for compatibility */

            } else {
                WRITE_BUFFER(outCmd, arg);

            }

        } else {
            ext = strrchr(arg, '.');
            if (ext && !strcmp(ext, ".lo")) {
                /* make separate versions for the .a and the .so */
                char *loDirC, *loDir, *loBaseC, *loBase, *loFull;

                /* OK, it's a .lo file, figure out the .libs name */
                ORL(loDirC, strdup, NULL, (arg));
                loDir = dirname(loDirC);
                ORL(loBaseC, strdup, NULL, (arg));
                loBase = basename(loBaseC);

                ext = strrchr(loBase, '.');
                if (ext) *ext = '\0';

                /* make the .libs/.o version */
                ORL(loFull, malloc, NULL, (strlen(loDir) + strlen(loBase) + 13));
                sprintf(loFull, "%s/.libs/%s.s%c.o", loDir, loBase, (buildBinary ? 't' : 'h'));
                WRITE_BUFFER(outAr, loFull);
                WRITE_BUFFER(outCmd, loFull);
                WRITE_BUFFER(tofree, loFull);

                free(loBaseC);
                free(loDirC);

            } else if (ext && !strcmp(ext, ".la")) {
                linkLaFile(opt, buildLib, &outCmd, &dependencyLibs, &tofree, arg);

            } else {
                WRITE_BUFFER(outAr, arg);
                WRITE_BUFFER(outCmd, arg);

            }

        }
    }

    if (insane) {
        /* just go to libtool */
        execLibtool(opt);
    }

    /* make sure an output name was specified */
    if (!outName) {
        outName = "a.out";
        WRITE_BUFFER(outCmd, "-o");
        outNamePos = outCmd.bufused;
        WRITE_BUFFER(outCmd, outName);
    }

    /* should we build a .so? */
    if (buildLib)
        if (rpath) buildSo = 1;

    /* get the directory names */
    ORL(outDirC, strdup, NULL, (outName));
    outDir = dirname(outDirC);
    ORL(outBaseC, strdup, NULL, (outName));
    outBase = basename(outBaseC);
    ext = strrchr(outBase, '.');
    if (ext) *ext = '\0';

    /* make the .libs dir */
    ORL(libsDir, malloc, NULL, (strlen(outDir) + 7));
    sprintf(libsDir, "%s/.libs", outDir);
    if (!opt->dryRun) mkdir(libsDir, 0777); /* ignore errors */

    /* building a binary is super-simple */
    if (buildBinary) {
        WRITE_BUFFER(outCmd, NULL);
        spawn(opt, outCmd.buf);
        outCmd.bufused--;
    }

    /* building a .a library is mostly simple */
    if (buildA) {
        char *apath;

        ORL(afile, malloc, NULL, (strlen(outBase) + 3));
        sprintf(afile, "%s.a", outBase);

        ORL(apath, malloc, NULL, (strlen(outDir) + strlen(afile) + 8));
        sprintf(apath, "%s/.libs/%s", outDir, afile);
        outAr.buf[2] = apath;

        /* run ar */
        WRITE_BUFFER(outAr, NULL);
        spawn(opt, outAr.buf);
        outAr.bufused--;

        /* and make sure to ranlib too! */
        outAr.buf[1] = "ranlib";
        outAr.buf[3] = NULL;
        spawn(opt, outAr.buf + 1);

        free(apath);
    }

    /* and building a .so file is the most complicated */
    if (buildSo) {
        char *sopath = NULL,
             *longpath = NULL,
             *linkpath = NULL;

        if (!avoidVersion) {
            /* we have three filenames:
             * (1) the soname, .so.major
             * (2) the long name, .so.major.minor.revision
             * (3) the linker name, .software
             *
             * We compile with the soname as output to avoid needing -Wl,-soname.
             */
            ORL(soname, malloc, NULL, (strlen(outBase) + 4*sizeof(int) + 5));
            sprintf(soname, "%s.so.%d", outBase, major);
            ORL(longname, malloc, NULL, (strlen(outBase) + 3*4*sizeof(int) + 7));
            sprintf(longname, "%s.so.%d.%d.%d", outBase, major, minor, revision);
            ORL(linkname, malloc, NULL, (strlen(outBase) + 4));
            sprintf(linkname, "%s.so", outBase);

        } else {
            /* just one soname: .so */
            ORL(soname, malloc, NULL, (strlen(outBase) + 4));
            sprintf(soname, "%s.so", outBase);

        }

        /* and get full paths for them all */
#define FULLPATH(x) do { \
        ORL(x ## path, malloc, NULL, (strlen(outDir) + strlen(x ## name) + 8)); \
        sprintf(x ## path, "%s/.libs/%s", outDir, x ## name); \
} while (0)
        FULLPATH(so);
        if (!avoidVersion) {
            FULLPATH(long);
            FULLPATH(link);
        }
#undef FULLPATH

        /* unlink anything that already exists */
        unlink(sopath);
        if (longpath)
            unlink(longpath);
        if (linkpath)
            unlink(linkpath);

        /* set up the link command */
        WRITE_BUFFER(outCmd, "-shared");
        outCmd.buf[outNamePos] = sopath;

        /* link */
        WRITE_BUFFER(outCmd, NULL);
        spawn(opt, outCmd.buf);
        outCmd.bufused--;

        if (!avoidVersion) {
            /* move it to the proper name */
            if ((tmpi = rename(sopath, longpath)) < 0) {
                perror(longpath);
                exit(1);
            }

            /* link in the shorter names */
            if ((tmpi = symlink(longname, sopath)) < 0) {
                perror(sopath);
                exit(1);
            }
            if ((tmpi = symlink(longname, linkpath)) < 0) {
                perror(linkpath);
                exit(1);
            }
        }

        free(sopath);
        free(longpath);
        free(linkpath);
    }

    /* finally, make the .la file */
    if (buildLib) {
        FILE *f = fopen(outName, "w");
        if (!f) {
            perror(outName);
            exit(1);
        }

        fprintf(f, SANE_HEADER
                   PACKAGE_HEADER);

        if (soname) {
            /* we have a .so */
            fprintf(f, "dlname='%s'\n", soname);

            if (longname && linkname) {
                /* and other names */
                fprintf(f, "library_names='%s %s %s'\n",
                        longname, soname, linkname);
            } else {
                fprintf(f, "library_names='%s'\n", soname);
            }
        } else {
            fprintf(f, "dlname=''\nlibrary_names=''\n");
        }

        fprintf(f, "old_library='%s'\n"
                   "inherited_linker_flags=''\n", afile);

        fprintf(f, "dependency_libs='");
        for (i = 0; i < dependencyLibs.bufused; i++)
            fprintf(f, " %s", dependencyLibs.buf[i]);
        fprintf(f, "'\n");

        /* version info */
        fprintf(f, "current=%d\n"
                   "age=%d\n"
                   "revision=%d\n",
                   (major + minor) /* current is weird */,
                   minor,
                   revision);

        fprintf(f, "installed=no\n"
                   "shouldnotlink=%s\n"
                   "dlopen=''\n"
                   "dlpreopen=''\n"
                   "libdir='%s'\n",
                   (module ? "yes" : "no"),
                   (rpath ? rpath : ""));

        fclose(f);
    }

    free(afile);
    free(soname);
    free(longname);
    free(linkname);
    free(libsDir);
    free(outBaseC);
    free(outDirC);

    for (i = 0; i < tofree.bufused; i++) free(tofree.buf[i]);

    FREE_BUFFER(tofree);
    FREE_BUFFER(dependencyLibs);
    FREE_BUFFER(outAr);
    FREE_BUFFER(outCmd);
}

static void ltinstall(struct Options *opt)
{
    char *laFile = NULL;
    size_t i, laPos = 0;
    char *dirC, *dir, *baseC, *base, *ext;

    /* skip any options */
    for (i = 1; opt->cmd[i] && opt->cmd[i][0] == '-'; i++);

    /* if the command seems invalid, just run it */
    if (!opt->cmd[i]) {
        spawn(opt, opt->cmd);
        return;
    }

    /* check if this is a .la file */
    ext = strrchr(opt->cmd[i], '.');
    if (ext && !strcmp(ext, ".la")) {
        laFile = opt->cmd[i];
        laPos = i;
    }

    /* get the directory info */
    ORL(dirC, strdup, NULL, (opt->cmd[i]));
    dir = dirname(dirC);
    ORL(baseC, strdup, NULL, (opt->cmd[i]));
    base = basename(baseC);

    if (!laFile) {
        char *libsF;

        /* check if there's a .libs version */
        ORL(libsF, malloc, NULL, (strlen(dir) + strlen(base) + 8));
        sprintf(libsF, "%s/.libs/%s", dir, base);
        if (access(libsF, F_OK) == 0) {
            /* use that one */
            opt->cmd[i] = libsF;
        }

        /* Just run the command directly */
        spawn(opt, opt->cmd);

        free(libsF);

    } else {
        /* install all the files specified in the .la */
        struct Buffer cpcmd;
        FILE *f;
        size_t cpLaPos;

        /* /bin/install doesn't support symbolic links, so use something that does */
        INIT_BUFFER(cpcmd);
        WRITE_BUFFER(cpcmd, "cp");
        WRITE_BUFFER(cpcmd, "-P");
        WRITE_BUFFER(cpcmd, "-R");
        cpLaPos = cpcmd.bufused;
        for (i = laPos; opt->cmd[i]; i++)
            WRITE_BUFFER(cpcmd, opt->cmd[i]);
        WRITE_BUFFER(cpcmd, NULL);

        /* open the file */
        f = fopen(laFile, "r");
        if (f) {
            char *lbuf;
            size_t lbufsz, lbufused;
            lbufsz = 32;
            ORL(lbuf, malloc, NULL, (lbufsz));

            while (fgets(lbuf, lbufsz, f)) {
                lbufused = strlen(lbuf);

                /* read in the remainder of the line */
                while (lbuf[lbufused-1] != '\n') {
                    lbufsz *= 2;
                    ORL(lbuf, realloc, NULL, (lbuf, lbufsz));
                    if (!fgets(lbuf + lbufused, lbufsz - lbufused, f)) break;
                    lbufused = strlen(lbuf);
                }

                /* is this a library_names or old_library line? */
                if (!strncmp(lbuf, "library_names='", 15) ||
                    !strncmp(lbuf, "old_library='", 13)) {
                    char *part, *saveptr;
                    char *dlibs = lbuf + 13 + (lbuf[0] == 'l' ? 2 : 0);
                    char *end = strrchr(dlibs, '\'');
                    if (end) *end = '\0';

                    /* now go one by one through the libs */
                    part = strtok_r(dlibs, " ", &saveptr);
                    while (part) {
                        /* and install it */
                        char *fullName;
                        ORL(fullName, malloc, NULL, (strlen(dir) + strlen(part) + 8));
                        sprintf(fullName, "%s/.libs/%s", dir, part);
                        cpcmd.buf[cpLaPos] = fullName;
                        spawn(opt, cpcmd.buf);
                        free(fullName);

                        part = strtok_r(NULL, " ", &saveptr);
                    }
                }
            }

            free(lbuf);
            fclose(f);
        }

        FREE_BUFFER(cpcmd);

    }

    free(baseC);
    free(dirC);
}

#endif /* _POSIX_VERSION */
