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
#include <time.h>

#define SLEEP_TIME 2
#define DEFAULT_MODE 0755

int sleep_time = SLEEP_TIME;

void handle_signal(int sig) {
    if (sig == SIGUSR1) {
        printf("Demon obudzony po sygnale SIGUSR1.\n");
    }
}

void copy_file(const char *src, const char *dst, off_t file_size) {
    int src_fd = open(src, O_RDONLY);
    if (src_fd == -1) {
        perror("Błąd przy otwieraniu pliku źródłowego");
        return;
    }

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd == -1) {
        perror("Błąd przy otwieraniu pliku docelowego");
        close(src_fd);
        return;
    }

    if (file_size < 1024 * 1024) {
        char buffer[4096];
        ssize_t bytes_read, bytes_written;
        while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
            bytes_written = write(dst_fd, buffer, bytes_read);
            if (bytes_written == -1) {
                perror("Błąd przy zapisie pliku");
                break;
            }
        }
    } else {
        void *src_map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, src_fd, 0);
        if (src_map == MAP_FAILED) {
            perror("Błąd przy mapowaniu pliku");
        } else {
            if (write(dst_fd, src_map, file_size) == -1) {
                perror("Błąd przy zapisie mmap");
            }
            munmap(src_map, file_size);
        }
    }

    close(src_fd);
    close(dst_fd);
}

void sync_directories(const char *src, const char *dst, int recursive, off_t large_file_threshold) {
    DIR *src_dir = opendir(src);
    if (!src_dir) {
        perror("Błąd przy otwieraniu katalogu źródłowego");
        return;
    }

    struct stat dst_stat;
    if (stat(dst, &dst_stat) == -1) {
        if (mkdir(dst, DEFAULT_MODE) == -1) {
            perror("Błąd przy tworzeniu katalogu docelowego");
            closedir(src_dir);
            return;
        }
    } else if (!S_ISDIR(dst_stat.st_mode)) {
        fprintf(stderr, "Docelowa ścieżka nie jest katalogiem\n");
        closedir(src_dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(src_dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char src_path[1024], dst_path[1024];
        if (snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name) >= sizeof(src_path) ||
            snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name) >= sizeof(dst_path)) {
            fprintf(stderr, "Ścieżka jest za długa!\n");
            continue;
        }

        struct stat src_stat;
        if (lstat(src_path, &src_stat) == -1) {
            perror("Błąd przy stat pliku źródłowego");
            continue;
        }

        if (S_ISLNK(src_stat.st_mode)) {
            continue;
        }

        if (S_ISDIR(src_stat.st_mode)) {
            printf("Katalog: %s\n", src_path);
            if (recursive) {
                sync_directories(src_path, dst_path, recursive, large_file_threshold);
            }
        } else {
            struct stat dst_file_stat;
            if (lstat(dst_path, &dst_file_stat) == -1 || src_stat.st_mtime > dst_file_stat.st_mtime) {
                copy_file(src_path, dst_path, src_stat.st_size);
            }
        }
    }

    closedir(src_dir);
}

void daemonize(const char *src, const char *dst) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("Błąd przy fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) {
        perror("Błąd przy setsid");
        exit(EXIT_FAILURE);
    }

    signal(SIGUSR1, handle_signal);

    while (1) {
        sleep(sleep_time);
        sync_directories(src, dst, 1, 1024 * 1024);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Użycie: %s <źródło> <cel>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // struct stat src_stat;
    // if (stat(argv[1], &src_stat) == -1) {
    //     perror("Błąd przy sprawdzaniu źródła");
    //     return EXIT_FAILURE;
    // }
    if (!S_ISDIR(src_stat.st_mode)) {
        fprintf(stderr, "Źródło musi być katalogiem\n");
        return EXIT_FAILURE;
    }

    struct stat dst_stat;
    if (stat(argv[2], &dst_stat) == -1) {
        if (mkdir(argv[2], DEFAULT_MODE) == -1) {
            perror("Błąd przy tworzeniu katalogu docelowego");
            return EXIT_FAILURE;
        }
    } else if (!S_ISDIR(dst_stat.st_mode)) {
        fprintf(stderr, "Cel musi być katalogiem\n");
        return EXIT_FAILURE;
    }

    daemonize(argv[1], argv[2]);
    return 0;
}
