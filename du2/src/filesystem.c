#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "filesystem.h"
#include "util.h"

#define FS_MAGIC 0x46533132u
#define NO_SECTOR 0u
#define ROOT_INODE_SECTOR 1u
#define DATA_BYTES (SECTOR_SIZE - sizeof(uint32_t))
#define DIR_ENTRIES_PER_SECTOR (DATA_BYTES / sizeof(dir_entry_t))
#define MAX_FILE_SIZE (1u << 30)
#define SYMLINK_DEPTH_LIMIT 16

#define FD_OFFSET 0
#define FD_INODE 1
#define FD_TYPE 2
#define FD_RESERVED 3

typedef struct {
	uint32_t magic;
	uint32_t total_sectors;
	uint32_t free_head;
	uint32_t root_inode;
} superblock_t;

typedef struct {
	uint32_t type;
	uint32_t size;
	uint32_t nlink;
	uint32_t first_data;
	uint32_t reserved[28];
} inode_t;

typedef struct {
	char name[MAX_FILENAME];
	uint32_t inode_sector;
} dir_entry_t;

static size_t bounded_strlen(const char *s, size_t limit)
{
	size_t i;

	if (s == NULL)
		return limit;

	for (i = 0; i < limit && s[i] != '\0'; i++)
		;
	return i;
}

static uint32_t load_u32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static void store_u32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, sizeof(v));
}

static int read_superblock(superblock_t *sb)
{
	uint8_t buffer[SECTOR_SIZE];

	if (sb == NULL)
		return FAIL;

	hdd_read(0, buffer);
	memcpy(sb, buffer, sizeof(*sb));

	if (sb->magic != FS_MAGIC)
		return FAIL;

	return OK;
}

static int write_superblock(const superblock_t *sb)
{
	uint8_t buffer[SECTOR_SIZE] = { 0 };

	if (sb == NULL)
		return FAIL;

	memcpy(buffer, sb, sizeof(*sb));
	hdd_write(0, buffer);
	return OK;
}

static int read_inode(uint32_t sector, inode_t *inode)
{
	uint8_t buffer[SECTOR_SIZE];
	superblock_t sb;

	if (inode == NULL || read_superblock(&sb) != OK)
		return FAIL;
	if (sector == NO_SECTOR || sector >= sb.total_sectors)
		return FAIL;

	hdd_read(sector, buffer);
	memcpy(inode, buffer, sizeof(*inode));
	return OK;
}

static int write_inode(uint32_t sector, const inode_t *inode)
{
	uint8_t buffer[SECTOR_SIZE] = { 0 };
	superblock_t sb;

	if (inode == NULL || read_superblock(&sb) != OK)
		return FAIL;
	if (sector == NO_SECTOR || sector >= sb.total_sectors)
		return FAIL;

	memcpy(buffer, inode, sizeof(*inode));
	hdd_write(sector, buffer);
	return OK;
}

static uint32_t get_next_sector(uint32_t sector)
{
	uint8_t buffer[SECTOR_SIZE];

	if (sector == NO_SECTOR)
		return NO_SECTOR;

	hdd_read(sector, buffer);
	return load_u32(buffer + DATA_BYTES);
}

static int set_next_sector(uint32_t sector, uint32_t next)
{
	uint8_t buffer[SECTOR_SIZE];

	if (sector == NO_SECTOR)
		return FAIL;

	hdd_read(sector, buffer);
	store_u32(buffer + DATA_BYTES, next);
	hdd_write(sector, buffer);
	return OK;
}

static uint32_t alloc_sector(void)
{
	superblock_t sb;
	uint8_t buffer[SECTOR_SIZE];
	uint32_t sector;
	uint32_t next;

	if (read_superblock(&sb) != OK || sb.free_head == NO_SECTOR)
		return NO_SECTOR;

	sector = sb.free_head;
	hdd_read(sector, buffer);
	next = load_u32(buffer);

	sb.free_head = next;
	write_superblock(&sb);

	memset(buffer, 0, sizeof(buffer));
	hdd_write(sector, buffer);

	return sector;
}

static int free_sector(uint32_t sector)
{
	superblock_t sb;
	uint8_t buffer[SECTOR_SIZE] = { 0 };

	if (sector <= ROOT_INODE_SECTOR || read_superblock(&sb) != OK ||
	    sector >= sb.total_sectors)
		return FAIL;

	store_u32(buffer, sb.free_head);
	hdd_write(sector, buffer);

	sb.free_head = sector;
	write_superblock(&sb);
	return OK;
}

static void free_data_chain(uint32_t first)
{
	uint32_t sector = first;

	while (sector != NO_SECTOR) {
		uint32_t next = get_next_sector(sector);

		free_sector(sector);
		sector = next;
	}
}

static uint32_t get_data_sector_for_offset(const inode_t *inode, uint32_t offset)
{
	uint32_t sector;
	uint32_t hops;
	uint32_t i;

	if (inode == NULL || inode->first_data == NO_SECTOR)
		return NO_SECTOR;

	sector = inode->first_data;
	hops = offset / DATA_BYTES;
	for (i = 0; i < hops && sector != NO_SECTOR; i++)
		sector = get_next_sector(sector);

	return sector;
}

static uint32_t ensure_data_sector_for_offset(uint32_t inode_sector,
					      inode_t *inode,
					      uint32_t offset)
{
	uint32_t sector;
	uint32_t hops;
	uint32_t i;

	if (inode == NULL)
		return NO_SECTOR;

	if (inode->first_data == NO_SECTOR) {
		sector = alloc_sector();
		if (sector == NO_SECTOR)
			return NO_SECTOR;
		inode->first_data = sector;
		write_inode(inode_sector, inode);
	}

	sector = inode->first_data;
	hops = offset / DATA_BYTES;
	for (i = 0; i < hops; i++) {
		uint32_t next = get_next_sector(sector);

		if (next == NO_SECTOR) {
			next = alloc_sector();
			if (next == NO_SECTOR)
				return NO_SECTOR;
			set_next_sector(sector, next);
		}
		sector = next;
	}

	return sector;
}

static int valid_name_char(char c)
{
	return (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') ||
	       c == '.' || c == '-' || c == '_';
}

static int validate_name(const char *name)
{
	size_t len;
	size_t i;

	if (name == NULL)
		return FAIL;

	len = bounded_strlen(name, MAX_FILENAME + 1);
	if (len == 0 || len > MAX_FILENAME)
		return FAIL;

	for (i = 0; i < len; i++) {
		if (!valid_name_char(name[i]))
			return FAIL;
	}

	return OK;
}

static int validate_path(const char *path)
{
	size_t len;
	size_t i;
	size_t component_len = 0;

	if (path == NULL || path[0] != PATHSEP)
		return FAIL;

	len = bounded_strlen(path, MAX_PATH + 1);
	if (len == 0 || len > MAX_PATH)
		return FAIL;

	if (len == 1)
		return OK;

	if (path[len - 1] == PATHSEP)
		return FAIL;

	for (i = 1; i < len; i++) {
		if (path[i] == PATHSEP) {
			if (component_len == 0 || component_len > MAX_FILENAME)
				return FAIL;
			component_len = 0;
			continue;
		}
		if (!valid_name_char(path[i]))
			return FAIL;
		component_len++;
		if (component_len > MAX_FILENAME)
			return FAIL;
	}

	return component_len > 0 ? OK : FAIL;
}

static int split_parent_child(const char *path,
			      char parent[MAX_PATH + 1],
			      char child[MAX_FILENAME + 1])
{
	size_t len;
	size_t slash = 0;
	size_t child_len;
	size_t i;

	if (validate_path(path) != OK || strcmp(path, "/") == 0)
		return FAIL;

	len = strlen(path);
	for (i = 0; i < len; i++) {
		if (path[i] == PATHSEP)
			slash = i;
	}

	child_len = len - slash - 1;
	if (child_len == 0 || child_len > MAX_FILENAME)
		return FAIL;

	if (slash == 0) {
		strcpy(parent, "/");
	} else {
		memcpy(parent, path, slash);
		parent[slash] = '\0';
	}

	memcpy(child, path + slash + 1, child_len);
	child[child_len] = '\0';

	return validate_name(child);
}

static int dir_entry_valid(const dir_entry_t *entry)
{
	return entry != NULL && entry->inode_sector != NO_SECTOR && entry->name[0] != '\0';
}

static void read_dir_entry(const uint8_t *buffer, size_t slot, dir_entry_t *entry)
{
	memcpy(entry, buffer + slot * sizeof(*entry), sizeof(*entry));
}

static void write_dir_entry(uint8_t *buffer, size_t slot, const dir_entry_t *entry)
{
	memcpy(buffer + slot * sizeof(*entry), entry, sizeof(*entry));
}

static int find_entry_in_dir(uint32_t dir_sector,
			     const char *name,
			     uint32_t *inode_sector,
			     uint32_t *entry_sector,
			     uint32_t *entry_slot)
{
	inode_t dir;
	uint32_t sector;

	if (validate_name(name) != OK || read_inode(dir_sector, &dir) != OK ||
	    dir.type != STAT_TYPE_DIR)
		return FAIL;

	for (sector = dir.first_data; sector != NO_SECTOR; sector = get_next_sector(sector)) {
		uint8_t buffer[SECTOR_SIZE];
		size_t i;

		hdd_read(sector, buffer);
		for (i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
			dir_entry_t entry;

			read_dir_entry(buffer, i, &entry);
			if (dir_entry_valid(&entry) &&
			    strncmp(entry.name, name, MAX_FILENAME) == 0) {
				if (inode_sector != NULL)
					*inode_sector = entry.inode_sector;
				if (entry_sector != NULL)
					*entry_sector = sector;
				if (entry_slot != NULL)
					*entry_slot = (uint32_t)i;
				return OK;
			}
		}
	}

	return FAIL;
}

static int dir_has_add_space(uint32_t dir_sector)
{
	superblock_t sb;
	inode_t dir;
	uint32_t sector;

	if (read_inode(dir_sector, &dir) != OK || dir.type != STAT_TYPE_DIR ||
	    read_superblock(&sb) != OK)
		return 0;

	for (sector = dir.first_data; sector != NO_SECTOR; sector = get_next_sector(sector)) {
		uint8_t buffer[SECTOR_SIZE];
		size_t i;

		hdd_read(sector, buffer);
		for (i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
			dir_entry_t entry;

			read_dir_entry(buffer, i, &entry);
			if (!dir_entry_valid(&entry))
				return 1;
		}
	}

	return sb.free_head != NO_SECTOR;
}

static int add_entry_to_dir(uint32_t dir_sector, const char *name, uint32_t child_inode)
{
	inode_t dir;
	uint32_t sector;
	uint32_t last = NO_SECTOR;
	dir_entry_t new_entry;

	if (child_inode == NO_SECTOR || validate_name(name) != OK ||
	    read_inode(dir_sector, &dir) != OK || dir.type != STAT_TYPE_DIR)
		return FAIL;

	if (find_entry_in_dir(dir_sector, name, NULL, NULL, NULL) == OK)
		return FAIL;

	memset(&new_entry, 0, sizeof(new_entry));
	memcpy(new_entry.name, name, strlen(name));
	new_entry.inode_sector = child_inode;

	sector = dir.first_data;
	while (sector != NO_SECTOR) {
		uint8_t buffer[SECTOR_SIZE];
		size_t i;

		hdd_read(sector, buffer);
		for (i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
			dir_entry_t entry;

			read_dir_entry(buffer, i, &entry);
			if (!dir_entry_valid(&entry)) {
				write_dir_entry(buffer, i, &new_entry);
				hdd_write(sector, buffer);
				dir.size += sizeof(dir_entry_t);
				write_inode(dir_sector, &dir);
				return OK;
			}
		}
		last = sector;
		sector = get_next_sector(sector);
	}

	sector = alloc_sector();
	if (sector == NO_SECTOR)
		return FAIL;

	if (last == NO_SECTOR) {
		dir.first_data = sector;
		write_inode(dir_sector, &dir);
	} else {
		set_next_sector(last, sector);
	}

	{
		uint8_t buffer[SECTOR_SIZE] = { 0 };

		write_dir_entry(buffer, 0, &new_entry);
		hdd_write(sector, buffer);
	}

	dir.size += sizeof(dir_entry_t);
	write_inode(dir_sector, &dir);
	return OK;
}

static int remove_entry_from_dir(uint32_t dir_sector, const char *name)
{
	inode_t dir;
	uint32_t entry_sector;
	uint32_t entry_slot;
	uint8_t buffer[SECTOR_SIZE];
	dir_entry_t empty_entry;

	if (find_entry_in_dir(dir_sector, name, NULL, &entry_sector, &entry_slot) != OK ||
	    read_inode(dir_sector, &dir) != OK)
		return FAIL;

	memset(&empty_entry, 0, sizeof(empty_entry));
	hdd_read(entry_sector, buffer);
	write_dir_entry(buffer, entry_slot, &empty_entry);
	hdd_write(entry_sector, buffer);

	if (dir.size >= sizeof(dir_entry_t))
		dir.size -= sizeof(dir_entry_t);
	write_inode(dir_sector, &dir);

	return OK;
}

static int rename_entry_in_dir(uint32_t dir_sector,
			       const char *oldname,
			       const char *newname)
{
	uint32_t entry_sector;
	uint32_t entry_slot;
	uint8_t buffer[SECTOR_SIZE];
	dir_entry_t entry;

	if (validate_name(newname) != OK ||
	    find_entry_in_dir(dir_sector, newname, NULL, NULL, NULL) == OK ||
	    find_entry_in_dir(dir_sector, oldname, NULL, &entry_sector, &entry_slot) != OK)
		return FAIL;

	hdd_read(entry_sector, buffer);
	read_dir_entry(buffer, entry_slot, &entry);
	memset(entry.name, 0, sizeof(entry.name));
	memcpy(entry.name, newname, strlen(newname));
	write_dir_entry(buffer, entry_slot, &entry);
	hdd_write(entry_sector, buffer);

	return OK;
}

static int is_dir_empty(uint32_t dir_sector)
{
	inode_t dir;
	uint32_t sector;

	if (read_inode(dir_sector, &dir) != OK || dir.type != STAT_TYPE_DIR)
		return 0;

	for (sector = dir.first_data; sector != NO_SECTOR; sector = get_next_sector(sector)) {
		uint8_t buffer[SECTOR_SIZE];
		size_t i;

		hdd_read(sector, buffer);
		for (i = 0; i < DIR_ENTRIES_PER_SECTOR; i++) {
			dir_entry_t entry;

			read_dir_entry(buffer, i, &entry);
			if (dir_entry_valid(&entry))
				return 0;
		}
	}

	return 1;
}

static int read_inode_bytes(const inode_t *inode,
			    uint32_t offset,
			    uint8_t *bytes,
			    uint32_t size)
{
	uint32_t done = 0;

	if (inode == NULL || bytes == NULL)
		return FAIL;

	while (done < size) {
		uint32_t pos = offset + done;
		uint32_t sector = get_data_sector_for_offset(inode, pos);
		uint8_t buffer[SECTOR_SIZE];
		uint32_t in_sector;
		uint32_t chunk;

		if (sector == NO_SECTOR)
			return FAIL;

		hdd_read(sector, buffer);
		in_sector = pos % DATA_BYTES;
		chunk = DATA_BYTES - in_sector;
		if (chunk > size - done)
			chunk = size - done;

		memcpy(bytes + done, buffer + in_sector, chunk);
		done += chunk;
	}

	return OK;
}

static int write_inode_bytes(uint32_t inode_sector,
			     inode_t *inode,
			     const uint8_t *bytes,
			     uint32_t size)
{
	uint32_t done = 0;

	if (inode == NULL || (bytes == NULL && size != 0))
		return FAIL;

	while (done < size) {
		uint32_t sector = ensure_data_sector_for_offset(inode_sector, inode, done);
		uint8_t buffer[SECTOR_SIZE];
		uint32_t in_sector = done % DATA_BYTES;
		uint32_t chunk = DATA_BYTES - in_sector;

		if (sector == NO_SECTOR)
			return FAIL;

		if (chunk > size - done)
			chunk = size - done;

		hdd_read(sector, buffer);
		memcpy(buffer + in_sector, bytes + done, chunk);
		hdd_write(sector, buffer);
		done += chunk;
	}

	inode->size = size;
	write_inode(inode_sector, inode);
	return OK;
}

static int read_symlink_target(uint32_t inode_sector, char target[MAX_PATH + 1])
{
	inode_t inode;

	if (target == NULL || read_inode(inode_sector, &inode) != OK ||
	    inode.type != STAT_TYPE_SYMLINK || inode.size > MAX_PATH)
		return FAIL;

	memset(target, 0, MAX_PATH + 1);
	if (inode.size == 0)
		return FAIL;

	if (read_inode_bytes(&inode, 0, (uint8_t *)target, inode.size) != OK)
		return FAIL;

	target[inode.size] = '\0';
	return validate_path(target);
}

static int resolve_path_internal(const char *path,
				 int follow_final,
				 int depth,
				 uint32_t *inode_sector)
{
	superblock_t sb;
	size_t len;
	size_t i;
	uint32_t current;

	if (inode_sector == NULL || validate_path(path) != OK ||
	    read_superblock(&sb) != OK || depth > SYMLINK_DEPTH_LIMIT)
		return FAIL;

	if (strcmp(path, "/") == 0) {
		*inode_sector = sb.root_inode;
		return OK;
	}

	len = strlen(path);
	current = sb.root_inode;
	i = 1;

	while (i < len) {
		size_t j = i;
		size_t component_len;
		char component[MAX_FILENAME + 1];
		uint32_t child_sector;
		inode_t current_inode;
		inode_t child_inode;
		int last;

		while (j < len && path[j] != PATHSEP)
			j++;

		component_len = j - i;
		if (component_len == 0 || component_len > MAX_FILENAME)
			return FAIL;

		memcpy(component, path + i, component_len);
		component[component_len] = '\0';

		if (read_inode(current, &current_inode) != OK ||
		    current_inode.type != STAT_TYPE_DIR ||
		    find_entry_in_dir(current, component, &child_sector, NULL, NULL) != OK ||
		    read_inode(child_sector, &child_inode) != OK)
			return FAIL;

		last = (j == len);
		if (child_inode.type == STAT_TYPE_SYMLINK && (follow_final || !last)) {
			char target[MAX_PATH + 1];
			char newpath[MAX_PATH + 1];
			const char *remaining = path + j;
			size_t target_len;
			size_t remaining_len;

			if (read_symlink_target(child_sector, target) != OK)
				return FAIL;

			target_len = strlen(target);
			remaining_len = strlen(remaining);
			if (remaining_len == 0) {
				strcpy(newpath, target);
			} else if (strcmp(target, "/") == 0) {
				if (remaining_len > MAX_PATH)
					return FAIL;
				strcpy(newpath, remaining);
			} else {
				if (target_len + remaining_len > MAX_PATH)
					return FAIL;
				strcpy(newpath, target);
				strcat(newpath, remaining);
			}

			return resolve_path_internal(newpath, follow_final, depth + 1,
						     inode_sector);
		}

		current = child_sector;
		i = j + 1;
	}

	*inode_sector = current;
	return OK;
}

static int resolve_path_no_follow(const char *path, uint32_t *inode_sector)
{
	return resolve_path_internal(path, 0, 0, inode_sector);
}

static int resolve_path_follow(const char *path, uint32_t *inode_sector)
{
	return resolve_path_internal(path, 1, 0, inode_sector);
}

static int remove_inode_reference(uint32_t inode_sector)
{
	inode_t inode;

	if (read_inode(inode_sector, &inode) != OK)
		return FAIL;

	if (inode.type == STAT_TYPE_DIR) {
		if (!is_dir_empty(inode_sector))
			return FAIL;
		free_data_chain(inode.first_data);
		free_sector(inode_sector);
		return OK;
	}

	if (inode.type == STAT_TYPE_FILE) {
		if (inode.nlink > 1) {
			inode.nlink--;
			write_inode(inode_sector, &inode);
		} else {
			free_data_chain(inode.first_data);
			free_sector(inode_sector);
		}
		return OK;
	}

	if (inode.type == STAT_TYPE_SYMLINK) {
		free_data_chain(inode.first_data);
		free_sector(inode_sector);
		return OK;
	}

	return FAIL;
}

void fs_format()
{
	superblock_t sb;
	inode_t root;
	uint32_t total_sectors = (uint32_t)(hdd_size() / SECTOR_SIZE);
	uint32_t i;

	memset(&sb, 0, sizeof(sb));
	sb.magic = FS_MAGIC;
	sb.total_sectors = total_sectors;
	sb.free_head = total_sectors > 2 ? 2 : NO_SECTOR;
	sb.root_inode = ROOT_INODE_SECTOR;

	write_superblock(&sb);

	memset(&root, 0, sizeof(root));
	root.type = STAT_TYPE_DIR;
	root.size = 0;
	root.nlink = 1;
	root.first_data = NO_SECTOR;
	write_inode(ROOT_INODE_SECTOR, &root);

	for (i = 2; i < total_sectors; i++) {
		uint8_t buffer[SECTOR_SIZE] = { 0 };
		uint32_t next = (i + 1 < total_sectors) ? i + 1 : NO_SECTOR;

		store_u32(buffer, next);
		hdd_write(i, buffer);
	}
}

file_t *fs_creat(const char *path)
{
	char parent_path[MAX_PATH + 1];
	char name[MAX_FILENAME + 1];
	uint32_t parent_sector;
	uint32_t inode_sector;
	inode_t inode;
	file_t *fd;

	if (split_parent_child(path, parent_path, name) != OK ||
	    resolve_path_follow(parent_path, &parent_sector) != OK ||
	    read_inode(parent_sector, &inode) != OK ||
	    inode.type != STAT_TYPE_DIR)
		return NULL;

	if (find_entry_in_dir(parent_sector, name, &inode_sector, NULL, NULL) == OK) {
		if (read_inode(inode_sector, &inode) != OK ||
		    inode.type != STAT_TYPE_FILE)
			return NULL;

		free_data_chain(inode.first_data);
		inode.size = 0;
		inode.first_data = NO_SECTOR;
		write_inode(inode_sector, &inode);
	} else {
		inode_sector = alloc_sector();
		if (inode_sector == NO_SECTOR)
			return NULL;

		memset(&inode, 0, sizeof(inode));
		inode.type = STAT_TYPE_FILE;
		inode.size = 0;
		inode.nlink = 1;
		inode.first_data = NO_SECTOR;
		write_inode(inode_sector, &inode);

		if (add_entry_to_dir(parent_sector, name, inode_sector) != OK) {
			free_sector(inode_sector);
			return NULL;
		}
	}

	fd = fd_alloc();
	fd->info[FD_OFFSET] = 0;
	fd->info[FD_INODE] = inode_sector;
	fd->info[FD_TYPE] = STAT_TYPE_FILE;
	fd->info[FD_RESERVED] = 0;
	return fd;
}

file_t *fs_open(const char *path)
{
	uint32_t inode_sector;
	inode_t inode;
	file_t *fd;

	if (resolve_path_follow(path, &inode_sector) != OK ||
	    read_inode(inode_sector, &inode) != OK ||
	    inode.type != STAT_TYPE_FILE)
		return NULL;

	fd = fd_alloc();
	fd->info[FD_OFFSET] = 0;
	fd->info[FD_INODE] = inode_sector;
	fd->info[FD_TYPE] = STAT_TYPE_FILE;
	fd->info[FD_RESERVED] = 0;
	return fd;
}

int fs_close(file_t *fd)
{
	if (fd == NULL)
		return FAIL;

	fd_free(fd);
	return OK;
}

int fs_unlink(const char *path)
{
	char parent_path[MAX_PATH + 1];
	char name[MAX_FILENAME + 1];
	uint32_t parent_sector;
	uint32_t inode_sector;
	inode_t inode;

	if (split_parent_child(path, parent_path, name) != OK ||
	    resolve_path_follow(parent_path, &parent_sector) != OK ||
	    find_entry_in_dir(parent_sector, name, &inode_sector, NULL, NULL) != OK ||
	    read_inode(inode_sector, &inode) != OK ||
	    inode.type == STAT_TYPE_DIR)
		return FAIL;

	if (remove_entry_from_dir(parent_sector, name) != OK)
		return FAIL;

	return remove_inode_reference(inode_sector);
}

int fs_rename(const char *oldpath, const char *newpath)
{
	char old_parent_path[MAX_PATH + 1];
	char old_name[MAX_FILENAME + 1];
	char new_parent_path[MAX_PATH + 1];
	char new_name[MAX_FILENAME + 1];
	uint32_t old_parent_sector;
	uint32_t new_parent_sector;
	uint32_t old_inode_sector;
	uint32_t target_inode_sector;
	inode_t old_inode;
	inode_t target_inode;
	int target_exists;

	if (validate_path(oldpath) != OK || validate_path(newpath) != OK ||
	    strcmp(oldpath, "/") == 0 || strcmp(newpath, "/") == 0)
		return FAIL;

	if (strcmp(oldpath, newpath) == 0)
		return resolve_path_no_follow(oldpath, &old_inode_sector);

	if (split_parent_child(oldpath, old_parent_path, old_name) != OK ||
	    split_parent_child(newpath, new_parent_path, new_name) != OK ||
	    resolve_path_follow(old_parent_path, &old_parent_sector) != OK ||
	    resolve_path_follow(new_parent_path, &new_parent_sector) != OK ||
	    find_entry_in_dir(old_parent_sector, old_name, &old_inode_sector, NULL, NULL) != OK ||
	    read_inode(old_inode_sector, &old_inode) != OK)
		return FAIL;

	if (old_inode.type == STAT_TYPE_DIR) {
		size_t old_len = strlen(oldpath);

		if (strncmp(newpath, oldpath, old_len) == 0 &&
		    newpath[old_len] == PATHSEP)
			return FAIL;
	}

	target_exists = find_entry_in_dir(new_parent_sector, new_name,
					  &target_inode_sector, NULL, NULL) == OK;

	if (!target_exists && old_parent_sector == new_parent_sector)
		return rename_entry_in_dir(old_parent_sector, old_name, new_name);

	if (!target_exists && !dir_has_add_space(new_parent_sector))
		return FAIL;

	if (target_exists) {
		if (target_inode_sector == old_inode_sector) {
			if (remove_entry_from_dir(old_parent_sector, old_name) != OK)
				return FAIL;
			return remove_inode_reference(old_inode_sector);
		}

		if (read_inode(target_inode_sector, &target_inode) != OK)
			return FAIL;

		if ((old_inode.type == STAT_TYPE_DIR) !=
		    (target_inode.type == STAT_TYPE_DIR))
			return FAIL;

		if (target_inode.type == STAT_TYPE_DIR && !is_dir_empty(target_inode_sector))
			return FAIL;

		if (remove_entry_from_dir(new_parent_sector, new_name) != OK ||
		    remove_inode_reference(target_inode_sector) != OK)
			return FAIL;
	}

	if (add_entry_to_dir(new_parent_sector, new_name, old_inode_sector) != OK)
		return FAIL;

	if (remove_entry_from_dir(old_parent_sector, old_name) != OK)
		return FAIL;

	return OK;
}

int fs_read(file_t *fd, uint8_t *bytes, size_t size)
{
	inode_t inode;
	uint32_t offset;
	uint32_t to_read;
	uint32_t done = 0;

	if (fd == NULL || bytes == NULL || fd->info[FD_TYPE] != STAT_TYPE_FILE ||
	    read_inode(fd->info[FD_INODE], &inode) != OK ||
	    inode.type != STAT_TYPE_FILE)
		return FAIL;

	offset = fd->info[FD_OFFSET];
	if (offset >= inode.size)
		return 0;

	to_read = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
	if (to_read > inode.size - offset)
		to_read = inode.size - offset;

	while (done < to_read) {
		uint32_t pos = offset + done;
		uint32_t sector = get_data_sector_for_offset(&inode, pos);
		uint8_t buffer[SECTOR_SIZE];
		uint32_t in_sector;
		uint32_t chunk;

		if (sector == NO_SECTOR)
			break;

		hdd_read(sector, buffer);
		in_sector = pos % DATA_BYTES;
		chunk = DATA_BYTES - in_sector;
		if (chunk > to_read - done)
			chunk = to_read - done;

		memcpy(bytes + done, buffer + in_sector, chunk);
		done += chunk;
	}

	fd->info[FD_OFFSET] += done;
	return (int)done;
}

int fs_write(file_t *fd, const uint8_t *bytes, size_t size)
{
	inode_t inode;
	uint32_t done = 0;
	uint32_t offset;
	uint32_t limit;

	if (fd == NULL || (bytes == NULL && size != 0) ||
	    fd->info[FD_TYPE] != STAT_TYPE_FILE ||
	    read_inode(fd->info[FD_INODE], &inode) != OK ||
	    inode.type != STAT_TYPE_FILE)
		return FAIL;

	offset = fd->info[FD_OFFSET];
	if (offset > inode.size || offset >= MAX_FILE_SIZE)
		return FAIL;

	limit = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
	if (limit > MAX_FILE_SIZE - offset)
		limit = MAX_FILE_SIZE - offset;

	while (done < limit) {
		uint32_t pos = offset + done;
		uint32_t sector = ensure_data_sector_for_offset(fd->info[FD_INODE],
								&inode, pos);
		uint8_t buffer[SECTOR_SIZE];
		uint32_t in_sector;
		uint32_t chunk;
		uint32_t new_size;

		if (sector == NO_SECTOR)
			break;

		hdd_read(sector, buffer);
		in_sector = pos % DATA_BYTES;
		chunk = DATA_BYTES - in_sector;
		if (chunk > limit - done)
			chunk = limit - done;

		memcpy(buffer + in_sector, bytes + done, chunk);
		hdd_write(sector, buffer);

		done += chunk;
		new_size = offset + done;
		if (new_size > inode.size) {
			inode.size = new_size;
			write_inode(fd->info[FD_INODE], &inode);
		}
	}

	fd->info[FD_OFFSET] += done;
	return (int)done;
}

int fs_seek(file_t *fd, size_t pos)
{
	inode_t inode;

	if (fd == NULL || fd->info[FD_TYPE] != STAT_TYPE_FILE ||
	    read_inode(fd->info[FD_INODE], &inode) != OK ||
	    inode.type != STAT_TYPE_FILE || pos > inode.size)
		return FAIL;

	fd->info[FD_OFFSET] = (uint32_t)pos;
	return OK;
}

size_t fs_tell(file_t *fd)
{
	if (fd == NULL)
		return 0;

	return fd->info[FD_OFFSET];
}

int fs_stat(const char *path, struct fs_stat *fs_stat)
{
	uint32_t inode_sector;
	inode_t inode;

	if (fs_stat == NULL ||
	    resolve_path_no_follow(path, &inode_sector) != OK ||
	    read_inode(inode_sector, &inode) != OK)
		return FAIL;

	fs_stat->st_size = inode.size;
	fs_stat->st_nlink = inode.nlink;
	fs_stat->st_type = inode.type;
	return OK;
}

int fs_mkdir(const char *path)
{
	char parent_path[MAX_PATH + 1];
	char name[MAX_FILENAME + 1];
	uint32_t parent_sector;
	uint32_t existing_sector;
	uint32_t inode_sector;
	inode_t inode;

	if (split_parent_child(path, parent_path, name) != OK ||
	    resolve_path_follow(parent_path, &parent_sector) != OK ||
	    find_entry_in_dir(parent_sector, name, &existing_sector, NULL, NULL) == OK)
		return FAIL;

	inode_sector = alloc_sector();
	if (inode_sector == NO_SECTOR)
		return FAIL;

	memset(&inode, 0, sizeof(inode));
	inode.type = STAT_TYPE_DIR;
	inode.size = 0;
	inode.nlink = 1;
	inode.first_data = NO_SECTOR;
	write_inode(inode_sector, &inode);

	if (add_entry_to_dir(parent_sector, name, inode_sector) != OK) {
		free_sector(inode_sector);
		return FAIL;
	}

	return OK;
}

int fs_rmdir(const char *path)
{
	char parent_path[MAX_PATH + 1];
	char name[MAX_FILENAME + 1];
	uint32_t parent_sector;
	uint32_t inode_sector;
	inode_t inode;

	if (path == NULL || strcmp(path, "/") == 0 ||
	    split_parent_child(path, parent_path, name) != OK ||
	    resolve_path_follow(parent_path, &parent_sector) != OK ||
	    find_entry_in_dir(parent_sector, name, &inode_sector, NULL, NULL) != OK ||
	    read_inode(inode_sector, &inode) != OK ||
	    inode.type != STAT_TYPE_DIR ||
	    !is_dir_empty(inode_sector))
		return FAIL;

	if (remove_entry_from_dir(parent_sector, name) != OK)
		return FAIL;

	free_data_chain(inode.first_data);
	free_sector(inode_sector);
	return OK;
}

file_t *fs_opendir(const char *path)
{
	uint32_t inode_sector;
	inode_t inode;
	file_t *fd;

	if (resolve_path_follow(path, &inode_sector) != OK ||
	    read_inode(inode_sector, &inode) != OK ||
	    inode.type != STAT_TYPE_DIR)
		return NULL;

	fd = fd_alloc();
	fd->info[FD_OFFSET] = 0;
	fd->info[FD_INODE] = inode_sector;
	fd->info[FD_TYPE] = STAT_TYPE_DIR;
	fd->info[FD_RESERVED] = 0;
	return fd;
}

int fs_readdir(file_t *dir, char *item)
{
	inode_t inode;
	uint32_t sector;
	uint32_t index = 0;
	uint32_t wanted;

	if (dir == NULL || item == NULL || dir->info[FD_TYPE] != STAT_TYPE_DIR ||
	    read_inode(dir->info[FD_INODE], &inode) != OK ||
	    inode.type != STAT_TYPE_DIR)
		return FAIL;

	wanted = dir->info[FD_OFFSET];
	for (sector = inode.first_data; sector != NO_SECTOR; sector = get_next_sector(sector)) {
		uint8_t buffer[SECTOR_SIZE];
		size_t i;

		hdd_read(sector, buffer);
		for (i = 0; i < DIR_ENTRIES_PER_SECTOR; i++, index++) {
			dir_entry_t entry;

			if (index < wanted)
				continue;

			read_dir_entry(buffer, i, &entry);
			if (dir_entry_valid(&entry)) {
				memcpy(item, entry.name, MAX_FILENAME);
				item[MAX_FILENAME] = '\0';
				dir->info[FD_OFFSET] = index + 1;
				return OK;
			}
		}
	}

	dir->info[FD_OFFSET] = index;
	return FAIL;
}

int fs_closedir(file_t *dir)
{
	return fs_close(dir);
}

int fs_link(const char *path, const char *linkpath)
{
	char parent_path[MAX_PATH + 1];
	char name[MAX_FILENAME + 1];
	uint32_t inode_sector;
	uint32_t parent_sector;
	uint32_t existing_sector;
	inode_t inode;

	if (resolve_path_no_follow(path, &inode_sector) != OK ||
	    read_inode(inode_sector, &inode) != OK ||
	    inode.type != STAT_TYPE_FILE ||
	    split_parent_child(linkpath, parent_path, name) != OK ||
	    resolve_path_follow(parent_path, &parent_sector) != OK ||
	    find_entry_in_dir(parent_sector, name, &existing_sector, NULL, NULL) == OK)
		return FAIL;

	if (add_entry_to_dir(parent_sector, name, inode_sector) != OK)
		return FAIL;

	inode.nlink++;
	write_inode(inode_sector, &inode);
	return OK;
}

int fs_symlink(const char *path, const char *linkpath)
{
	char parent_path[MAX_PATH + 1];
	char name[MAX_FILENAME + 1];
	uint32_t parent_sector;
	uint32_t existing_sector;
	uint32_t inode_sector;
	inode_t inode;
	uint32_t size;

	if (validate_path(path) != OK ||
	    split_parent_child(linkpath, parent_path, name) != OK ||
	    resolve_path_follow(parent_path, &parent_sector) != OK ||
	    find_entry_in_dir(parent_sector, name, &existing_sector, NULL, NULL) == OK)
		return FAIL;

	size = (uint32_t)strlen(path);
	inode_sector = alloc_sector();
	if (inode_sector == NO_SECTOR)
		return FAIL;

	memset(&inode, 0, sizeof(inode));
	inode.type = STAT_TYPE_SYMLINK;
	inode.size = 0;
	inode.nlink = 1;
	inode.first_data = NO_SECTOR;
	write_inode(inode_sector, &inode);

	if (write_inode_bytes(inode_sector, &inode, (const uint8_t *)path, size) != OK ||
	    add_entry_to_dir(parent_sector, name, inode_sector) != OK) {
		free_data_chain(inode.first_data);
		free_sector(inode_sector);
		return FAIL;
	}

	return OK;
}
