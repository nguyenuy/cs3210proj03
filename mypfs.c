/*
  gcc -Wall `pkg-config fuse --cflags --libs` hello.c -o hello
*/

#define FUSE_USE_VERSION 29
#define __USE_XOPEN //fixes a bug
#define _GNU_SOURCE //fixes the same bug, just gonna leave it in since it doesn't seem to break anything

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
#include <libssh/libssh.h>
#include <libssh/sftp.h>

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

// Used for SFTP Session with factor-controller.cc.gatech.edu
const char *host = "130.207.21.100";
ssh_session my_ssh_session;
sftp_session sftp;


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

NODE initNode(const char* name, NODE_TYPE type, char* unique_id)
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
	struct _node **oldChildren = parent->children; //preserve the old children pointer
	int oldNum = parent->num_children; //preserve the old count
	
	struct _node **newChildren = malloc(sizeof(NODE) * (oldNum + 1)); //going to need space for 1 more child

	int i;
	for (i = 0; i < oldNum; i++) {
		newChildren[i] = oldChildren[i]; //copy the old children to the new children pointer
	}

	newChildren[oldNum] = child; //set the last entry to the newest child

	parent->children = newChildren; //fix the children pointer
	parent->num_children = oldNum + 1; //fix the parent's children count

	child->parent = parent; //set its parent

	free(oldChildren); //since we malloc'd new space for all the old children too, we can free this now

	return child;
}

void deleteChild(NODE parent, NODE child)
{
	struct _node **oldChildren = parent->children; //preserve the old children pointer
	int oldNum = parent->num_children; //preserve the old count
	
	struct _node **newChildren;
	int i;
	if (oldNum <= 0) //if it has no children, we can't remove anything
		return;

	newChildren = malloc(sizeof(NODE) * (oldNum - 1)); //allocate a new smaller space for the old children

	int new_index = 0;
	for (i = 0; i < oldNum; i++) { //loop over all the children, and only add them to the new pointer if they're not the one we want to remove
		if (oldChildren[i] != child) {
			newChildren[new_index++] = oldChildren[i];
		}
	}

	parent->children = newChildren; //fix the parent's children pointer
	parent->num_children = oldNum - 1; //fix the parent's children count
	
	//free all the memory we've been using
	//if statements serve to prevent freeing null pointers
	
	if (oldChildren) //free old children first since the array is no longer needed
		free(oldChildren);
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


char* stringAfterCharacter(const char* path, char after) //quick function used to grab extensions from file paths
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

NODE nodeForGivenPath(char* path, NODE curr, bool create, NODE_TYPE type, char* unique_id, bool ignore_ext) //function to use for 'overloading'
{

	char name[1000];
	int i = 0;
	bool last_node = false;
	char* ext;
	char compare_name[1024];
	char* curr_char;
	int n = 0;
	
	ext = stringAfterCharacter(path, '.');

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
		ext = stringAfterCharacter(curr->children[i]->name, '.');
		curr_char = curr->children[i]->name;
		n = 0;
		while(*curr_char != '\0' && (ext == NULL || (!ignore_ext || curr_char < ext-1))) {
			compare_name[n++] = *(curr_char++);
		}
		compare_name[n] = '\0';
		if (0 == strcmp(name, compare_name))
			return nodeForGivenPath(path, curr->children[i], create, type, unique_id, ignore_ext);
		*compare_name = '\0';
	}

	if (create) {
		if(last_node) { //new node is trailing end of the path, and it might not be a directory
			return nodeForGivenPath(path, addChild(curr, initNode(name, type, unique_id)), create, type, unique_id, ignore_ext);
		} else { //if it's not the trailing end of the path, it's going to be a new directory
			return nodeForGivenPath(path, addChild(curr, initNode(name, NODE_DIR, unique_id)), create, type, unique_id, ignore_ext);
		}
	}

	return NULL;
}

//nodeForGivenPath(char* path, NODE curr, bool create, NODE_TYPE type, char* unique_id, bool ignore_ext)

NODE nodeForPath(const char* path) 
{
        return nodeForGivenPath((char*)path, root, false, 0, NULL, false);
}

NODE createnodeForGivenPath(const char* path, NODE_TYPE type, char* unique_id)
{
	return nodeForGivenPath((char*)path, root, true, type, unique_id, false);
}

NODE nodeIgnoreEXT(const char* path)
{
	return nodeForGivenPath((char*)path, root, false, 0, NULL, true);
}


// mostly from a fuse example
static int mypfs_getattr(const char *path, struct stat *stbuf) //grabbing the file attributes we need
{
	int res = 0;
	NODE fileNode;
	NODE fileNodeIgnoreExt;
	char fullFileName[1000];

	fileNode = nodeForPath(path);
	fileNodeIgnoreExt = nodeIgnoreEXT(path);
	if (fileNodeIgnoreExt == NULL)
		return -ENOENT;
		
	getFullPath(fileNodeIgnoreExt->unique_id, fullFileName);

	if (fileNodeIgnoreExt && fileNodeIgnoreExt->type == NODE_FILE && fileNodeIgnoreExt != fileNode) {
		// fix path stuff for stat()
		strcat(fullFileName, ".");
		strcat(fullFileName, stringAfterCharacter(path, '.'));
		printf("DEBUG: fullFileName ==> %s\n", fullFileName);
	}

	
	//Most from below is grabbed from hello.c, but added some stat() calls for the hidden folder later
	memset(stbuf, 0, sizeof(struct stat));
	if (fileNodeIgnoreExt->type == NODE_DIR) { //if (strcmp(path, "/") == 0
		stbuf->st_mode = S_IFDIR | 0444;
		stbuf->st_nlink = 2;
	} else if (fileNodeIgnoreExt != NULL && fileNode != fileNodeIgnoreExt) {
		puts("DEBUG: getattr for non-original file ext");
		stat(fullFileName, stbuf);
		stbuf->st_mode = S_IFREG | 0444;
	} else if (fileNodeIgnoreExt != NULL) {
		printf("DEBUG: fullFileName ==> %s\n", fullFileName);
		stat(fullFileName, stbuf);
	} else
		res = -ENOENT;

	return res;
}

static int mypfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	int i;
	(void) offset; //
	(void) fi;     //from the fuse examples, these 2 lines of code are currently based on magic
	NODE fileNode;
	
	fileNode = nodeForPath(path);
	
	if (fileNode == NULL)
		return -ENOENT;

	filler(buf, ".", NULL, 0);  //grabbed from the example (hello.c)
	filler(buf, "..", NULL, 0); //grabbed from the example (hello.c)
	
	for (i = 0; i < fileNode->num_children; i++) {
		filler(buf, fileNode->children[i]->name, NULL, 0); //put the node's "children" aka subdirectories into buf
	}

	return 0;
}

// from example
static int mypfs_open(const char *path, struct fuse_file_info *fi)
{
	NODE fileNode;
	char fullFileName[1000];

	fileNode = nodeIgnoreEXT(path);

	if (fileNode == NULL)  
		        return -ENOENT;

	getFullPath(fileNode->unique_id, fullFileName);

	// different extensions
	if (strcmp(stringAfterCharacter(path, '.'), stringAfterCharacter(fileNode->name, '.'))) {
		strcat(fullFileName, ".");
		strcat(fullFileName, stringAfterCharacter(path, '.'));
	}

	fi->fh = open(fullFileName, fi->flags, 0666); //Owner read/write

	if(fi->fh == -1) {
	        puts("DEBUG: fd == -1");
	        return -errno;
	}

	fileNode->open_count++;

	return 0;
}

// from example
static int mypfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	(void) fi;
	NODE fileNode;

	fileNode = nodeIgnoreEXT(path);

	if (fileNode == NULL)
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

	new_node = initNode(end, NODE_FILE, NULL);
	puts("DEBUG: create");
	addChild(root, new_node);
	return mypfs_open(path, fi);
}

static int mypfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

	int res;
	char fullFileName[1000];
	
	NODE fileNode = nodeIgnoreEXT(path);
	NODE real_node = nodeForPath(path);
	
	puts("DEBUG: write");
	getFullPath(fileNode->unique_id, fullFileName);
	printf("DEBUG: fullFileName ==> %s\n", fullFileName);

	if (fileNode != real_node)
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

	old_node = nodeForPath(from);
	
	
	new_node = createnodeForGivenPath(to, old_node->type, old_node->unique_id);
	
	if (new_node != old_node) //making sure the user isn't renaming the file to the same name
		deleteNode(old_node);
		
	return 0;

}


int uploadToServer(char *fullFileName, char *uploadName, char *upload_dir) {
    ssize_t ret_in, ret_out;
    int buf_size = 8192;
    char buffer[buf_size];
	
	//make sure there's a special directory to upload into
	sftp_mkdir(sftp, upload_dir, S_IRWXU);
	int filehandle = open(fullFileName,O_RDWR, 0666);
	
	sftp_file file;
	
	file = sftp_open(sftp, uploadName, O_WRONLY | O_TRUNC | O_CREAT | O_APPEND, S_IRWXU);
	while ((ret_in = read(filehandle, &buffer, buf_size)) > 0) {  
		sftp_write(file, &buffer, (size_t) ret_in);
	}
	sftp_close(file);
	close(filehandle);
	return 0;
}

//function left mostly unimplemented
//parts pulled from sshlib's documentation section on SFTP
int pullFromServer() {
	int access_type;
	sftp_file file;
	char buffer[16384];
	int nbytes, nwritten, rc;
	int fd;
	
	access_type = O_RDONLY;
	file = sftp_open(sftp, "mypfsPics/Patern_test.jpg", access_type, 0);
	if(file == NULL) {
		fprintf(stderr, "Can't open file for reading: %s\n", ssh_get_error(my_ssh_session));
		return SSH_ERROR;
	}
	
	fd = open("/home/ubuntu/.downloadedpics/Pattt.jpg", (O_CREAT | O_RDWR), S_IRWXU);
	if (fd < 0) {
		fprintf(stderr, "Can't open file for writing: %s\n",
		strerror(errno));
		return SSH_ERROR;
	}
	for (;;) {
		nbytes = sftp_read(file, buffer, sizeof(buffer));
		if (nbytes == 0) {
			break; // EOF
		} else if (nbytes < 0) {
			fprintf(stderr, "Error while reading file: %s\n", ssh_get_error(my_ssh_session));
			sftp_close(file);
			return SSH_ERROR;
		}
		nwritten = write(fd, buffer, nbytes);
		if (nwritten != nbytes) {
			fprintf(stderr, "Error writing: %s\n", strerror(errno));
			sftp_close(file);
			return SSH_ERROR;
		}
	}
	rc = sftp_close(file);
	if (rc != SSH_OK) {
		fprintf(stderr, "Can't close the read file: %s\n", ssh_get_error(my_ssh_session));
		return rc;
	}
	return SSH_OK;
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
	
	char fullFileName[1000];
	NODE fileNode = nodeIgnoreEXT(path);
	
	char buf[1024];
	struct tm file_time;
	char year[1024];
	char month[1024];
	char new_name[2048];
	char uploadName[1024];
	char upload_dir[1024];

	getFullPath(fileNode->unique_id, fullFileName);
	close(fi->fh); //close the file handle
	fileNode->open_count--;

	// redetermine where the file goes
	if (fileNode->open_count <= 0) {
		puts("DEBUG: file completely closed; checking if renaming necessary");
		ed = exif_data_new_from_file(fullFileName);
		if (ed) {
			entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_DATE_TIME);
			exif_entry_get_value(entry, buf, sizeof(buf));
			
			//time string formatting
			strptime(buf, "%Y:%m:%d %H:%M:%S", &file_time);
			strftime(year, 1024, "%Y", &file_time);
			strftime(month, 1024, "%B", &file_time);
			//-----------------
			
			
			sprintf(new_name, "/Dates/%s/%s/%s", year, month, fileNode->name); //file path is now /Dates/year/month/<file>; write this to new_name
			sprintf(upload_dir, "mypfsPics/");
			printf("DEBUG: new_name ==> %s\n", new_name);
			
			sprintf(uploadName, "%s%s", upload_dir, stringAfterCharacter(path, '/'));
			
			uploadToServer(fullFileName, uploadName, upload_dir);
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
				sprintf(new_name, "/Dates/%s/%s/%s", year, month, fileNode->name); //same as before, but pretending that the exif data was today's date
				
				sprintf(upload_dir, "mypfsPics/");
				sprintf(uploadName, "%s%s", upload_dir, stringAfterCharacter(path, '/'));
				uploadToServer(fullFileName, uploadName, upload_dir);
				
				mypfs_rename(path, new_name); //again, call rename for node stuff

			}
		}
	}

	return 0;
}

static int mypfs_unlink(const char *path)
{
	char fullFileName[1000];
	int res = 0;
	NODE fileNode = nodeForPath(path);
	puts("DEBUG: unlink");
	getFullPath(fileNode->unique_id, fullFileName);
	deleteNode(fileNode);
	
	//some debug info to make sure unlink() worked successfully
	res = unlink(fullFileName);
	if(res == -1) {
		puts("DEBUG:res = -1");
	} else if (res == 0) {
		puts("DEBUG:res = 0");
	} else {
		puts("DEBUG:res = not 0 or -1, something broke");
	}
	return res; //if this is -1, something went wrong with unlink()
}

static int mypfs_mkdir(const char *path, mode_t mode) //mkdir not permitted inside the FS to preserve folder integrity
{
	return -1;
}

static int mypfs_opendir(const char *path, struct fuse_file_info *fi)
{
	NODE node = nodeForPath(path);
	puts("DEBUG: opendir");

	if (node && node->type == NODE_DIR) //if the filetype is a directory, open it!
		return 0;

	return -1; //trying to open something that isn't a directory throws an "Operation not supported" error
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
	root = initNode("/", NODE_DIR, NULL);
	
	// SSH Testing right here
   
   my_ssh_session = ssh_new();
   // This is the the factor-3210 server
   //const char *user = "unguyen3";
   //const char *password = "";
   char user[256];
   const char *password;
   printf("Input a username: ");
   scanf("%s", &user);
   int rc;
   
   //Create the SSH Session First
	if (my_ssh_session == NULL)
		puts("DEBUG: my_ssh_session is NULL");
   
   ssh_options_set(my_ssh_session, SSH_OPTIONS_HOST, host);
   ssh_options_set(my_ssh_session, SSH_OPTIONS_USER, user);
   
   if(ssh_connect(my_ssh_session)){
		printf("DEBUG: Connection failed: %s\n", ssh_get_error(my_ssh_session));
   } else {
		puts("DEBUG: Successfully established SSH session");
		password = getpass("Enter your password: ");
		rc = ssh_userauth_password(my_ssh_session, user, password);
		if (rc == SSH_AUTH_ERROR) {
			puts("DEBUG: Error authorizing connection");
		} else {
			puts("DEBUG: Successfully authenticated connection");
		}
   }
   
   //Next, create the SFTP Session
   sftp = sftp_new(my_ssh_session);
   if (sftp == NULL) {
		puts("DEBUG: sftp connection unsuccessful");
   } else {
		puts("DEBUG: sftp connection successful");
   }
   
   sftp_mkdir(sftp, "mypfsPics", S_IRWXU);
   return fuse_main(argc, argv, &mypfs_oper, NULL);
}
