#ifndef VSD_IMG_H
#define VSD_IMG_H
#include <stdio.h>
/*
 * vsd_img -- the virtual-SD image factory (tools/vsd.py). Builds,
 * inspects and patches a FAT16 volume file through the project's OWN lib/fatfs (the
 * exact ff.c the GBA links, over tests/hostfat's ramdisk.c), never a second FAT
 * implementation. See tools/vsd_img.c's header comment for the build recipe.
 *
 * Every function returns 0 on success, nonzero on failure (a message already went to
 * stderr). Declared here (rather than static in vsd_img.c) so a host test can link
 * this file with -DVSD_IMG_NO_MAIN and call them directly, without picking up main().
 */

/* f_mkfs a fresh FAT16 volume (two FATs, matching the ubiquitous real-card default)
 * of `size_mb` MiB; if `app_dir` is non-NULL/non-empty, f_mkdir "/<app_dir>" (the
 * tool's single SD folder); if `template_dir` is non-NULL, recursively copy its tree in
 * (host path -> image path, 1:1, '/' separators, rooted at the image's "/"). Dumps the
 * resulting volume to `out_path` as a flat 512-byte-sector raw image (no partition
 * table -- disk_ioctl's GET_SECTOR_SIZE is 512, matching FF_MAX_SS). A small `size_mb`
 * (roughly < 16) makes f_mkfs build FAT12 instead -- no real SD card is ever formatted
 * that way, so this REFUSES (returns nonzero, a message on stderr) unless the mounted
 * result is FS_FAT16; use >= 16 MiB. */
int vsdimg_mkimg(const char* out_path, unsigned size_mb, const char* template_dir,
                 const char* app_dir);

/* Mount `img_path`, walk every regular file under "/", print one "path size crc32"
 * line per file to `out`, sorted by path so two lists of the same tree always compare
 * byte-identical regardless of on-disk directory order. CRC32 matches zlib.crc32
 * (tools/crc32.c). Directories are not listed. */
int vsdimg_list(const char* img_path, FILE* out);

/* Mount `img_path`, f_open(target_path, FA_WRITE|FA_CREATE_ALWAYS) (creates the file
 * if absent, truncates+rewrites if present -- the "put a real fixture on the card"
 * path), write `src_file`'s host bytes in full, f_close, dump the volume back over
 * `img_path`. */
int vsdimg_patch(const char* img_path, const char* target_path, const char* src_file);

/* Mount `img_path` and write the exact bytes of the file at `target_path` to `out`
 * (what the chains compare against). Nonzero if the file does not exist. */
int vsdimg_cat(const char* img_path, const char* target_path, FILE* out);

#endif /* VSD_IMG_H */
