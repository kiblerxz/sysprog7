#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>

/* Параметры запуска */
typedef struct {
    const char *root_dir;     /* каталог поиска, по умолчанию ~/files */
    const char *needle;       /* слово для поиска */
    bool ignore_case;         /* -i: игнор регистра */
    bool use_mmap;            /* --mmap: чтение файлов через mmap */
} options_t;

/* Разворачивание пути вида ~/files -> /home/user/files */
static char *expand_tilde(const char *path) {
    if (!path || path[0] != '~') return strdup(path);
    const char *home = getenv("HOME");
    if (!home) home = "";
    size_t a = strlen(home), b = strlen(path);
    char *out = malloc(a + b);    /* b включает '~', заменяем её на $HOME */
    if (!out) return NULL;
    strcpy(out, home);
    strcat(out, path + 1);
    return out;
}

/* Безопасное склеивание путей: out = a + "/" + b */
static int path_join(const char *a, const char *b, char *out, size_t cap) {
    size_t la = strlen(a), lb = strlen(b);
    size_t need = la + 1 + lb + 1; /* '/' + '\0' */
    if (need > cap) return -1;
    strcpy(out, a);
    if (la && out[la-1] != '/') { out[la] = '/'; out[la+1] = '\0'; }
    strcat(out, b);
    return 0;
}

/* Побайтное сравнение двух буферов без учёта регистра */
static int memcasecmp(const char *a, const char *b, size_t n){
    for (size_t i=0;i<n;i++){
        int da=tolower((unsigned char)a[i]);
        int db=tolower((unsigned char)b[i]);
        if (da!=db) return da-db;
    }
    return 0;
}

/* Поиск подстроки с/без игнорирования регистра; возвращает указатель или NULL */
static const char* find_substr(const char *hay, const char *needle, bool icase){
    if (!icase) return strstr(hay, needle);
    size_t nlen = strlen(needle);
    if (nlen==0) return hay;
    for (const char *p = hay; *p; ++p){
        if (tolower((unsigned char)*p) == tolower((unsigned char)needle[0])) {
            if (strlen(p) < nlen) return NULL;
            if (memcasecmp(p, needle, nlen)==0) return p;
        }
    }
    return NULL;
}

/* Вывод совпадения в формате: путь:номер_строки:строка */
static void print_match(const char *abspath, long lineno, const char *line) {
    printf("%s:%ld:%s", abspath, lineno, line);
    if (line[0] && line[strlen(line)-1] != '\n') printf("\n");
}

/* Сканирование текстового файла построчно через fopen/getline */
static void scan_file_stream(const char *abspath, const options_t *opt) {
    FILE *f = fopen(abspath, "r");
    if (!f) { fprintf(stderr, "open failed: %s: %s\n", abspath, strerror(errno)); return; }

    char *line = NULL;
    size_t cap = 0;
    long lineno = 0;
    for (;;) {
        ssize_t r = getline(&line, &cap, f);
        if (r < 0) break;
        lineno++;
        if (find_substr(line, opt->needle, opt->ignore_case))
            print_match(abspath, lineno, line);
    }
    free(line);
    fclose(f);
}

/* Сканирование файла через mmap (доп. задание) */
static void scan_file_mmap(const char *abspath, const options_t *opt) {
    int fd = open(abspath, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open failed: %s: %s\n", abspath, strerror(errno)); return; }

    struct stat st;
    if (fstat(fd, &st) != 0) { fprintf(stderr, "fstat failed: %s: %s\n", abspath, strerror(errno)); close(fd); return; }
    if (!S_ISREG(st.st_mode) || st.st_size == 0) { close(fd); return; }

    size_t len = (size_t)st.st_size;
    void *mem = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mem == MAP_FAILED) { fprintf(stderr, "mmap failed: %s: %s\n", abspath, strerror(errno)); close(fd); return; }

    const char *buf = (const char*)mem;
    long line_no = 1;
    const char *line_start = buf;

    /* Разбиваем на строки по '\n' и проверяем каждую */
    for (size_t i=0; i<len; ++i) {
        if (buf[i] == '\n') {
            size_t L = (size_t)(&buf[i] - line_start);
            char *tmp = malloc(L + 1);
            if (!tmp) { munmap(mem, len); close(fd); return; }
            memcpy(tmp, line_start, L); tmp[L] = '\0';
            if (find_substr(tmp, opt->needle, opt->ignore_case))
                print_match(abspath, line_no, tmp);
            free(tmp);
            line_no++;
            line_start = &buf[i+1];
        }
    }
    if (line_start < buf + len) {          /* последняя строка без \n */
        size_t L = (size_t)((buf + len) - line_start);
        char *tmp = malloc(L + 1);
        if (tmp) {
            memcpy(tmp, line_start, L); tmp[L] = '\0';
            if (find_substr(tmp, opt->needle, opt->ignore_case))
                print_match(abspath, line_no, tmp);
            free(tmp);
        }
    }

    munmap(mem, len);
    close(fd);
}

/* Пропускаем все, что не обычный файл и не каталог (ссылки, устройства и пр.) */
static bool is_skippable(const struct stat *st) {
    if (S_ISREG(st->st_mode) || S_ISDIR(st->st_mode)) return false;
    return true;
}

/* Прототип рекурсивного обхода */
static void walk_dir(const char *dirpath, const options_t *opt);

/* Обработка одного элемента директории */
static void handle_entry(const char *parent, const char *name, const options_t *opt) {
    char full[PATH_MAX];
    if (path_join(parent, name, full, sizeof(full)) != 0) {
        fprintf(stderr, "path too long: %s/%s\n", parent, name);
        return;
    }

    struct stat st;
    if (lstat(full, &st) != 0) {
        fprintf(stderr, "lstat failed: %s: %s\n", full, strerror(errno));
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return;
        walk_dir(full, opt);      /* рекурсия: заходим во ВСЕ каталоги, в т.ч. скрытые */
        return;
    }

    if (is_skippable(&st)) return;

    char abspath[PATH_MAX];
    if (!realpath(full, abspath)) {         /* если не удалось — печатаем как есть */
        strncpy(abspath, full, sizeof(abspath)-1);
        abspath[sizeof(abspath)-1] = '\0';
    }

    if (opt->use_mmap) scan_file_mmap(abspath, opt);
    else               scan_file_stream(abspath, opt);
}

/* Рекурсивный обход директории */
static void walk_dir(const char *dirpath, const options_t *opt) {
    DIR *d = opendir(dirpath);
    if (!d) { fprintf(stderr, "opendir failed: %s: %s\n", dirpath, strerror(errno)); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        handle_entry(dirpath, de->d_name, opt);
    }
    closedir(d);
}

/* Подсказка по использованию */
static void usage(const char *prog){
    fprintf(stderr,
        "Usage: %s [-i] [--mmap] [DIR] WORD\n"
        "  DIR   : каталог поиска (по умолчанию ~/files)\n"
        "  WORD  : слово для поиска\n"
        "  -i    : игнор регистра\n"
        "  --mmap: чтение файлов через mmap\n", prog);
}

/* Точка входа */
int main(int argc, char **argv) {
    options_t opt = {0};
    opt.ignore_case = false;
    opt.use_mmap = false;

    /* Разбор флагов (-i, --mmap) */
    int i = 1;
    for (; i < argc; ++i) {
        if (strcmp(argv[i], "-i") == 0) opt.ignore_case = true;
        else if (strcmp(argv[i], "--mmap") == 0) opt.use_mmap = true;
        else break;
    }

    const char *dirarg = NULL;
    const char *word   = NULL;

    if (i >= argc) { usage(argv[0]); return 2; }

    if (i == argc - 1) {            /* задано только слово -> каталог по умолчанию */
        dirarg = "~/files";
        word   = argv[i];
    } else if (i == argc - 2) {     /* заданы каталог и слово */
        dirarg = argv[i];
        word   = argv[i+1];
    } else {
        usage(argv[0]); return 2;
    }

    char *dir = expand_tilde(dirarg);
    if (!dir) { fprintf(stderr, "OOM\n"); return 1; }

    opt.root_dir = dir;
    opt.needle   = word;

    struct stat st;
    if (stat(opt.root_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "not a directory: %s\n", opt.root_dir);
        free(dir);
        return 1;
    }

    walk_dir(opt.root_dir, &opt);
    free(dir);
    return 0;
}
