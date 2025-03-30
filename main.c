#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/mman.h>

#define DEFAULT_MODE 0755

typedef struct stat Stat;

unsigned int sleep_time = 0;
int recursive = 0;
int mmap_threshold = 10 * 1024 * 1024;

void handle_signal(int sig) {
    if (sig == SIGUSR1) printf("Demon obudzony po sygnale SIGUSR1.\n");
}

void copy_file(const char *src, const char *dst, off_t size) {
    if (size >= mmap_threshold) {

        int src_fd = open(src, O_RDONLY);
        int dst_fd = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (src_fd == -1 || dst_fd == -1) return;

        void *src_map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, src_fd, 0);
        void *dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
        if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
            close(src_fd);
            close(dst_fd);
            return;
        }

        memcpy(dst_map, src_map, size);

        munmap(src_map, size);
        munmap(dst_map, size);
        close(src_fd);
        close(dst_fd);
    } else {
        int src_fd = open(src, O_RDONLY), dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (src_fd == -1 || dst_fd == -1) return;

        char buffer[4096]; 
        ssize_t r;
        while ((r = read(src_fd, buffer, sizeof(buffer))) > 0) 
            write(dst_fd, buffer, r);

        close(src_fd); 
        close(dst_fd);
    }
}

void remove_extraneous_files(const char *src, const char *dst) {
    DIR *dir = opendir(dst);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char src_path[1024], dst_path[1024];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

        Stat src_stat;
        if (lstat(src_path, &src_stat) == -1) {
            struct stat dst_stat;
            if (lstat(dst_path, &dst_stat) != -1) {
                if (S_ISDIR(dst_stat.st_mode)) {
                    if (recursive) {
                        char cmd[1050];
                        snprintf(cmd, sizeof(cmd), "rm -rf %s", dst_path);
                        system(cmd);
                    }
                } else {
                    unlink(dst_path);
                }
            }
        }
    }
    closedir(dir);
}

void sync_directories(const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char src_path[1024], dst_path[1024];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

        Stat src_stat, dst_stat;
        if (lstat(src_path, &src_stat) == -1 || S_ISLNK(src_stat.st_mode)) continue;

        if (S_ISDIR(src_stat.st_mode)) {
            if (recursive) {
                if (stat(dst_path, &dst_stat) == -1) mkdir(dst_path, DEFAULT_MODE);
                sync_directories(src_path, dst_path);
            }
        } else if (lstat(dst_path, &dst_stat) == -1 || src_stat.st_mtime > dst_stat.st_mtime) {
            copy_file(src_path, dst_path, src_stat.st_size);
        }
    }
    closedir(dir);

    remove_extraneous_files(src, dst);
}

void daemonize(const char *src, const char *dst) {
    if (fork() > 0) exit(0);
    signal(SIGUSR1, handle_signal);
    while (1) { sleep(sleep_time); sync_directories(src, dst); }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Użycie: %s <źródło> <cel> [-R] [czas] [próg mmap]\n", argv[0]);
        return EXIT_FAILURE;
    }
        
    Stat src_stat, dst_stat;
    if (stat(argv[1], &src_stat) == -1 || !S_ISDIR(src_stat.st_mode)) {
        fprintf(stderr, "Błąd: Źródło musi być katalogiem\n");
        return EXIT_FAILURE;
    }
    if (stat(argv[2], &dst_stat) == -1 || !S_ISDIR(dst_stat.st_mode)) {
        fprintf(stderr, "Katalog docelowy nie istnieje!\n");
        if(!mkdir(argv[2], DEFAULT_MODE)) {
            fprintf(stderr, "Utworzono katalog: %s\n", argv[2]);
        } else {
            return EXIT_FAILURE;
        }
    }

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-R")) {
            recursive = 1;
        } else if (!sleep_time) { 
            sleep_time = atoi(argv[i]);
        } else {
            mmap_threshold = atol(argv[i]);
        }
    }

    if(!sleep_time) sleep_time = 300;


    daemonize(argv[1], argv[2]);
}