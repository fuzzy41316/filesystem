# CS537 Fall 2024 - Project 6

## 🔍 Problem Statement  
In this project, I developed a **FUSE-based filesystem** implementing **RAID 0**, **RAID 1**, and **RAID 1v** functionalities. The goal was to create a filesystem that supports operations such as file creation, reading/writing, and directory management while ensuring the correct mirroring of data across different RAID configurations. Key tasks included:

- **RAID 0 Implementation**: Implementing a striped disk configuration that splits data across multiple disks for performance.
- **RAID 1 Implementation**: Creating a mirrored disk configuration that replicates data across disks for redundancy.
- **RAID 1v Implementation**: Implementing a variation of RAID 1 where data is mirrored, but with an additional level of fault tolerance.
- **Filesystem Operations**: Implementing essential FUSE filesystem operations, including `getattr`, `mknod`, `mkdir`, `unlink`, `rmdir`, `read`, `write`, and `readdir`, while ensuring that these operations are handled efficiently and correctly across the RAID disks.
- **Superblock Management**: Modifying the superblock to store metadata and information about disk usage, and ensuring that this information is synchronized across all RAID configurations.
- **Disk Initialization**: Using the provided `create_disk.sh` script to initialize the disk images required for the RAID configurations.

The project required ensuring that operations on the filesystem were **RAID-aware**, meaning that any changes made to files should properly reflect across the underlying RAID disks, ensuring data integrity and fault tolerance.

---

## 🎯 What I Learned  
This project provided extensive experience in **filesystem design** and **RAID implementation**. Key takeaways include:

✅ **RAID Configurations** – Implementing RAID 0, RAID 1, and RAID 1v to understand how data is stored across multiple disks, optimizing for both performance and redundancy.  
✅ **FUSE Filesystem Operations** – Gaining hands-on experience with FUSE, implementing filesystem operations such as file creation, reading, writing, and directory management.  
✅ **Superblock and Metadata Management** – Understanding how superblocks work and modifying them to manage disk metadata effectively.  
✅ **Fault Tolerance and Data Mirroring** – Ensuring that RAID 1 and RAID 1v correctly mirror data across disks to prevent data loss in case of disk failure.  
✅ **Disk Initialization** – Using shell scripts to initialize disk images and prepare them for RAID operations.  
✅ **Filesystem Efficiency** – Balancing performance with data integrity, ensuring that operations on the filesystem are both fast and reliable.

---

## 🏆 Results  
I successfully implemented a **RAID 0**, **RAID 1**, and **RAID 1v** filesystem, passing **all test cases**. The filesystem operations were fully functional, with file creation, reading/writing, and directory management working seamlessly across different RAID configurations. Additionally, the superblock management was integrated, ensuring that metadata was correctly mirrored across disks and maintained in sync.

This project deepened my understanding of **filesystem internals**, **RAID technology**, and **disk management**, and it significantly enhanced my skills in **working with FUSE**, **data redundancy**, and **fault-tolerant systems**. 
