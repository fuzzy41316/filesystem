#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "wfs.h"

#define MAX_DISKS 10  // Maximum number of disks supported

// Global variables for disks
int raid_mode = -1;
int disk_count = 0;
char *disk_files[MAX_DISKS];
size_t disk_size[MAX_DISKS];
void *disk_mmap[MAX_DISKS];

// Superblock
struct wfs_sb *superblock = NULL;

// Function to read the superblock from the disks
int read_superblock() 
{
    // Read superblock from the first disk
    superblock = (struct wfs_sb *)disk_mmap[0];

    // Validate superblock and RAID mode
    if (superblock->num_disks != disk_count) 
    {
        fprintf(stderr, "Error: Incorrect number of disks.\n");
        return -1;
    }
    raid_mode = superblock->raid_mode;
    return 0;
}

/*
 * Return file attributes. The "stat" structure is described in detail in the stat(2) manual page. For the given pathname, this should fill in the elements of the "stat" structure. 
 * If a field is meaningless or semi-meaningless (e.g., st_ino) then it should be set to 0 or given a "reasonable" value. This call is pretty much required for a usable filesystem.
*/
static int wfs_getattr(const char *path, struct stat *stbuf) 
{
    // Implement file attribute logic here
    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        // Root directory
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        // Implement lookup for other files and directories
        return -ENOENT;
    }
    return 0;
}

/*
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems.
 *
*/ 
static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    // Implement file creation logic here
    return -ENOSYS;
}

/*
 * Create a directory with the given name. The directory permissions are encoded in mode. See mkdir(2) for details. This function is needed for any reasonable read/write filesystem.
*/
static int wfs_mkdir(const char *path, mode_t mode) {
    // Implement directory creation logic here
    return -ENOSYS;
}

/* 
 * Remove (delete) the given file, symbolic link, hard link, or special node. Note that if you support hard links, unlink only deletes the data when the last hard link is removed. See unlink(2) for details.
*/
static int wfs_unlink(const char *path) {
    // Implement file deletion logic here
    return -ENOSYS;
}

/*
 * Remove the given directory. This should succeed only if the directory is empty (except for "." and ".."). See rmdir(2) for details.
*/
static int wfs_rmdir(const char *path) {
    // Implement directory deletion logic here
    return -ENOSYS;
}

/* 
 * Read sizebytes from the given file into the buffer buf, beginning offset bytes into the file. See read(2) for full details. 
 * Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. Required for any sensible filesystem.
*/
static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    // Implement file read logic here
    return -ENOSYS;
}

/*
 * As for read above, except that it can't return 0.
*/
static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Implement file write logic here
    return -ENOSYS;
}

/*
 * The readdir function is somewhat like read, in that it starts at a given offset and returns results in a caller-supplied buffer. 
 * However, the offset not a byte offset, and the results are a series of struct dirents rather than being uninterpreted bytes. To make life easier, FUSE provides a "filler" function that will help you put things into the buffer.

    1. Find the first directory entry following the given offset (see below).
    2. Optionally, create a struct stat that describes the file as for getattr (but FUSE only looks at st_ino and the file-type bits of st_mode).
    3. Call the filler function with arguments of buf, the null-terminated filename, the address of your struct stat (or NULL if you have none), and the offset of the next directory entry.
    4. If filler returns nonzero, or if there are no more files, return 0.
    5. Find the next file in the directory.
    6. Go back to step 2.

 * From FUSE's point of view, the offset is an uninterpreted off_t (i.e., an unsigned integer). You provide an offset when you call filler, and it's possible that such an offset might come back to you as an argument later. 
 * Typically, it's simply the byte offset (within your directory layout) of the directory entry, but it's really up to you.
 * It's also important to note that readdir can return errors in a number of instances; in particular it can return -EBADF if the file handle is invalid, or -ENOENT if you use the path argument and the path doesn't exist.
*/
static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    if (strcmp(path, "/") != 0) return -ENOENT;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    return 0;
}

/*
 * Initialize the filesystem. This function can often be left unimplemented, but it can be a handy way to perform one-time setup such as allocating variable-sized data structures or initializing a new filesystem. 
 * The fuse_conn_info structure gives information about what features are supported by FUSE, and can be used to request certain capabilities 
 * (see below for more information). The return value of this function is available to all file operations in the private_data field of fuse_context. 
 * It is also passed as a parameter to the destroy() method.
*/
static void *wfs_init(struct fuse_conn_info *conn) 
{
    // Map the disk images into memory
    for (int i = 0; i < disk_count; i++) 
    {
        int fd = open(disk_files[i], O_RDWR);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        struct stat st;
        if (fstat(fd, &st) < 0) 
        {
            perror("fstat");
            exit(1);
        }
        disk_size[i] = st.st_size;
        disk_mmap[i] = mmap(NULL, disk_size[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (disk_mmap[i] == MAP_FAILED) 
        {
            perror("mmap");
            exit(1);
        }
        close(fd);
    }

    if (read_superblock() < 0) exit(1);

    return NULL;
}

/*
 * Called when the filesystem exits. The private_data comes from the return value of init.
*/
static void wfs_destroy(void *private_data) 
{
    // Unmap the disk images from memory
    for (int i = 0; i < disk_count; i++) if (munmap(disk_mmap[i], disk_size[i]) < 0) perror("munmap");
}


// FUSE operations structure
static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod   = wfs_mknod,
    .mkdir   = wfs_mkdir,
    .unlink  = wfs_unlink,
    .rmdir   = wfs_rmdir,
    .read    = wfs_read,
    .write   = wfs_write,
    .readdir = wfs_readdir,
    .init    = wfs_init,
    .destroy = wfs_destroy,
};


int main(int argc, char *argv[]) {
    // Process arguments to extract disk files and FUSE options
    int fuse_argc = 0;
    char *fuse_argv[argc + 1]; // +1 for the program name
    fuse_argv[fuse_argc++] = argv[0]; // Program name

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-f") == 0) {
            fuse_argv[fuse_argc++] = argv[i];
        } else if (strncmp(argv[i], "-", 1) != 0) {
            // Assume it's a disk file or mount point
            if (disk_count < MAX_DISKS) {
                disk_files[disk_count++] = argv[i];
            } else {
                fprintf(stderr, "Error: Too many disks.\n");
                return -1;
            }
        } else {
            // Pass other FUSE options
            fuse_argv[fuse_argc++] = argv[i];
        }
    }

    if (disk_count < 2) {
        fprintf(stderr, "Error: Not enough disks.\n");
        return -1;
    }

    // Make sure to null-terminate the fuse_argv array
    fuse_argv[fuse_argc] = NULL;

    // Initialize FUSE with specified operations
    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}