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
#include <syslog.h>

#define DEFAULT_MODE 0755  // Domyślne uprawnienia dla katalogów

typedef struct stat Stat;

// Globalne zmienne konfiguracyjne
unsigned int sleep_time = 0;        // Czas oczekiwania między synchronizacjami
int recursive = 0;                  // Flaga rekurencyjnego kopiowania
int mmap_threshold = 10 * 1024 * 1024;  // Próg rozmiaru pliku dla użycia mmap

// Funkcja obsługi sygnału SIGUSR1
void handle_signal(int sig) {
    if (sig == SIGUSR1) 
        printf("Demon obudzony po sygnale SIGUSR1.\n");
}

// Funkcja kopiująca plik z src do dst
// Wykorzystuje mmap, gdy rozmiar pliku jest równy lub większy od mmap_threshold
void copy_file(const char *src, const char *dst, off_t size) {
    if (size >= mmap_threshold) {
        // Otwarcie pliku źródłowego i docelowego
        int src_fd = open(src, O_RDONLY);
        int dst_fd = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (src_fd == -1 || dst_fd == -1) return;

        // Mapowanie plików do pamięci
        void *src_map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, src_fd, 0);
        void *dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
        if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
            close(src_fd);
            close(dst_fd);
            return;
        }

        // Kopiowanie danych z pamięci źródłowej do docelowej
        memcpy(dst_map, src_map, size);

        // Zwolnienie mapowań i zamknięcie plików
        munmap(src_map, size);
        munmap(dst_map, size);
        close(src_fd);
        close(dst_fd);

        // Zapis do sysloga
        syslog(LOG_INFO, "Skopiowano plik: %s -> %s", src, dst);
    } else {
        // Kopiowanie tradycyjne, buforowane, gdy plik jest mniejszy od progu
        int src_fd = open(src, O_RDONLY);
        int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (src_fd == -1 || dst_fd == -1) return;

        char buffer[4096]; 
        ssize_t r;
        // Czytanie i zapisywanie danych w pętli
        while ((r = read(src_fd, buffer, sizeof(buffer))) > 0) 
            write(dst_fd, buffer, r);

        // Zamknięcie plików
        close(src_fd); 
        close(dst_fd);

        // Logowanie operacji kopiowania
        syslog(LOG_INFO, "Skopiowano plik: %s -> %s", src, dst);
    }
}

// Funkcja usuwająca zbędne pliki i katalogi w katalogu docelowym,
// które nie występują w katalogu źródłowym
void remove_extraneous_files(const char *src, const char *dst) {
    DIR *dir = opendir(dst);
    if (!dir) return;

    struct dirent *entry;
    // Przeglądanie zawartości katalogu docelowego
    while ((entry = readdir(dir))) {
        // Pomijanie bieżącego i nadrzędnego katalogu
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char src_path[1024], dst_path[1024];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

        Stat src_stat;
        // Jeśli plik/katalog nie istnieje w źródle, należy go usunąć z docelowego
        if (lstat(src_path, &src_stat) == -1) {
            struct stat dst_stat;
            if (lstat(dst_path, &dst_stat) != -1) {
                if (S_ISDIR(dst_stat.st_mode)) {
                    // Usuwanie rekurencyjne katalogu, jeśli ustawiona jest opcja recursive
                    if (recursive) {
                        char cmd[1050];
                        snprintf(cmd, sizeof(cmd), "rm -rf %s", dst_path);
                        system(cmd);
                        syslog(LOG_INFO, "Usunięto katalog: %s", dst_path);
                    }
                } else {
                    // Usuwanie pliku
                    unlink(dst_path);
                    syslog(LOG_INFO, "Usunięto plik: %s", dst_path);
                }
            }
        }
    }
    closedir(dir);
}

// Funkcja synchronizująca zawartość katalogu źródłowego z docelowym
void sync_directories(const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (!dir) return;

    struct dirent *entry;
    // Przeglądanie plików i katalogów w źródle
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char src_path[1024], dst_path[1024];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

        Stat src_stat, dst_stat;
        if (lstat(src_path, &src_stat) == -1 || S_ISLNK(src_stat.st_mode)) continue;

        // Jeśli element jest katalogiem i mamy włączoną rekurencję
        if (S_ISDIR(src_stat.st_mode)) {
            if (recursive) {
                // Tworzenie katalogu docelowego, jeśli nie istnieje
                if (stat(dst_path, &dst_stat) == -1) 
                    mkdir(dst_path, DEFAULT_MODE);
                // Rekurencyjne synchronizowanie katalogów
                sync_directories(src_path, dst_path);
            }
        } 
        // Jeśli element jest plikiem, kopiowanie gdy plik w źródle jest nowszy lub nie istnieje w docelowym
        else if (lstat(dst_path, &dst_stat) == -1 || src_stat.st_mtime > dst_stat.st_mtime) {
            copy_file(src_path, dst_path, src_stat.st_size);
        }
    }
    closedir(dir);

    // Usunięcie elementów zbędnych w katalogu docelowym
    remove_extraneous_files(src, dst);
}

// Funkcja daemonizująca, która co sleep_time sekund synchronizuje katalogi
void daemonize(const char *src, const char *dst) {
    if (fork() > 0) exit(0);  // Tworzenie procesu potomnego i zakończenie procesu macierzystego
    signal(SIGUSR1, handle_signal);  // Ustawienie obsługi sygnału SIGUSR1
    while (1) { 
        sleep(sleep_time);  // Oczekiwanie przez zadany czas
        sync_directories(src, dst);  // Synchronizacja katalogów
    }
}

int main(int argc, char *argv[]) {
    // Inicjalizacja sysloga
    openlog(argv[0], LOG_PID | LOG_CONS, LOG_USER);

    // Sprawdzenie poprawności argumentów wywołania
    if (argc < 3) {
        fprintf(stderr, "Użycie: %s <źródło> <cel> [-R] [czas] [próg mmap]\n", argv[0]);
        return EXIT_FAILURE;
    }
        
    Stat src_stat, dst_stat;
    // Weryfikacja, czy źródło jest katalogiem
    if (stat(argv[1], &src_stat) == -1 || !S_ISDIR(src_stat.st_mode)) {
        fprintf(stderr, "Błąd: Źródło musi być katalogiem\n");
        return EXIT_FAILURE;
    }
    // Weryfikacja istnienia katalogu docelowego
    if (stat(argv[2], &dst_stat) == -1 || !S_ISDIR(dst_stat.st_mode)) {
        fprintf(stderr, "Katalog docelowy nie istnieje!\n");
        // Próba utworzenia katalogu docelowego
        if(!mkdir(argv[2], DEFAULT_MODE)) {
            fprintf(stderr, "Utworzono katalog: %s\n", argv[2]);
            syslog(LOG_INFO, "Utworzono katalog: %s\n", argv[2]);
        } else {
            return EXIT_FAILURE;
        }
    }

    // Przetwarzanie dodatkowych argumentów: -R, czas i próg mmap
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-R")) {
            recursive = 1;  // Włączenie rekurencyjnego kopiowania
        } else if (!sleep_time) { 
            sleep_time = atoi(argv[i]);  // Ustawienie czasu oczekiwania
        } else {
            mmap_threshold = atol(argv[i]);  // Ustawienie progu dla mmap
        }
    }

    // Ustawienie domyślnego czasu, jeśli nie podano w argumentach
    if(!sleep_time) sleep_time = 300;

    // Uruchomienie procesu demonizującego
    daemonize(argv[1], argv[2]);

    closelog();
}
