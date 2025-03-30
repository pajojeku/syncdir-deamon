
# syncdir-deamon

`syncdir-deamon` is a simple daemon written in C that synchronizes two directories. The program copies files from a source directory to a destination directory, preserving the folder structure and updating files that have been modified. The daemon runs in the background, periodically checking for changes.

## Features

- Syncs files from a source directory to a destination directory.
- Supports recursive synchronization of subdirectories with the `-R` flag.
- Allows custom sleep time between sync cycles.
- Uses memory-mapped files for large files to improve performance.
- Logs all file operations (copy and delete) to the system log (`syslog`).

## Installation

1. Clone this repository:
   ```bash
   git clone https://github.com/pajojeku/syncdir-deamon.git
   cd syncdir-deamon
   ```

2. Compile the program:
   ```bash
   gcc -o syncdir-deamon main.c
   ```

## Usage

```bash
./syncdir-deamon <source_directory> <destination_directory> [-R] [sleep_time] [mmap_threshold]
```

- `<source_directory>`: The source directory to sync.
- `<destination_directory>`: The destination directory to sync to.
- `-R`: Optional flag to enable recursive syncing of subdirectories.
- `sleep_time`: Optional time in seconds to wait between sync cycles (default is 300 seconds).
- `mmap_threshold`: Optional threshold (in bytes) for using memory-mapped files for large files (default is 10MB).

## Example

To sync two directories every 2 minutes, with the recursive flag enabled:

```bash
./syncdaemon /path/to/source /path/to/destination -R 120
```

## License

This project is licensed under the MIT License.
