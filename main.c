#include <stdio.h>      // Biblioteka do obsługi wejścia/wyjścia (np. printf, fprintf)
#include <stdlib.h>     // Biblioteka do funkcji ogólnych (np. malloc, free, exit, atoi, atol)
#include <string.h>     // Biblioteka do operacji na łańcuchach znaków (np. strcmp, strcpy, memcpy)
#include <sys/types.h>  // Definicje typów używanych w systemie operacyjnym (np. pid_t, off_t)
#include <sys/stat.h>   // Struktury i funkcje do obsługi informacji o plikach (np. stat, lstat)
#include <unistd.h>     // Funkcje systemowe POSIX (np. fork, sleep, unlink, rmdir, close)
#include <fcntl.h>      // Definicje do obsługi plików (np. open, O_RDONLY, O_CREAT)
#include <dirent.h>     // Obsługa katalogów (np. opendir, readdir, closedir)
#include <signal.h>     // Obsługa sygnałów (np. signal, SIGUSR1)
#include <sys/mman.h>   // Obsługa mapowania plików do pamięci (np. mmap, munmap)
#include <syslog.h>     // Obsługa logowania do sysloga (systemowy dziennik zdarzeń)

#define DEFAULT_MODE 0755  // Domyślne uprawnienia dla katalogów (rwxr-xr-x), czyli właściciel ma pełne prawa, grupa i inni tylko odczyt i wykonanie

typedef struct stat Stat;  // Tworzymy alias "Stat" dla struktury "struct stat" (ułatwia pisanie kodu)

// Globalne zmienne konfiguracyjne
unsigned int sleep_time = 0;        // Czas (w sekundach) pomiędzy kolejnymi synchronizacjami katalogów (domyślnie 0, ustawiany później)
int recursive = 0;                  // Flaga (0 lub 1), czy kopiowanie ma być rekurencyjne (czyli czy kopiować podkatalogi)
int mmap_threshold = 10 * 1024 * 1024;  // Próg rozmiaru pliku (w bajtach), od którego używamy mmap zamiast zwykłego kopiowania (domyślnie 10MB)

// Funkcja obsługująca sygnał SIGUSR1
// Sygnały to specjalne powiadomienia wysyłane do procesu przez system lub inne procesy
// SIGUSR1 to jeden z sygnałów użytkownika, można go użyć np. do "obudzenia" demona
void handle_signal(int sig) {
    if (sig == SIGUSR1) // Sprawdzamy, czy otrzymany sygnał to SIGUSR1
        printf("Demon obudzony po sygnale SIGUSR1.\n"); // Wyświetlamy komunikat na ekranie
}

// Funkcja kopiująca plik z lokalizacji src do dst
// src - ścieżka do pliku źródłowego (skąd kopiujemy)
// dst - ścieżka do pliku docelowego (dokąd kopiujemy)
// size - rozmiar pliku źródłowego (w bajtach)
// Jeśli plik jest duży (>= mmap_threshold), używamy mmap (mapowanie pliku do pamięci, szybsze dla dużych plików)
// W przeciwnym razie kopiujemy tradycyjnie, buforując dane w tablicy
void copy_file(const char *src, const char *dst, off_t size) {
    if (size >= mmap_threshold) { // Jeśli plik jest duży
        // Otwieramy plik źródłowy do odczytu (O_RDONLY)
        int src_fd = open(src, O_RDONLY);
        // Otwieramy plik docelowy do odczytu i zapisu, tworzymy jeśli nie istnieje, nadpisujemy jeśli istnieje
        int dst_fd = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (src_fd == -1 || dst_fd == -1) return; // Jeśli nie udało się otworzyć plików, kończymy funkcję

        // Mapujemy plik źródłowy do pamięci (tylko do odczytu)
        void *src_map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, src_fd, 0);
        // Mapujemy plik docelowy do pamięci (do odczytu i zapisu)
        void *dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
        if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
            // Jeśli mapowanie się nie powiodło, zamykamy pliki i kończymy funkcję
            close(src_fd);
            close(dst_fd);
            return;
        }

        // Kopiujemy dane z pamięci źródłowej do docelowej (cały plik naraz)
        memcpy(dst_map, src_map, size);

        // Odmapowujemy pliki z pamięci (zwalniamy zasoby)
        munmap(src_map, size);
        munmap(dst_map, size);
        // Zamykamy deskryptory plików
        close(src_fd);
        close(dst_fd);

        // Zapisujemy informację o skopiowaniu pliku do sysloga (systemowy dziennik zdarzeń)
        syslog(LOG_INFO, "Skopiowano plik (mmap): %s -> %s", src, dst);
    } else {
        // Jeśli plik jest mały, kopiujemy tradycyjnie
        int src_fd = open(src, O_RDONLY); // Otwieramy plik źródłowy do odczytu
        int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644); // Otwieramy plik docelowy do zapisu
        if (src_fd == -1 || dst_fd == -1) return; // Jeśli nie udało się otworzyć plików, kończymy funkcję

        char buffer[4096]; // Bufor na dane (4KB)
        ssize_t r; // Liczba przeczytanych bajtów
        // Czytamy dane z pliku źródłowego i zapisujemy do docelowego, aż do końca pliku
        while ((r = read(src_fd, buffer, sizeof(buffer))) > 0) 
            write(dst_fd, buffer, r);

        // Zamykamy pliki
        close(src_fd); 
        close(dst_fd);

        // Logujemy operację kopiowania do sysloga
        syslog(LOG_INFO, "Skopiowano plik: %s -> %s", src, dst);
    }
}


// Funkcja usuwająca katalog i całą jego zawartość (rekurencyjnie)
// path - ścieżka do katalogu do usunięcia
void remove_directory(const char *path) {
    DIR *dir = opendir(path); // Otwieramy katalog do przeglądania
    if (!dir) return; // Jeśli nie udało się otworzyć katalogu, kończymy funkcję
    
    struct dirent *entry; // Struktura opisująca wpis w katalogu (plik lub podkatalog)
    char full_path[1024]; // Bufor na pełną ścieżkę do pliku/podkatalogu
    struct stat statbuf;  // Struktura na informacje o pliku

    // Przeglądamy wszystkie wpisy w katalogu
    while ((entry = readdir(dir)) != NULL) {
        // Pomijamy wpisy "." (katalog bieżący) i ".." (katalog nadrzędny)
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        // Tworzymy pełną ścieżkę do wpisu (np. /katalog/plik.txt)
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        
        // Pobieramy informacje o wpisie (czy to plik, katalog, itp.)
        if (lstat(full_path, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                // Jeśli to katalog, usuwamy go rekurencyjnie (czyli najpierw jego zawartość)
                remove_directory(full_path);
            } else {
                // Jeśli to plik, usuwamy go
                unlink(full_path);
            }
        }
    }
    
    closedir(dir); // Zamykamy katalog
    // Usuwamy pusty już katalog
    rmdir(path);
}


// Funkcja usuwająca zbędne pliki i katalogi w katalogu docelowym,
// które nie występują w katalogu źródłowym
// src - ścieżka do katalogu źródłowego
// dst - ścieżka do katalogu docelowego
void remove_extraneous_files(const char *src, const char *dst) {
    DIR *dir = opendir(dst); // Otwieramy katalog docelowy
    if (!dir) return; // Jeśli nie udało się otworzyć, kończymy funkcję

    struct dirent *entry; // Struktura opisująca wpis w katalogu
    // Przeglądamy wszystkie wpisy w katalogu docelowym
    while ((entry = readdir(dir))) {
        // Pomijamy "." i ".."
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char src_path[1024], dst_path[1024]; // Bufory na ścieżki do plików/katalogów
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name); // Ścieżka w źródle
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name); // Ścieżka w docelowym

        Stat src_stat;
        // Sprawdzamy, czy plik/katalog istnieje w źródle
        if (lstat(src_path, &src_stat) == -1) {
            struct stat dst_stat;
            // Pobieramy informacje o pliku/katalogu w docelowym
            if (lstat(dst_path, &dst_stat) != -1) {
                if (S_ISDIR(dst_stat.st_mode)) {
                    // Jeśli to katalog i kopiowanie jest rekurencyjne, usuwamy cały katalog
                    if (recursive) {
                        remove_directory(dst_path);
                        syslog(LOG_INFO, "Usunięto katalog: %s", dst_path);
                    }
                } else {
                    // Jeśli to plik, usuwamy go
                    unlink(dst_path);
                    syslog(LOG_INFO, "Usunięto plik: %s", dst_path);
                }
            }
        }
    }
    closedir(dir); // Zamykamy katalog
}

// Funkcja synchronizująca zawartość katalogu źródłowego z docelowym
// src - ścieżka do katalogu źródłowego
// dst - ścieżka do katalogu docelowego
void sync_directories(const char *src, const char *dst) {
    DIR *dir = opendir(src); // Otwieramy katalog źródłowy
    if (!dir) return; // Jeśli nie udało się otworzyć, kończymy funkcję

    struct dirent *entry; // Struktura opisująca wpis w katalogu
    // Przeglądamy wszystkie wpisy w katalogu źródłowym
    while ((entry = readdir(dir))) {
        // Pomijamy "." i ".."
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char src_path[1024], dst_path[1024]; // Bufory na ścieżki
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name); // Ścieżka w źródle
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name); // Ścieżka w docelowym

        Stat src_stat, dst_stat;
        // Pobieramy informacje o pliku/katalogu w źródle
        // Pomijamy linki symboliczne i błędy stat
        if (lstat(src_path, &src_stat) == -1 || S_ISLNK(src_stat.st_mode)) continue;

        // Jeśli wpis jest katalogiem i mamy włączoną rekurencję
        if (S_ISDIR(src_stat.st_mode)) {
            if (recursive) {
                // Tworzymy katalog docelowy, jeśli nie istnieje
                if (stat(dst_path, &dst_stat) == -1) 
                    mkdir(dst_path, DEFAULT_MODE);
                // Rekurencyjnie synchronizujemy podkatalogi
                sync_directories(src_path, dst_path);
            }
        } 
        // Jeśli wpis jest plikiem
        // Kopiujemy, jeśli plik w źródle jest nowszy (ma większy czas modyfikacji) lub nie istnieje w docelowym
        else if (lstat(dst_path, &dst_stat) == -1 || src_stat.st_mtime > dst_stat.st_mtime) {
            copy_file(src_path, dst_path, src_stat.st_size);
        }
    }
    closedir(dir); // Zamykamy katalog

    // Usuwamy zbędne pliki/katalogi z katalogu docelowego
    remove_extraneous_files(src, dst);
}

// Funkcja demonizująca, która co sleep_time sekund synchronizuje katalogi
// src - ścieżka do katalogu źródłowego
// dst - ścieżka do katalogu docelowego
void daemonize(const char *src, const char *dst) {
    if (fork() > 0) exit(0);  // Tworzymy proces potomny i kończymy proces macierzysty (dzięki temu program działa w tle jako demon)
    signal(SIGUSR1, handle_signal);  // Ustawiamy obsługę sygnału SIGUSR1 (gdy proces dostanie ten sygnał, wywoła się handle_signal)
    while (1) { 
        sleep(sleep_time);  // Czekamy określoną liczbę sekund (sleep_time)
        sync_directories(src, dst);  // Synchronizujemy katalogi (kopiujemy nowe pliki, usuwamy zbędne)
    }
}

/*
 * Funkcja główna programu (punkt wejścia do programu).
 * Argumenty wywołania:
 *   argv[1] - ścieżka do katalogu źródłowego (musi istnieć)
 *   argv[2] - ścieżka do katalogu docelowego (jeśli nie istnieje, zostanie utworzony)
 *   argv[3] - opcjonalnie: "-R" (rekurencyjne kopiowanie katalogów)
 *   argv[4] - opcjonalnie: czas (w sekundach) między synchronizacjami (domyślnie 300)
 *   argv[5] - opcjonalnie: próg rozmiaru pliku (w bajtach) dla mmap (domyślnie 10MB)
 * Przykład wywołania:
 *   ./program /ścieżka/źródło /ścieżka/cel -R 60 1048576
 */
int main(int argc, char *argv[]) {
    // Inicjalizujemy sysloga (logowanie zdarzeń systemowych)
    openlog(argv[0], LOG_PID | LOG_CONS, LOG_USER);

    // Sprawdzamy, czy liczba argumentów jest poprawna (minimum 3, maksimum 6)
    if (argc < 3 || argc > 6) {
        fprintf(stderr, "Użycie: %s <źródło> <cel> [-R] [czas] [próg mmap]\n", argv[0]);
        return EXIT_FAILURE; // Kończymy program z kodem błędu
    }
        
    Stat src_stat, dst_stat;
    // Sprawdzamy, czy katalog źródłowy istnieje i jest katalogiem
    if (stat(argv[1], &src_stat) == -1 || !S_ISDIR(src_stat.st_mode)) {
        fprintf(stderr, "Błąd: Źródło musi być katalogiem\n");
        return EXIT_FAILURE;
    }
    // Sprawdzamy, czy katalog docelowy istnieje i jest katalogiem
    if (stat(argv[2], &dst_stat) == -1 || !S_ISDIR(dst_stat.st_mode)) {
        fprintf(stderr, "Katalog docelowy nie istnieje!\n");
        // Próbujemy utworzyć katalog docelowy
        if(!mkdir(argv[2], DEFAULT_MODE)) {
            fprintf(stderr, "Utworzono katalog: %s\n", argv[2]);
            syslog(LOG_INFO, "Utworzono katalog: %s\n", argv[2]);
        } else {
            return EXIT_FAILURE;
        }
    }

    // Przetwarzamy dodatkowe argumenty: -R, czas i próg mmap
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-R")) {
            recursive = 1;  // Włączamy rekurencyjne kopiowanie katalogów
        } else if (!sleep_time) { 
            sleep_time = atoi(argv[i]);  // Ustawiamy czas oczekiwania między synchronizacjami (zamieniamy tekst na liczbę)
        } else {
            mmap_threshold = atol(argv[i]);  // Ustawiamy próg rozmiaru pliku dla mmap (zamieniamy tekst na liczbę)
        }
    }

    // Jeśli nie podano czasu, ustawiamy domyślny (300 sekund)
    if(!sleep_time) sleep_time = 300;

    // Uruchamiamy proces demonizujący (program działa w tle)
    daemonize(argv[1], argv[2]);

    closelog(); // Zamykamy sysloga (kończymy logowanie)
}
