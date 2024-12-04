#define FUSE_USE_VERSION 30
#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdbool.h>

#define MAX_DISKS 10  // Maximum number of disks supported

// RAID modes
typedef enum {
    RAID_UNKNOWN = -1,
    RAID_0 = 0,
    RAID_1 = 1,
    RAID_1V = 2
} raid_mode_t;


// Global variables for disks
int raid_mode = -1;
int disk_count = 0;
char *disk_files[MAX_DISKS];
size_t disk_size[MAX_DISKS];
void *disk_mmap[MAX_DISKS];

// Superblock
struct wfs_sb *superblock = NULL;

///////////////////////
// HELPER FUNCTIONS //
/////////////////////
int get_inode_from_path(const char *path);
struct wfs_dentry *get_directory_entry(const int block_num);
struct wfs_inode *get_inode(const int inode_num);
bool is_bitmap_set(char *bitmap, off_t index);

/* Given the index and bitmap, check if the bitmap is non-null */
bool is_bitmap_bit_set(char *bitmap, off_t index)
{
    return ((bitmap[index / 8] & (1 << (index % 8))) != 0);
}

/* From a block number, get the directory entry */
struct wfs_dentry *get_directory_entry(const int block_num)
{
    return (struct wfs_dentry *)((char *)disk_mmap[0] + superblock->d_blocks_ptr + block_num * BLOCK_SIZE);
}

/* Given a path, get the inode number*/
int get_inode_from_path(const char *path)
{
    // Make sure path is non-zero
    if (strlen(path) == 0)
    {
        printf("Error: Path is empty\n");
        return -ENOENT;
    } 

    // Make sure path is not the root
    if(strcmp(path, "/") == 0)
    {
        printf("Error: Path is root\n");
        return -EEXIST;
    } 

    // Duplicate path to tokenize without modifying original
    char *path_copy = strdup(path);

    // Find the final slash to identify the final file/directory
    char *final_slash = strrchr(path_copy, '/');

    // Seperate parent path and targetname
    if (final_slash == path_copy)
        *(final_slash + 1) = '\0';  // Parent is the root directory
    else
        *final_slash = '\0';

    char *parent_path = path_copy;
    char *target_name = final_slash + 1;

    // Initialize variables to traverse the path
    struct wfs_inode *current_inode = (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr);
    int current_inode_num = 0;  // Start from root inode

    // Tokenize the parent path, and then traverse each component
    char *token;
    char *rest = parent_path;
    for (token = __strtok_r(rest, "/", &rest); token != NULL; token = __strtok_r(NULL, "/", &rest))
    {
        int found = 0;

        // Iterate over blocks of current directory
        for (int i = 0; i < N_BLOCKS; i++)
        {
            if (current_inode->blocks[i] == 0)
                continue;   // Skip if unused
            
            // Otherwise get directory entries in the current block
            struct wfs_dentry *entries = get_directory_entry(current_inode->blocks[i]);
            
            // Skip over emppty directory entries
            if (entries == NULL)
                continue;

            // Now iterate over each directory in the block
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (entries[j].num == 0)
                    continue;   // Skip if empty directory entry
                
                // Compares entry with token
                if (strcmp(entries[j].name, token) == 0)
                {
                    // Found the entry
                    current_inode_num = entries[j].num;
                    current_inode = get_inode(current_inode_num);

                    if (current_inode == NULL)
                    {
                        free(path_copy);
                        printf("Error: Inode number %d is invalid\n", current_inode_num);
                        return -ENOENT;
                    }

                    // Found the matching entry, exit loop
                    found = 1;  
                    break;
                }
            }

            // Exit block loop, entry found
            if (found)
                break;
        }
    }

    // After traversing the parent path, search for the target entry
    for (int i = 0; i < N_BLOCKS; i++)
    {
        if (current_inode->blocks[i] == 0)
            continue;   // Skip if unused

        struct wfs_dentry *entries = get_directory_entry(current_inode->blocks[i]);

        if (entries == NULL)
            continue;   // Skip if empty
        
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (entries[j].num == 0)
                continue;   // Skip if empty

            if (strcmp(entries[j].name, target_name) == 0)
            {
                // Found the target entry
                current_inode_num = entries[j].num;

                // Make sure inode is allocated 
                char *inode_bitmap = (char *)((char *)disk_mmap[0] + superblock->i_bitmap_ptr);

                if (!is_bitmap_bit_set(inode_bitmap, current_inode_num))
                {
                    free(path_copy);
                    printf("Error: Inode number %d is not allocated\n", current_inode_num);
                    return -ENOENT;
                }

                free(path_copy);
                return current_inode_num; // Success return inode number
            }   
        }
    }

    // If target entry is not found
    free(path_copy);
    printf("Error: Target entry %s not found\n", target_name);  
    return -ENOENT;

}
/* Get the inode from the inode_number */
struct wfs_inode * get_inode(const int inode_num)
{
    return (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + inode_num * sizeof(struct wfs_inode));
}

//////////////////////////////
// END OF HELPER FUNCTIONS //
////////////////////////////

/*
 * Return file attributes. The "stat" structure is described in detail in the stat(2) manual page. For the given pathname, this should fill in the elements of the "stat" structure. 
 * If a field is meaningless or semi-meaningless (e.g., st_ino) then it should be set to 0 or given a "reasonable" value. This call is pretty much required for a usable filesystem.
*/
static int wfs_getattr(const char *path, struct stat *stbuf) 
{
    // Translate the path to an inode number (wfs_inode->num)
    int inode_num = get_inode_from_path(path);
    if (inode_num == -ENOENT)
        return -ENOENT;

    // with the inode number, get the correct inode
    struct wfs_inode *inode = get_inode(inode_num);

    // Fil the stat structure
    stbuf->st_mode = inode->mode; 
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;

    return 0;
}

/*
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems.
 *
*/ 
static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) 
{
    // Implement file creation logic here
    return -ENOSYS;
}

/*
 * Create a directory with the given name. The directory permissions are encoded in mode. See mkdir(2) for details. This function is needed for any reasonable read/write filesystem.
*/
static int wfs_mkdir(const char *path, mode_t mode) 
{

    return 0;
}

/* 
 * Remove (delete) the given file, symbolic link, hard link, or special node. Note that if you support hard links, unlink only deletes the data when the last hard link is removed. See unlink(2) for details.
*/
static int wfs_unlink(const char *path) 
{
    // Implement file deletion logic here
    return -ENOSYS;
}

/*
 * Remove the given directory. This should succeed only if the directory is empty (except for "." and ".."). See rmdir(2) for details.
*/
static int wfs_rmdir(const char *path) 
{
    // Implement directory deletion logic here
    return -ENOSYS;
}

/* 
 * Read sizebytes from the given file into the buffer buf, beginning offset bytes into the file. See read(2) for full details. 
 * Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. Required for any sensible filesystem.
*/
static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) 
{
    // Implement file read logic here
    return -ENOSYS;
}

/*
 * As for read above, except that it can't return 0.
*/
static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) 
{
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
static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) 
{
    return -ENOSYS;
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
};


int main(int argc, char *argv[]) 
{
    // Extract disk files from arguments
    int fuse_argc = 0;
    char *fuse_argv[argc + 1]; // +1 for NULL terminator
    disk_count = 0;

    // Assume the first arguments are disk files until we encounter a FUSE option (starting with '-')
    int i = 1; // argv[0] is program name
    while (i < argc && argv[i][0] != '-') 
    {
        if (disk_count < MAX_DISKS) disk_files[disk_count++] = argv[i++];
        else 
        {
            fprintf(stderr, "Error: Too many disks.\n");
            exit(1);
        }
    }

    if (disk_count < 2) 
    {
        fprintf(stderr, "Error: Not enough disks.\n");
        exit(1);
    }

    // Prepare FUSE arguments
    fuse_argv[fuse_argc++] = argv[0]; // Program name
    while (i < argc) fuse_argv[fuse_argc++] = argv[i++];

    // Null-terminate the fuse_argv array
    fuse_argv[fuse_argc] = NULL;

    // Call fuse_main with FUSE options and mount point
    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}