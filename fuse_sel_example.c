/*
 

  gcc -Wall `pkg-config fuse --cflags --libs` hello.c -o hello
*/

#define FUSE_USE_VERSION 29
#define __USE_XOPEN
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <libexif/exif-data.h>
#include <stdbool.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>
#include <wand/MagickWand.h>
#include <dirent.h>

static int ypfs_rename(const char *from, const char *to);
char* string_after_char(const char* path, char after);

typedef enum {NODE_DIR, NODE_FILE} NODE_TYPE; //NODE_DIR == 0 / NODE_FILE == 1

typedef struct _node {
	char* name;
	char* hash; // unique name for potential duplicate files
	NODE_TYPE type;
	struct _node ** children;
	struct _node* parent;
	int num_children;
	struct stat attr;
	int open_count; //is file currently open 1/0
} * NODE;

char configdir[256];

void getConfigDirectory()
{
	struct passwd pw;
	struct passwd *result;
	char buf[256];

	getpwuid_r(getuid(), &pw, buf, 256, &result); //should be thread-safe
	strcpy(configdir, pw.pw_dir); //base directory
	strcat(configdir, "/.ypfs");  //add the .ypfs
}


void logStuff(const char* log_str) //logging function for debugging purposes
{
	FILE *log_file;
	struct timeval time;
	gettimeofday(&time, NULL);
	
	log_file = fopen("/tmp/ypfs.log", "a"); //log file located in /tmp/ypfs.log
	fprintf(log_file, "%ld:%ld :: %s\n", time.tv_sec, time.tv_usec, log_str); //log line format: "2190348209348:2103948209348 somestuff"
	fclose(log_file);
}

/*
 * Root of file system
 */
static NODE root;

NODE init_node(const char* name, NODE_TYPE type, char* hash)
{
	NODE temp = malloc(sizeof(struct _node)); //allocate space for a temp node
	
	temp->name = malloc(sizeof(char) * (strlen(name) + 1)); //malloc space for the name
	strcpy(temp->name, name); //userspace strcpy, I'm so happy to see you again
	
	temp->type = type;
	temp->children = NULL; //new node, no children pointers yet
	temp->num_children = 0; //^
	temp->open_count = 0; //'file' current not open
	temp->hash = NULL; //initialize temp hash pointer

	if (type == NODE_FILE && hash == NULL) { //if it's a file, and doesn't have a hash yet
		temp->hash = malloc(sizeof(char) * 100); //make space for 100 bytes
		sprintf(temp->hash, "%lld", random() % 10000000000); //hash = huge random number
	}

	if (type == NODE_FILE && hash) { //if it's a file and it DOES have a hash
		temp->hash = malloc(sizeof(char) * 100); //allocate some space for temp hash
		sprintf(temp->hash, "%s", hash); //put the hash in the temp node

	}


	return temp;
}

NODE addChild(NODE parent, NODE child)
{
	struct _node **old_children = parent->children; //preserve the old children pointer
	int old_num = parent->num_children; //preserve the old count
	
	struct _node **new_children = malloc(sizeof(NODE) * (old_num + 1)); //going to need space for 1 more child

	int i;
	for (i = 0; i < old_num; i++) {
		new_children[i] = old_children[i]; //copy the old children to the new children pointer
	}

	new_children[old_num] = child; //set the last entry to the newest child

	parent->children = new_children; //fix the children pointer
	parent->num_children = old_num + 1; //fix the parent's children count

	child->parent = parent; //set its parent

	free(old_children); //since we malloc'd new space for all the old children too, we can free this now

	return child;
}

void deleteChild(NODE parent, NODE child)
{
	struct _node **old_children = parent->children; //preserve the old children pointer
	int old_num = parent->num_children; //preserve the old count
	
	struct _node **new_children;
	int i;
	if (old_num <= 0) //if it has no children, we can't remove anything
		return;

	new_children = malloc(sizeof(NODE) * (old_num - 1)); //allocate a new smaller space for the old children

	int new_index = 0;
	for (i = 0; i < old_num; i++) { //loop over all the children, and only add them to the new pointer if they're not the one we want to remove
		if (old_children[i] != child) {
			new_children[new_index++] = old_children[i];
		}
	}

	parent->children = new_children; //fix the parent's children pointer
	parent->num_children = old_num - 1; //fix the parent's children count
	
	//free all the memory we've been using
	//if statements serve to prevent freeing null pointers
	
	if (old_children) //free old children first since the array is no longer needed
		free(old_children);
	if (child->name) //we're removing this child, so free all of its attributes that need freeing
		free(child->name);
	if (child->hash)
		free(child->hash);
	if (child)
		free(child);
}

void deleteNode(NODE temp) //makes life easier later to just call this function
{
	deleteChild(temp->parent, temp); //call the deleteChild on the child of the child's parent, which is the child. Confusing.
}

void getFullPath(const char* path, char* full)  //get the full path, again, just makes life easier
{
	sprintf(full, "%s/%s", configdir, path);
}

NODE _node_for_path(char* path, NODE curr, bool create, NODE_TYPE type, char* hash, bool ignore_ext) //function to use for 'overloading'
{

	char name[1000];
	int i = 0;
	bool last_node = false;
	char* ext;
	char compare_name[1024];
	char* curr_char;
	int n = 0;

	if (curr == NULL)
		logStuff("_node_for_path: curr == NULL");

	ext = string_after_char(path, '.'); //neat function that 

	if (*path == '/')
		path++;
	

	i = 0;
	while(*path && *path != '/' && ( ext == NULL || (path < ext-1 || !ignore_ext))) {
		name[i++] = *(path++);
	}
	name[i] = '\0';
	

	if (*path == '\0')
		last_node = true;

	if (i == 0) {
		return curr;
	}

	for (i = 0; i < curr->num_children; i++) {
		ext = string_after_char(curr->children[i]->name, '.');
		curr_char = curr->children[i]->name;
		n = 0;
		while(*curr_char != '\0' && (ext == NULL || (!ignore_ext || curr_char < ext-1))) {
			compare_name[n++] = *(curr_char++);
		}
		compare_name[n] = '\0';
		if (0 == strcmp(name, compare_name))
			return _node_for_path(path, curr->children[i], create, type, hash, ignore_ext);
		*compare_name = '\0';
	}

	
	// sorry about this weird line
	if (create) {
		return _node_for_path(path, addChild(curr, init_node(name, last_node ? type : NODE_DIR, hash)), create, type, hash, ignore_ext);
	}

	return NULL;
}

NODE node_for_path(const char* path) 
{
        return _node_for_path((char*)path, root, false, 0, NULL, false);
}

NODE create_node_for_path(const char* path, NODE_TYPE type, char* hash)
{
	return _node_for_path((char*)path, root, true, type, hash, false);
}

NODE node_ignore_extension(const char* path)
{
	return _node_for_path((char*)path, root, false, 0, NULL, true);
}

char* string_after_char(const char* path, char after)
{
	char* end = (char*)path;
	while (*end) end++;
	while(end > path && *end != after) end--;
	if (end != path || *end == after)
		return end + 1;

	return NULL;
}


// from example
static int ypfs_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
	NODE file_node;
	NODE file_node_ignore_ext;
	char full_file_name[1000];

	logStuff("getattr");

	file_node = node_for_path(path);
	file_node_ignore_ext = node_ignore_extension(path);
	if (file_node_ignore_ext == NULL)
		return -ENOENT;
	getFullPath(file_node_ignore_ext->hash, full_file_name);

	if (file_node_ignore_ext && file_node_ignore_ext->type == NODE_FILE && file_node_ignore_ext != file_node) {
		// convert here, so file 1324242 becomes 1324242.png
		logStuff("EXTENSION DOESN'T MATCH; NEED TO CONVERT");
		convert_img(file_node_ignore_ext, path);
		// for stat later in function
		strcat(full_file_name, ".");
		strcat(full_file_name, string_after_char(path, '.'));
		logStuff(full_file_name);
	}


	memset(stbuf, 0, sizeof(struct stat));
	if (file_node_ignore_ext->type == NODE_DIR) { //if (strcmp(path, "/") == 0
		stbuf->st_mode = S_IFDIR | 0444;
		stbuf->st_nlink = 2;
	} else if (file_node_ignore_ext != NULL && file_node != file_node_ignore_ext) {
		logStuff("getattr for non-original file ext");
		stat(full_file_name, stbuf);
		stbuf->st_mode = S_IFREG | 0444;
	} else if (file_node_ignore_ext != NULL) {
		logStuff(full_file_name);
		stat(full_file_name, stbuf);
	} else
		res = -ENOENT;

	return res;
}

static int ypfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	int i;
	(void) offset;
	(void) fi;
	NODE file_node;
	file_node = node_for_path(path);
	logStuff("readdir");
	if (file_node == NULL)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	
	for (i = 0; i < file_node->num_children; i++) {
		filler(buf, file_node->children[i]->name, NULL, 0);
	}

	return 0;
}

// from example
static int ypfs_open(const char *path, struct fuse_file_info *fi)
{
	NODE file_node;
	char full_file_name[1000];

	logStuff("open");


	file_node = node_ignore_extension(path);

	if (file_node == NULL)  
		        return -ENOENT;

	getFullPath(file_node->hash, full_file_name);

	// different extensions
	if (strcmp(string_after_char(path, '.'), string_after_char(file_node->name, '.'))) {
		strcat(full_file_name, ".");
		strcat(full_file_name, string_after_char(path, '.'));
	}

	fi->fh = open(full_file_name, fi->flags, 0666); //Owner read/write

	if(fi->fh == -1) {
	        logStuff("fd == -1");
	        return -errno;
	}

	file_node->open_count++;




	return 0;
}

// from example
static int ypfs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	(void) fi;
	NODE file_node;
	logStuff("read");

	file_node = node_ignore_extension(path);

	if (file_node == NULL)
		return -ENOENT;

	size = pread(fi->fh, buf, size, offset);
	
	if (size < 0)
		logStuff("read error");

	return size;
}

static int ypfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	NODE new_node;
	char* end = (char*)path;
	char* urls[128];
	int num_urls;
	int i;
	int num_slashes = 0;

	while(*end != '\0') end++;
	while(*end != '/' && end >= path) end--;
	if (*end == '/') end++;

	for(i = 0; i < strlen(path); i++) {
		if (path[i] == '/') num_slashes++;
	}
	
	// no writing to non-root folders
	if (num_slashes > 1 ) {
		return -1;
	}

	new_node = init_node(end, NODE_FILE, NULL);
	logStuff("create");
	addChild(root, new_node);
	return ypfs_open(path, fi);
}

static int ypfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

	int res;
	char full_file_name[1000];
	NODE file_node = node_ignore_extension(path);
	NODE real_node = node_for_path(path);
	logStuff("write");
	getFullPath(file_node->hash, full_file_name);
	logStuff(full_file_name);

	if (file_node != real_node)
		return -1;

	res = pwrite(fi->fh, buf, size, offset);
	if(res == -1) {
		logStuff("pwrite error");
		res = -errno;
	}


	return res;
}

static int ypfs_release(const char *path, struct fuse_file_info *fi)
{
	ExifData *ed;
	ExifEntry *entry;
	char full_file_name[1000];
	NODE file_node = node_ignore_extension(path);
	char buf[1024];
	struct tm file_time;
	char year[1024];
	char month[1024];
	char new_name[2048];

	logStuff("release");

	getFullPath(file_node->hash, full_file_name);
	close(fi->fh);
	file_node->open_count--;

	// redetermine where the file goes
	if (file_node->open_count <= 0) {
		logStuff("file completely closed; checking if renaming necessary");
		ed = exif_data_new_from_file(full_file_name);
		if (ed) {
			entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_DATE_TIME);
			exif_entry_get_value(entry, buf, sizeof(buf));
			logStuff("Tag content:");
			logStuff(buf);
			strptime(buf, "%Y:%m:%d %H:%M:%S", &file_time);
			strftime(year, 1024, "%Y", &file_time);
			strftime(month, 1024, "%B", &file_time);
			sprintf(new_name, "/Dates/%s/%s/%s", year, month, file_node->name);
			logStuff(new_name);
			ypfs_rename(path, new_name);
			exif_data_unref(ed);
		} else {
			int num_slashes = 0;
			int i;
			time_t rawtime;

			for (i = 0; i < strlen(path); i++) {
				if (path[i] == '/')
					num_slashes++;
			}
			
			// if path only has 1 slash, then we're inside of the 'root' folder
			if (num_slashes == 1) {
				struct tm * now_time;
				time(&rawtime);
				now_time = localtime(&rawtime);
				strftime(year, 1024, "%Y", now_time);
				strftime(month, 1024, "%B", now_time);
				sprintf(new_name, "/Dates/%s/%s/%s", year, month, file_node->name);
				logStuff(new_name);
				ypfs_rename(path, new_name);

			}
		}
	}

	return 0;
}

static int ypfs_truncate(const char *path, off_t offset)
{
	char full_file_name[1000];
	NODE file_node = node_ignore_extension(path);
	NODE real_node = node_for_path(path);
	logStuff("truncate");
	getFullPath(file_node->hash, full_file_name);
	if (file_node != real_node) {
		strcat(full_file_name, strchr(path, '.'));
	}
	return truncate(full_file_name, offset);
}

static int ypfs_unlink(const char *path)
{
	char full_file_name[1000];
	NODE file_node = node_for_path(path);
	logStuff("unlink");
	getFullPath(file_node->hash, full_file_name);

	deleteNode(file_node);

	return unlink(full_file_name);
}

static int ypfs_rename(const char *from, const char *to)
{
	NODE old_node;
	NODE new_node;
	char* end = (char*)to;
	logStuff("rename");

	old_node = node_for_path(from);

	while(*end != '\0') end++;
	while(*end != '/' && end >= to) end--;
	if (*end == '/') end++;
	
	
	new_node = create_node_for_path(to, old_node->type, old_node->hash);
	logStuff("test 1");
	if (new_node != old_node)
		deleteNode(old_node);

	logStuff("test 2");
	return 0;

}

static int ypfs_mkdir(const char *path, mode_t mode) //mkdir not permitted inside the FS to preserve folder integrity
{

	logStuff("mkdir called");
	return -1;
}

static int ypfs_opendir(const char *path, struct fuse_file_info *fi)
{
	NODE node = node_for_path(path);
	logStuff("opendir");

	if (node && node->type == NODE_DIR) //if the filetype is a directory, open it!
		return 0;

	return -1; //trying to open a directory that isn't a directory won't work
}

static void* ypfs_init(struct fuse_conn_info *conn)
{
	return NULL;
}

static void ypfs_destroy(void * data)
{
	logStuff("destroy");
	//destroy hash files here
}

static struct fuse_operations ypfs_oper = {
	.getattr	= ypfs_getattr,
	.readdir	= ypfs_readdir,
	.open		= ypfs_open,
	.read		= ypfs_read,
	.create		= ypfs_create,
	.write		= ypfs_write,
	.release	= ypfs_release,
	.truncate	= ypfs_truncate,
	.unlink		= ypfs_unlink,
	.rename		= ypfs_rename,
	.mkdir		= ypfs_mkdir,
	.opendir	= ypfs_opendir,
	.init		= ypfs_init,
	.destroy	= ypfs_destroy
};

int main(int argc, char *argv[])
{
	logStuff("===========start============");
	srandom(time(NULL));
	getConfigDirectory();
	printf("mkdir: %d\n", mkdir(configdir, 0777));
	root = init_node("/", NODE_DIR, NULL);
	
	return fuse_main(argc, argv, &ypfs_oper, NULL);
}