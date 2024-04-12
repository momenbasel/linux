/*
 * "Optimize" a list of dependencies as spit out by gcc -MD
 * for the kernel build
 * ===========================================================================
 *
 * Author       Kai Germaschewski
 * Copyright    2002 by Kai Germaschewski  <kai.germaschewski@gmx.de>
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 *
 * Introduction:
 *
 * gcc produces a very nice and correct list of dependencies which
 * tells make when to remake a file.
 *
 * To use this list as-is however has the drawback that virtually
 * every file in the kernel includes autoconf.h.
 *
 * If the user re-runs make *config, autoconf.h will be
 * regenerated.  make notices that and will rebuild every file which
 * includes autoconf.h, i.e. basically all files. This is extremely
 * annoying if the user just changed CONFIG_HIS_DRIVER from n to m.
 *
 * So we play the same trick that "mkdep" played before. We replace
 * the dependency on autoconf.h by a dependency on every config
 * option which is mentioned in any of the listed prerequisites.
 *
 * kconfig populates a tree in include/config/ with an empty file
 * for each config symbol and when the configuration is updated
 * the files representing changed config options are touched
 * which then let make pick up the changes and the files that use
 * the config symbols are rebuilt.
 *
 * So if the user changes his CONFIG_HIS_DRIVER option, only the objects
 * which depend on "include/config/HIS_DRIVER" will be rebuilt,
 * so most likely only his driver ;-)
 *
 * The idea above dates, by the way, back to Michael E Chastain, AFAIK.
 *
 * So to get dependencies right, there are two issues:
 * o if any of the files the compiler read changed, we need to rebuild
 * o if the command line given to the compile the file changed, we
 *   better rebuild as well.
 *
 * The former is handled by using the -MD output, the later by saving
 * the command line used to compile the old object and comparing it
 * to the one we would now use.
 *
 * Again, also this idea is pretty old and has been discussed on
 * kbuild-devel a long time ago. I don't have a sensibly working
 * internet connection right now, so I rather don't mention names
 * without double checking.
 *
 * This code here has been based partially based on mkdep.c, which
 * says the following about its history:
 *
 *   Copyright abandoned, Michael Chastain, <mailto:mec@shout.net>.
 *   This is a C version of syncdep.pl by Werner Almesberger.
 *
 *
 * It is invoked as
 *
 *   fixdep <depfile> <target> <cmdline>
 *
 * and will read the dependency file <depfile>
 *
 * The transformed dependency snipped is written to stdout.
 *
 * It first generates a line
 *
 *   savedcmd_<target> = <cmdline>
 *
 * and then basically copies the .<target>.d file to stdout, in the
 * process filtering out the dependency on autoconf.h and adding
 * dependencies on include/config/MY_OPTION for every
 * CONFIG_MY_OPTION encountered in any of the prerequisites.
 *
 * We don't even try to really parse the header files, but
 * merely grep, i.e. if CONFIG_FOO is mentioned in a comment, it will
 * be picked up as well. It's not a problem with respect to
 * correctness, since that can only give too many dependencies, thus
 * we cannot miss a rebuild. Since people tend to not mention totally
 * unrelated CONFIG_ options all over the place, it's not an
 * efficiency problem either.
 *
 * (Note: it'd be easy to port over the complete mkdep state machine,
 *  but I don't think the added complexity is worth it)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdbool.h>

#define HASHSZ 256

struct item {
    struct item *next;
    unsigned int len;
    unsigned int hash;
    char name[];
};

static struct item *config_hashtab[HASHSZ], *file_hashtab[HASHSZ];

static void usage(void) {
    fprintf(stderr, "Usage: fixdep <depfile> <target> <cmdline>\n");
    exit(1);
}

static void *safe_malloc(size_t size) {
    void *buf = malloc(size);
    if (!buf) {
        perror("fixdep: malloc failure");
        exit(EXIT_FAILURE);
    }
    return buf;
}

static unsigned int strhash(const char *str, unsigned int sz) {
    unsigned int i, hash = 2166136261U;
    for (i = 0; i < sz; i++) {
        hash = (hash ^ str[i]) * 0x01000193;
    }
    return hash;
}

static void add_to_hashtable(const char *name, int len, struct item **hashtab) {
    unsigned int hash = strhash(name, len);
    struct item *item = safe_malloc(sizeof(*item) + len + 1);
    memcpy(item->name, name, len);
    item->name[len] = '\0';
    item->len = len;
    item->hash = hash;
    item->next = hashtab[hash % HASHSZ];
    hashtab[hash % HASHSZ] = item;
}

static bool in_hashtable(const char *name, int len, struct item **hashtab) {
    unsigned int hash = strhash(name, len);
    struct item *item;
    for (item = hashtab[hash % HASHSZ]; item; item = item->next) {
        if (item->hash == hash && item->len == len && memcmp(item->name, name, len) == 0) {
            return true;
        }
    }
    add_to_hashtable(name, len, hashtab);
    return false;
}

static void use_config(const char *m, int slen) {
    if (!in_hashtable(m, slen, config_hashtab)) {
        printf("    $(wildcard include/config/%.*s) \\\n", slen, m);
    }
}

static int str_ends_with(const char *s, int slen, const char *sub) {
    int sublen = strlen(sub);
    return slen >= sublen && memcmp(s + slen - sublen, sub, sublen) == 0;
}

static char *read_file(const char *filename, size_t *size) {
    struct stat st;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("fixdep: open file error");
        exit(EXIT_FAILURE);
    }
    if (fstat(fd, &st) < 0) {
        perror("fixdep: fstat error");
        close(fd);
        exit(EXIT_FAILURE);
    }
    char *buf = safe_malloc(st.st_size + 1);
    if (read(fd, buf, st.st_size) != st.st_size) {
        perror("fixdep: read error");
        free(buf);
        close(fd);
        exit(EXIT_FAILURE);
    }
    buf[st.st_size] = '\0';
    *size = st.st_size;
    close(fd);
    return buf;
}

static bool should_ignore_file(const char *s, int len) {
    return str_ends_with(s, len, "include/generated/autoconf.h") ||
           str_ends_with(s, len, ".rlib") ||
           str_ends_with(s, len, ".rmeta") ||
           str_ends_with(s, len, ".so");
}

static void parse_dep_file(const char *buf, const char *target) {
    const char *p = buf;
    printf("savedcmd_%s := $(cmd_%s)\n\n", target, target);
    printf("deps_%s := \\\n", target);

    while (*p) {
        const char *start = p;
        while (*p && !isspace(*p) && *p != ':') {
            p++;
        }
        int len = p - start;
        if (len > 0 && !should_ignore_file(start, len)) {
            printf("  %.*s \\\n", len, start);
        }
        while (isspace(*p)) {
            p++;
        }
        if (*p == ':') {
            p++;
        }
    }

    printf("\n%s: $(deps_%s)\n\n", target, target);
    printf("$(deps_%s):\n", target);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        usage();
    }
    const char *depfile = argv[1];
    const char *target = argv[2];
    const char *cmdline = argv[3];

    size_t size;
    char *buf = read_file(depfile, &size);
    parse_dep_file(buf, target);
    free(buf);

    return 0;
}
