/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall fusexmp.c `pkg-config fuse --cflags --libs` -o fusexmp
*/

#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <libexif/exif-loader.h>
#include <pwd.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif


const int PATH_LEN = 1024;

typedef enum {NODE_DIR, NODE_FILE} NODE_TYPE;
   

struct node{
  char* path;
  struct node* parent;
  struct node* child;
  struct node* prev;
  struct node* next;
};

// getEmptyNode inits a new new node
static struct node* getEmptyNode(char* path){
  struct node* p = (struct node*)malloc(sizeof(struct node));
  p->path = (char*)malloc(PATH_LEN*sizeof(char));
  strcpy(p->path, path);
  p->child = NULL;
  p->prev = NULL;
  return p;

}

static struct node* ptr = NULL;


char configdir[256];

// sets the configdir to "/home/user/.pict
void get_configdir() {
   struct passwd pw;
   struct passwd *result;
   char buf[256];
   
   //This function gets the current user's home directory
   getpwuid_r(getuid(), &pw, buf, 256, &result);
   
   strcpy(configdir, pw.pw_dir);
   strcat(configdir, "/.pict");
   
   printf("DEBUG: configdir ==> %s\n", configdir);
}

char* fullFileName(char* part){
  char* full = (char*)malloc(PATH_LEN);
  memset(full, 0, PATH_LEN);
  strcat(full, configdir);
  strcat(full, "/");
  strcat(full,part);
  return full;

}


void printDate(ExifData* ed, char* y, char* m){
  char buf[PATH_LEN];
  struct tm file_time;
  char new_name[2048];
  char month[1024];
  char year[1024];
  if(ed){
    ExifEntry* entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_DATE_TIME);
    exif_entry_get_value(entry, buf, sizeof(buf));
    strptime(buf, "%Y:%m:%d %H:%M:%S", &file_time);
    strftime(year, 1024, "%Y", &file_time);
    strftime(month, 1024, "%B", &file_time);
    //sprintf(new_name, "/%s/%s/%s", year, month, file_node->name);
    //printf("month of %s and year of %s\n", month, year);
    strcpy(y, year);
    strcpy(m, month);
    exif_data_unref(ed);
  }

}

static void xmp_init(struct fuse_conn_info* conn){
  //ptr will contain the root
  ptr = getEmptyNode("/");
  struct node* p1 = getEmptyNode("/dogs");
  struct node* p2 = getEmptyNode("/cats");
  ptr->child = p1;
  p1->parent = ptr;
  p1->next = p2;
  p2->prev = p1;
  p2->parent = ptr;

  struct node* q1 = getEmptyNode("/myDog.jpg");
  struct node* q2 = getEmptyNode("/superDog.jpg");
  p1->child = q1;
  q1->parent = p1;
  q2->parent = p1;
  q1->next = q2;
  q2->prev = q1;
  
  // 1. configdir is the picture vault, dir with metadata managed by Linux
  get_configdir();
  printf("DEBUG: mkdir ==> %d\n", mkdir(configdir, 0777));
  
  // 2. Get file listing from picture vault
  DIR *dirStream;
  struct dirent *dp;
  struct dirent *d_entries;
  struct dirent *more_d_entries;
  int num_entries = 0;
  int j;
   
  dirStream = opendir(configdir);
  if (dirStream == NULL) {
     printf("DEBUG: Could not open configdir: %s",configdir);
     return;
  }
  
  d_entries = calloc(1,sizeof(struct dirent));
  do {
     dp = readdir(dirStream);
     if (dp != NULL) {
        d_entries[num_entries] = *dp;
        num_entries++;
        // realloc is a little dirty. see next debug printout
        more_d_entries = realloc(d_entries, 2 + num_entries);
        if (more_d_entries != NULL) {
           d_entries = more_d_entries;
        } else {
           free(d_entries);
           puts("DEBUG: Error re-allocating memory");
        }
        
        printf("DEBUG: File in .pict directory: %s\n",dp->d_name);
	//printf("DEBUG: inode is %d\n", dp->d_ino);
	if(strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..")!=0){
	  char* fullpath = fullFileName(dp->d_name);
	  printf("the file is %s\n", fullpath);
	  ExifData *ed = exif_data_new_from_file(fullpath);
	  char month[PATH_LEN];
	  char year[PATH_LEN];
	  printDate(ed,month, year);
	  printf("year of %s and month of %s\n", year, month);
	}
     }
  } while (dp != NULL);    
   
  /*
  for (j = 0; j < num_entries; j++) {
     printf("DEBUG: d_entries[%d] ==> %s\n",j, d_entries[j].d_name);
  }
  */
}

static int xmp_getattr(const char *path, struct stat *stbuf)
{
	int res;

	res = lstat(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_access(const char *path, int mask)
{
	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}

struct node* searchChild(struct node* curr,char* path){
  if(curr == NULL)
    return NULL;
  struct node* p = curr->child; // return p
  while(p != NULL){
    if(strcmp(p->path, path) == 0)
      break;
    p = p->next;
  }
  return p;
}

void printNode(struct node* p){
  if(p == NULL)
    printf("node for DBG is NULL\n");
  else
    printf("node for DBG is %s\n", p->path);
}

struct node* gotoNode(char* path){
  struct node* p = ptr->child; // return the p
  char buf[PATH_LEN];
  strcpy(buf,path);
  
  
  char* q = strtok(buf,"/");
  if(q == NULL)
    return ptr->child;

  while(q != NULL){
    p = p->child;
    printNode(p);
    q = strtok(NULL,"/");
    
  }

  return p;
}
static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	/char name[2] = { };
	char name[PATH_LEN];
	int i;

	(void) offset;
	(void) fi;

	//if (strcmp(path, "/") != 0)
	//  return -ENOENT;

	struct node* p = gotoNode(path);
	printf("DBG: p returns %s\n\n", p->path);
	if(p == NULL)
	  return -ENOENT;
	while(p != NULL){
	  //strcpy(name,p->path);
	  if(p->parent == ptr){
	    filler(buf, (p->path+1), NULL, 0);
	    printf("filler %s\n", p->path+1);
	  }else{
	    filler(buf,p->path+1, NULL, 0);
	    printf("filler %s\n", p->path+1);
	  }
	  p = p->next;
	}

	/**
	for (i = 0; i < FSEL_FILES; i++) {
	  
	  //name[0] = fsel_hex_map[i];
	  filler(buf, name, NULL, 0);
	}
	**/
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;

	/* On Linux this could just be 'mknod(path, mode, rdev)' but this
	   is more portable */
	if (S_ISREG(mode)) {
		res = open(path, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0)
			res = close(res);
	} else if (S_ISFIFO(mode))
		res = mkfifo(path, mode);
	else
		res = mknod(path, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	int res;

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char *path)
{
	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path)
{
	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to)
{
	int res;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode)
{
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid)
{
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size)
{
	int res;

	res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2])
{
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	int res;

	res = open(path, fi->flags);
	if (res == -1)
		return -errno;

	close(res);
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;
	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	close(fd);
	return res;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;
	fd = open(path, O_WRONLY);
	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	close(fd);
	return res;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) fi;
	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) isdatasync;
	(void) fi;
	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;

	if (mode)
		return -EOPNOTSUPP;

	fd = open(path, O_WRONLY);
	if (fd == -1)
		return -errno;

	res = -posix_fallocate(fd, offset, length);

	close(fd);
	return res;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations xmp_oper = {
	.init       = xmp_init,
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir,
	.mknod		= xmp_mknod,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.open		= xmp_open,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
};

int main(int argc, char *argv[])
{
	umask(0);
	return fuse_main(argc, argv, &xmp_oper, NULL);
}
