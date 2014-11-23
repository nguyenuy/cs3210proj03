/*
  FUSE fsel: FUSE select example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall fsel.c `pkg-config fuse --cflags --libs` -o fsel
*/

#define FUSE_USE_VERSION 29

#include <fuse.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>

/*
 * fsel_open_mask is used to limit the number of opens to 1 per file.
 * This is to use file index (0-F) as fh as poll support requires
 * unique fh per open file.  Lifting this would require proper open
 * file management.
 */
static unsigned fsel_open_mask;
static const char fsel_hex_map[] = "0123456789ABCDEF";
static struct fuse *fsel_fuse;	/* needed for poll notification */

#define FSEL_CNT_MAX	10	/* each file can store upto 10 chars */
#define FSEL_FILES	16

static pthread_mutex_t fsel_mutex;	/* protects notify_mask and cnt array */
static unsigned fsel_poll_notify_mask;	/* poll notification scheduled? */
static struct fuse_pollhandle *fsel_poll_handle[FSEL_FILES]; /* poll notify handles */
static unsigned fsel_cnt[FSEL_FILES];	/* nbytes stored in each file */

const int PATH_LEN = 128;

struct node{
  char* path;
  struct node* parent;
  struct node* child;
  struct node* prev;
  struct node* next;
};

static struct node* getEmptyNode(char* path){
  struct node* p = (struct node*)malloc(sizeof(struct node));
  p->path = (char*)malloc(PATH_LEN*sizeof(char));
  strcpy(p->path, path);
  return p;

}

static struct node* ptr = NULL;

static void fsel_init(struct fuse_conn_info* conn){
  printf("just should just appear once in initialization\n");
  ptr = getEmptyNode("/");
  struct node* p1 = getEmptyNode("/dogs");
  struct node* p2 = getEmptyNode("/cats");
  ptr->child = p1;
  p1->parent = ptr;
  p1->next = p2;
  p2->prev = p1;
  p2->parent = ptr;

  struct node* q1 = getEmptyNode("/dogs/myDog.jpg");
  struct node* q2 = getEmptyNode("/dogs/superDog.jpg");
  p1->child = q1;
  q1->parent = p1;
  q2->parent = p1;
  q1->next = q2;
  q2->prev = q1;
}

static int fsel_path_index(const char *path)
{
	char ch = path[1];

	if (strlen(path) != 2 || path[0] != '/' || !isxdigit(ch) || islower(ch))
		return -1;
	return ch <= '9' ? ch - '0' : ch - 'A' + 10;
}

static int fsel_getattr(const char *path, struct stat *stbuf)
{
	int idx;

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0555;
		stbuf->st_nlink = 2;
		return 0;
	}
	
	/**
	idx = fsel_path_index(path);
	if (idx < 0)
		return -ENOENT;
	**/
	stbuf->st_mode = S_IFDIR | 0444;
	stbuf->st_nlink = 1;
	//stbuf->st_size = fsel_cnt[idx];
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

  char check[PATH_LEN];
  strcpy(check,"");
  while(q != NULL){
    // go to the child of this string: "/dogs", q->dogs, so goto "/dogs/myDog.jpg"
    strcat(check,"/");
    strcat(check,q);
    printf("DBG: check is %s\n", check);
    // search the children of p for check
    //p = searchChild(p, check);
    p = p->child;
    printNode(p);
    q = strtok(NULL,"/");
    
  }

  return p;
}


static int fsel_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			off_t offset, struct fuse_file_info *fi)
{
  //char name[2] = { };
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
	  if(p->parent == ptr)
	    filler(buf, (p->path+1), NULL, 0);
	  else
	    filler(buf,p->path, NULL, 0);
	  printf("filler %s\n", p->path);
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




static int fsel_open(const char *path, struct fuse_file_info *fi)
{

  printf("fsel_open\n");
  int idx = fsel_path_index(path);
  
  if (idx < 0)
    return -ENOENT;
  if ((fi->flags & 3) != O_RDONLY)
    return -EACCES;
  if (fsel_open_mask & (1 << idx))
    return -EBUSY;
  fsel_open_mask |= (1 << idx);
  
  /*
   * fsel files are nonseekable somewhat pipe-like files which
   * gets filled up periodically by producer thread and consumed
   * on read.  Tell FUSE as such.
   */
  fi->fh = idx;
  fi->direct_io = 1;
  fi->nonseekable = 1;
  
  return 0;
}

static int fsel_release(const char *path, struct fuse_file_info *fi)
{
	int idx = fi->fh;

	(void) path;

	fsel_open_mask &= ~(1 << idx);
	return 0;
}

static int fsel_read(const char *path, char *buf, size_t size, off_t offset,
		     struct fuse_file_info *fi)
{
	int idx = fi->fh;

	(void) path;
	(void) offset;

	pthread_mutex_lock(&fsel_mutex);
	if (fsel_cnt[idx] < size)
		size = fsel_cnt[idx];
	printf("READ   %X transferred=%zu cnt=%u\n", idx, size, fsel_cnt[idx]);
	fsel_cnt[idx] -= size;
	pthread_mutex_unlock(&fsel_mutex);

	memset(buf, fsel_hex_map[idx], size);
	return size;
}

static int fsel_poll(const char *path, struct fuse_file_info *fi,
		     struct fuse_pollhandle *ph, unsigned *reventsp)
{
	static unsigned polled_zero;
	int idx = fi->fh;

	(void) path;

	/*
	 * Poll notification requires pointer to struct fuse which
	 * can't be obtained when using fuse_main().  As notification
	 * happens only after poll is called, fill it here from
	 * fuse_context.
	 */
	if (!fsel_fuse) {
		struct fuse_context *cxt = fuse_get_context();
		if (cxt)
			fsel_fuse = cxt->fuse;
	}

	pthread_mutex_lock(&fsel_mutex);

	if (ph != NULL) {
		struct fuse_pollhandle *oldph = fsel_poll_handle[idx];

		if (oldph)
			fuse_pollhandle_destroy(oldph);

		fsel_poll_notify_mask |= (1 << idx);
		fsel_poll_handle[idx] = ph;
	}

	if (fsel_cnt[idx]) {
		*reventsp |= POLLIN;
		printf("POLL   %X cnt=%u polled_zero=%u\n",
		       idx, fsel_cnt[idx], polled_zero);
		polled_zero = 0;
	} else
		polled_zero++;

	pthread_mutex_unlock(&fsel_mutex);
	return 0;
}

static struct fuse_operations fsel_oper = {
  .init = fsel_init,
  .getattr	= fsel_getattr,
  .readdir	= fsel_readdir,
  .open		= fsel_open,
  .release	= fsel_release,
  .read		= fsel_read,
  .poll		= fsel_poll,
};

static void *fsel_producer(void *data)
{
	const struct timespec interval = { 0, 250000000 };
	unsigned idx = 0, nr = 1;

	(void) data;

	while (1) {
		int i, t;

		pthread_mutex_lock(&fsel_mutex);

		/*
		 * This is the main producer loop which is executed
		 * ever 500ms.  On each iteration, it fills one byte
		 * to 1, 2 or 4 files and sends poll notification if
		 * requested.
		 */
		for (i = 0, t = idx; i < nr;
		     i++, t = (t + FSEL_FILES / nr) % FSEL_FILES) {
			if (fsel_cnt[t] == FSEL_CNT_MAX)
				continue;

			fsel_cnt[t]++;
			if (fsel_fuse && (fsel_poll_notify_mask & (1 << t))) {
				struct fuse_pollhandle *ph;

				printf("NOTIFY %X\n", t);
				ph = fsel_poll_handle[t];
				fuse_notify_poll(ph);
				fuse_pollhandle_destroy(ph);
				fsel_poll_notify_mask &= ~(1 << t);
				fsel_poll_handle[t] = NULL;
			}
		}

		idx = (idx + 1) % FSEL_FILES;
		if (idx == 0)
			nr = (nr * 2) % 7;	/* cycle through 1, 2 and 4 */

		pthread_mutex_unlock(&fsel_mutex);

		nanosleep(&interval, NULL);
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t producer;
	pthread_attr_t attr;
	int ret;

	errno = pthread_mutex_init(&fsel_mutex, NULL);
	if (errno) {
		perror("pthread_mutex_init");
		return 1;
	}

	errno = pthread_attr_init(&attr);
	if (errno) {
		perror("pthread_attr_init");
		return 1;
	}

	errno = pthread_create(&producer, &attr, fsel_producer, NULL);
	if (errno) {
		perror("pthread_create");
		return 1;
	}

	ret = fuse_main(argc, argv, &fsel_oper, NULL);

	pthread_cancel(producer);
	pthread_join(producer, NULL);

	return ret;
}
