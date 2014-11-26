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
#include <dirent.h>

typedef enum {NODE_DIR, NODE_FILE} NODE_TYPE; //NODE_DIR == 0 / NODE_FILE == 1

typedef struct _node {
	char* name;
	char* unique_id; // unique name for potential duplicate files
	NODE_TYPE type;
	struct _node ** children;
	struct _node* parent;
	int num_children;
	struct stat attr;
	int open_count;
} * NODE;

char filevaultdir[256];

void getFileVaultDirectory()
{
	struct passwd pw;
	struct passwd *result;
	char buf[256];

	getpwuid_r(getuid(), &pw, buf, 256, &result); //should be thread-safe
	strcpy(filevaultdir, pw.pw_dir); //base directory
	strcat(filevaultdir, "/.mypics");  //hidden representation on user home directory
}

/*
 * Root of file system
 */
static NODE root;

NODE init_node(const char* name, NODE_TYPE type, char* unique_id)
{
	NODE temp = malloc(sizeof(struct _node)); //allocate space for a temp node
	
	temp->name = malloc(sizeof(char) * (strlen(name) + 1)); //malloc space for the name + '\0'
	strcpy(temp->name, name); //userspace strcpy, I'm so happy to see you again
	
	temp->type = type;
	temp->children = NULL; //new node, no children pointers yet
	temp->num_children = 0; //^
	temp->open_count = 0;
	temp->unique_id = NULL;

	if (type == NODE_FILE && unique_id == NULL) { //if it's a file, and doesn't have a unique_id yet
		temp->unique_id = malloc(sizeof(char) * 100); //make space for 100 bytes
		sprintf(temp->unique_id, "%lld", random() % 100000000000000); //unique_id = huge random number
	}

	if (type == NODE_FILE && unique_id) { //if it's a file and it DOES have a unique_id
		temp->unique_id = malloc(sizeof(char) * 100); //allocate some space for temp unique_id
		sprintf(temp->unique_id, "%s", unique_id); //put the unique_id in the temp node
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
	if (child->unique_id)
		free(child->unique_id);
	if (child)
		free(child);
}

void deleteNode(NODE temp) //makes life easier later to just call this function
{
	deleteChild(temp->parent, temp); //call the deleteChild on the child of the child's parent, which is the child. Confusing.
}


char* string_after_char(const char* path, char after) //quick function used to grab extensions from file paths
{
	char* end = (char*)path;
	while (*end) end++; //set end to the end of the string
	while(end > path && *end != after) end--; //once it's at the end, go backwards until you reach the character we want to "stop" at
	if (end != path || *end == after)
		return end + 1;

	return NULL; //something went wrong
}


void getFullPath(const char* path, char* full)  //get the full path, again, just makes life easier
{
	sprintf(full, "%s/%s", filevaultdir, path);
}

NODE _node_for_path(char* path, NODE curr, bool create, NODE_TYPE type, char* unique_id, bool ignore_ext) //function to use for 'overloading'
{

	char name[1000];
	int i = 0;
	bool last_node = false;
	char* ext;
	char compare_name[1024];
	char* curr_char;
	int n = 0;
	
	ext = string_after_char(path, '.');

	if (*path == '/')
		path++;

	i = 0;
	while(*path && *path != '/' && ( ext == NULL || (path < ext-1 || !ignore_ext))) {
		name[i++] = *(path++); //  Dates/2014/November/
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
			return _node_for_path(path, curr->children[i], create, type, unique_id, ignore_ext);
		*compare_name = '\0';
	}

	
	// sorry about this weird line
	if (create) {
		return _node_for_path(path, addChild(curr, init_node(name, last_node ? type : NODE_DIR, unique_id)), create, type, unique_id, ignore_ext);
	}

	return NULL;
}

//_node_for_path(char* path, NODE curr, bool create, NODE_TYPE type, char* unique_id, bool ignore_ext)

NODE node_for_path(const char* path) 
{
        return _node_for_path((char*)path, root, false, 0, NULL, false);
}

NODE create_node_for_path(const char* path, NODE_TYPE type, char* unique_id)
{
	return _node_for_path((char*)path, root, true, type, unique_id, false);
}

NODE node_ignore_extension(const char* path)
{
	return _node_for_path((char*)path, root, false, 0, NULL, true);
}


// from a fuse example
static int mypfs_getattr(const char *path, struct stat *stbuf) //grabbing the file attributes we need
{
	int res = 0;
	NODE file_node;
	NODE file_node_ignore_ext;
	char full_file_name[1000];

	file_node = node_for_path(path);
	file_node_ignore_ext = node_ignore_extension(path);
	if (file_node_ignore_ext == NULL)
		return -ENOENT;
		
	getFullPath(file_node_ignore_ext->unique_id, full_file_name);

	if (file_node_ignore_ext && file_node_ignore_ext->type == NODE_FILE && file_node_ignore_ext != file_node) {
		// convert here, so file 1324242 becomes 1324242.png
		puts("DEBUG: EXTENSION DOESN'T MATCH; NEED TO CONVERT");
		//convert_img(file_node_ignore_ext, path);
		
		// fix path stuff for stat()
		strcat(full_file_name, ".");
		strcat(full_file_name, string_after_char(path, '.'));
		printf("DEBUG: full_file_name ==> %s\n", full_file_name);
	}

	
	//Most from below is grabbed from hello.c, but added some stat() calls for the hidden folder later
	memset(stbuf, 0, sizeof(struct stat));
	if (file_node_ignore_ext->type == NODE_DIR) { //if (strcmp(path, "/") == 0
		stbuf->st_mode = S_IFDIR | 0444;
		stbuf->st_nlink = 2;
	} else if (file_node_ignore_ext != NULL && file_node != file_node_ignore_ext) {
		puts("DEBUG: getattr for non-original file ext");
		stat(full_file_name, stbuf);
		stbuf->st_mode = S_IFREG | 0444;
	} else if (file_node_ignore_ext != NULL) {
		printf("DEBUG: full_file_name ==> %s\n", full_file_name);
		stat(full_file_name, stbuf);
	} else
		res = -ENOENT;

	return res;
}

static int mypfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	int i;
	(void) offset;
	(void) fi;
	NODE file_node;
	
	file_node = node_for_path(path);
	
	if (file_node == NULL)
		return -ENOENT;

	filler(buf, ".", NULL, 0);  //grabbed from the example (hello.c)
	filler(buf, "..", NULL, 0); //grabbed from the example (hello.c)
	
	for (i = 0; i < file_node->num_children; i++) {
		filler(buf, file_node->children[i]->name, NULL, 0); //put the node's "children" aka subdirectories into buf
	}

	return 0;
}

// from example
static int mypfs_open(const char *path, struct fuse_file_info *fi)
{
	NODE file_node;
	char full_file_name[1000];

	file_node = node_ignore_extension(path);

	if (file_node == NULL)  
		        return -ENOENT;

	getFullPath(file_node->unique_id, full_file_name);

	// different extensions
	if (strcmp(string_after_char(path, '.'), string_after_char(file_node->name, '.'))) {
		strcat(full_file_name, ".");
		strcat(full_file_name, string_after_char(path, '.'));
	}

	fi->fh = open(full_file_name, fi->flags, 0666); //Owner read/write

	if(fi->fh == -1) {
	        puts("DEBUG: fd == -1");
	        return -errno;
	}

	file_node->open_count++;

	return 0;
}

// from example
static int mypfs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	(void) fi;
	NODE file_node;

	file_node = node_ignore_extension(path);

	if (file_node == NULL)
		return -ENOENT;

	size = pread(fi->fh, buf, size, offset);
	
	if (size < 0)
		puts("Debug: READ ERROR");

	return size;
}

static int mypfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	NODE new_node;
	char* end = (char*)path;
	int i;
	int num_slashes = 0;

	
	//Grabs the filename from the path by getting everything after the last /
	while(*end != '\0') end++;
	while(*end != '/' && end >= path) end--;
	if (*end == '/') end++;

	//count number of slashes in filename
	for(i = 0; i < strlen(path); i++) {
		if (path[i] == '/') num_slashes++;
	}
	
	// no writing to non-root folders
	// /~root~/~something else~ has at least 2 slashes if it's not root directory
	if (num_slashes > 1 ) {
		return -1;
	}

	new_node = init_node(end, NODE_FILE, NULL);
	puts("DEBUG: create");
	addChild(root, new_node);
	return mypfs_open(path, fi);
}

static int mypfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

	int res;
	char full_file_name[1000];
	
	NODE file_node = node_ignore_extension(path);
	NODE real_node = node_for_path(path);
	
	puts("DEBUG: write");
	getFullPath(file_node->unique_id, full_file_name);
	printf("DEBUG: full_file_name ==> %s\n", full_file_name);

	if (file_node != real_node)
		return -1;

	res = pwrite(fi->fh, buf, size, offset);
	if(res == -1) {
		puts("DEBUG: pwrite error");
		res = -errno;
	}


	return res;
}


static int mypfs_rename(const char *from, const char *to)
{
	NODE old_node;
	NODE new_node;
	puts("DEBUG: rename");

	old_node = node_for_path(from);
	
	
	new_node = create_node_for_path(to, old_node->type, old_node->unique_id);
	
	if (new_node != old_node) //making sure the user isn't renaming the file to the same name
		deleteNode(old_node);
		
	return 0;

}


/*
From the Fuse documentation:

"Release is called when there are no more references to an open file: 
all file descriptors are closed and all memory mappings are unmapped.

For every open() call there will be exactly one release() call 
with the same flags and file descriptor."

*/
static int mypfs_release(const char *path, struct fuse_file_info *fi) //basically, this is where the magic happens in terms of directory sorting
{
	ExifData *ed; //temp var for the ExifData we need to pull from the file
	ExifEntry *entry;
	
	char full_file_name[1000];
	NODE file_node = node_ignore_extension(path);
	
	char buf[1024];
	struct tm file_time;
	char year[1024];
	char month[1024];
	char new_name[2048];

	getFullPath(file_node->unique_id, full_file_name);
	close(fi->fh); //close the file handle"
	file_node->open_count--;

	// redetermine where the file goes
	if (file_node->open_count <= 0) {
		puts("DEBUG: file completely closed; checking if renaming necessary");
		ed = exif_data_new_from_file(full_file_name);
		if (ed) {
			entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_DATE_TIME);
			exif_entry_get_value(entry, buf, sizeof(buf));
			
			//time string formatting
			strptime(buf, "%Y:%m:%d %H:%M:%S", &file_time);
			strftime(year, 1024, "%Y", &file_time);
			strftime(month, 1024, "%B", &file_time);
			//-----------------
			
			
			sprintf(new_name, "/Dates/%s/%s/%s", year, month, file_node->name); //file path is now /Dates/year/month/<file>; write this to new_name
			printf("DEBUG: new_name ==> %s\n", new_name);
			
			
			mypfs_rename(path, new_name); //call rename to make sure nodes stay together correctly
			exif_data_unref(ed); //we're done with exif data, so get rid of it
			
		} else {  //file had no exif data, so we're gonna invent some
		
			int num_slashes = 0;
			int i;
			time_t rawtime;

			for (i = 0; i < strlen(path); i++) {
				if (path[i] == '/')
					num_slashes++;
			}
			
			// if path only has 1 slash, then we're inside of the 'root' folder, and thus allowed to be there
			if (num_slashes == 1) {
				struct tm * now_time;
				time(&rawtime);
				now_time = localtime(&rawtime); //put the current time in now_time
				strftime(year, 1024, "%Y", now_time);
				strftime(month, 1024, "%B", now_time);
				sprintf(new_name, "/Dates/%s/%s/%s", year, month, file_node->name); //same as before, but pretending that the exif data was today's date
				mypfs_rename(path, new_name); //again, call rename for node stuff

			}
		}
	}

	return 0;
}

static int mypfs_unlink(const char *path)
{
	char full_file_name[1000];
	int res = 0;
	NODE file_node = node_for_path(path);
	puts("DEBUG: unlink");
	getFullPath(file_node->unique_id, full_file_name);
	deleteNode(file_node);
	res = unlink(full_file_name);
	if(res == -1) {
		puts("DEBUG:res = -1");
	} else if (res == 0) {
		puts("DEBUG:res = 0");
	} else {
		puts("DEBUG:res = not 0 or -1");
	}
	return res;
}

static int mypfs_mkdir(const char *path, mode_t mode) //mkdir not permitted inside the FS to preserve folder integrity
{
	return -1;
}

static int mypfs_opendir(const char *path, struct fuse_file_info *fi)
{
	NODE node = node_for_path(path);
	puts("DEBUG: opendir");

	if (node && node->type == NODE_DIR) //if the filetype is a directory, open it!
		return 0;

	return -1; //trying to open a 'file' that isn't a directory won't work
}

static void* mypfs_init(struct fuse_conn_info *conn)
{
	return NULL;
}

static struct fuse_operations mypfs_oper = {
	.getattr	= mypfs_getattr,
	.readdir	= mypfs_readdir,
	.open		= mypfs_open,
	.read		= mypfs_read,
	.create		= mypfs_create,
	.write		= mypfs_write,
	.release	= mypfs_release,
	.unlink		= mypfs_unlink,
	.rename		= mypfs_rename,
	.mkdir		= mypfs_mkdir,
	.opendir	= mypfs_opendir,
	.init		= mypfs_init,
};

int main(int argc, char *argv[])
{
	puts("DEBUG: ===========start_filesystem============");
	srandom(time(NULL));
	getFileVaultDirectory();
	printf("mkdir filevaultdir: %d\n", mkdir(filevaultdir, 0777));
	root = init_node("/", NODE_DIR, NULL);
	
	return fuse_main(argc, argv, &mypfs_oper, NULL);
}