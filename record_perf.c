#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--record] [--dataset NAME] [--records-dir DIR] -- <cmd ...>\n"
            "  cmd should be: <search_exe> <query_file> <index_file> [...]\n",
            prog);
}

static const char *derive_dataset(const char *query_path) {
    const char *base = strrchr(query_path, '/');
    base = base ? base + 1 : query_path;
    static char buf[256];
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, base, len);
    buf[len] = '\0';
    if (strncmp(buf, "query_", 6) == 0) {
        return buf + 6;
    }
    return buf[0] ? buf : "unknown";
}

static double timespec_diff_sec(const struct timespec *a,
                                const struct timespec *b) {
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    int record = 0;
    const char *dataset_override = NULL;
    const char *records_dir = "records";

    int idx = 1;
    while (idx < argc) {
        if (strcmp(argv[idx], "--record") == 0) {
            record = 1;
            idx++;
        } else if (strcmp(argv[idx], "--dataset") == 0) {
            if (idx + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            dataset_override = argv[idx + 1];
            idx += 2;
        } else if (strcmp(argv[idx], "--records-dir") == 0) {
            if (idx + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            records_dir = argv[idx + 1];
            idx += 2;
        } else if (strcmp(argv[idx], "--") == 0) {
            idx++;
            break;
        } else {
            break;
        }
    }

    if (idx >= argc) {
        usage(argv[0]);
        return 1;
    }

    char **cmd = &argv[idx];
    int cmd_argc = argc - idx;
    if (cmd_argc < 3) {
        fprintf(stderr, "command should include executable, query file, index file\n");
        return 1;
    }

    const char *executable = cmd[0];
    const char *query_file = cmd[1];
    const char *index_file = cmd[2];

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return 1;
    }

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        // child
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(1);
        }
        close(pipefd[1]);
        execvp(executable, cmd);
        perror("execvp");
        _exit(127);
    }

    close(pipefd[1]);
    FILE *out = fdopen(pipefd[0], "r");
    if (!out) {
        perror("fdopen");
        close(pipefd[0]);
        return 1;
    }

    char buf[4096];
    int hits = 0;
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), out)) > 0) {
        size_t nwritten = fwrite(buf, 1, nread, stdout);
        (void)nwritten; /* best effort forward; ignore short writes */
        for (size_t i = 0; i < nread; ++i) {
            if (buf[i] == '1') hits++;
        }
    }
    fclose(out);

    int status;
    waitpid(pid, &status, 0);
    clock_gettime(CLOCK_MONOTONIC, &end_ts);

    int return_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    double elapsed = timespec_diff_sec(&end_ts, &start_ts);

    if (record) {
        if (mkdir(records_dir, 0755) != 0 && errno != EEXIST) {
            perror("mkdir");
            return return_code;
        }

        char csv_path[512];
        const char *dataset = dataset_override ? dataset_override : derive_dataset(query_file);
        snprintf(csv_path, sizeof(csv_path), "%s/perf_%s.csv", records_dir, dataset);

        int new_file = access(csv_path, F_OK) != 0;
        FILE *csv = fopen(csv_path, "a");
        if (!csv) {
            perror("fopen");
            return return_code;
        }

        char ts_buf[32];
        time_t now = time(NULL);
        struct tm t;
        gmtime_r(&now, &t);
        strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &t);

        if (new_file) {
            fprintf(csv,
                    "timestamp_utc,executable,query_file,index_file,dataset,elapsed_seconds,hit_count,return_code\n");
        }
        fprintf(csv, "%s,%s,%s,%s,%s,%.6f,%d,%d\n", ts_buf, executable, query_file,
                index_file, dataset, elapsed, hits, return_code);
        fclose(csv);

        fprintf(stderr, "perf record appended to %s\n", csv_path);
    }

    return return_code;
}
