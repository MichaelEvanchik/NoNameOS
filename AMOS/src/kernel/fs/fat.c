/*
 *     AAA    M M    OOO    SSSS
 *    A   A  M M M  O   O  S 
 *    AAAAA  M M M  O   O   SSS
 *    A   A  M   M  O   O      S
 *    A   A  M   M   OOO   SSSS 
 *
 *    Author:  Stephen Fewer
 *    Contact: steve [AT] harmonysecurity [DOT] com
 *    Web:     http://amos.harmonysecurity.com/
 *    License: GNU General Public License (GPL)
 */

#include <kernel/fs/fat.h>
#include <kernel/fs/vfs.h>
#include <kernel/io/io.h>
#include <kernel/mm/mm.h>
#include <kernel/kernel.h>
#include <lib/string.h>

void fat_setFATCluster( struct FAT_MOUNTPOINT * mount, int cluster, int value, int commit )
{
	switch( mount->type )
	{
		case FAT_12:
			mount->fat_data[ (cluster * 3) / 2 ] = value;
			break;
		case FAT_16:
			mount->fat_data[ cluster ] = value;
			break;	
		case FAT_32:
			mount->fat_data[ cluster ] = value;
			break;
		default:
			return;
	}
	
	if( commit )
	{
		vfs_seek( mount->device, sizeof(struct FAT_BOOTSECTOR)+1, VFS_SEEK_START );	
		vfs_write( mount->device, (void *)mount->fat_data, mount->fat_size );
	}
}

int fat_getFATCluster( struct FAT_MOUNTPOINT * mount, int cluster )
{
	int next_cluster;

	switch( mount->type )
	{
		case FAT_12:

			next_cluster = *(WORD *)((BYTE *)&mount->fat_data[ ( (cluster * 3) / 2 ) ]);

			// if cluster is odd
			if( cluster & 1 )
				next_cluster >>= 4;
			else
				next_cluster = FAT_CLUSTER12( next_cluster );
		
			if( next_cluster == FAT_12_ENDOFCLUSTER )
				return FAIL;
			
			break;
			
		case FAT_16:
			next_cluster = ((WORD *)mount->fat_data)[ cluster ];
			if( next_cluster == FAT_16_ENDOFCLUSTER )
				return FAIL;
			break;	
		case FAT_32:
			next_cluster = ((DWORD *)mount->fat_data)[ cluster ];
			if( next_cluster == FAT_32_ENDOFCLUSTER )
				return FAIL;
			break;
		default:
			return FAIL;
	}

	return (int)next_cluster;	
}

int fat_determineType( struct FAT_MOUNTPOINT * mount )
{
	int root_dir_sectors;
	int total_sectors;
	int fats;
	int data_sectors;
	int cluster_count;
	
	root_dir_sectors = (((mount->bootsector.num_root_dir_ents * 32) + (mount->bootsector.bytes_per_sector - 1)) / mount->bootsector.bytes_per_sector);
	
	if( mount->bootsector.sectors_per_fat != 0 )
		fats = mount->bootsector.sectors_per_fat;
	else
		fats = mount->bootsector.bs32.BPB_FATSz32;
		
	if( mount->bootsector.total_sectors != 0 )
		total_sectors = mount->bootsector.total_sectors;
	else
		total_sectors = mount->bootsector.total_sectors_large;

	data_sectors = total_sectors - ( mount->bootsector.reserved_sectors + (mount->bootsector.num_fats * fats) + root_dir_sectors );
	
	cluster_count = data_sectors / mount->bootsector.sectors_per_cluster;

	if( cluster_count < 4085 )
		mount->type = FAT_12;
	else if( cluster_count < 65525 )
		mount->type = FAT_16;
	else
		mount->type = FAT_32;

	return mount->type;
}

int fat_cluster2block( struct FAT_MOUNTPOINT * mount, int cluster )
{
	return cluster * mount->bootsector.sectors_per_cluster
		+ mount->bootsector.hidden_sectors 
    	+ mount->bootsector.num_fats * mount->bootsector.sectors_per_fat 
    	+ mount->bootsector.num_root_dir_ents /(mount->bootsector.bytes_per_sector / sizeof (struct FAT_ENTRY))-1;
}

int fat_rwCluster( struct FAT_MOUNTPOINT * mount, int cluster, BYTE * clusterBuffer, int mode )
{
	int i, block;
	rw vfs_rw;
	// set the correct mode of operation
	if( mode == FAT_READ )
		vfs_rw = vfs_read;
	else if( mode == FAT_WRITE )
		vfs_rw = vfs_write;
	else
		return FAIL;
	// convert cluster to a logical block number
	block = fat_cluster2block( mount, cluster );
	// seek to the correct offset
	if( vfs_seek( mount->device, (block*mount->bootsector.bytes_per_sector)+1, VFS_SEEK_START ) == FAIL )
		return FAIL;
	// loop through the blocks
	for( i=0 ; i<mount->bootsector.sectors_per_cluster ; i++ )
	{
		clusterBuffer += mount->bootsector.bytes_per_sector * i;
		// perform the read or write
		if( vfs_rw( mount->device, (void *)clusterBuffer, mount->bootsector.bytes_per_sector ) == FAIL )
			return FAIL;
	}
	return SUCCESS;
}

int fat_compareName( struct FAT_ENTRY * entry, char * name )
{
	int i, x;

	for(i=0;i<strlen(name);i++)
		name[i] = toupper( name[i] );
	
	//to do: check past the end of the extension
	for( i=0 ; i<8 ; i++ )
	{
		if( entry->name[i] == 0x20 )
			break;
			
		if( name[i] != entry->name[i] )
			return FAIL;
	}

	if( name[i] == '.' || entry->extention[0] != 0x20 )
	{
		i++;
		for( x=0 ; x<3 ; x++ )
		{
			if( entry->extention[x] == 0x20 )
				break;
			
			if( name[i+x] != entry->extention[x] )
				return FAIL;
		}
	}
	return SUCCESS;
}

int fat_getIndex( struct FAT_ENTRY * dir, char * name )
{
	int i;
	for( i=0 ; i<32 ; i++ )
	{
		if( dir[i].name[0] == 0x00 )
			break;
		
		if( dir[i].name[0] == FAT_ENTRY_DELETED )
			continue;
		
		if( dir[i].start_cluster == 0x0000 && !dir[i].attribute.archive )		
			continue;
		
		if( fat_compareName( &dir[i], name ) == SUCCESS )
			return i;
	}
	return FAIL;
}

int fat_file2entry( struct FAT_MOUNTPOINT * mount, char * filename, struct FAT_ENTRY * entry )
{
	int i, index = -1, length;
	char * curr_name;
	struct FAT_ENTRY * curr_dir, prevEntry;
	BYTE * clusterBuffer;
	// advance past the fisrt forward slash
	if( filename[0] == '/' )
		filename++;
	// get the total length of the filename string
	length = strlen( filename );
	// convert to uppercase
	for(i=0;i<length;i++)
		filename[i] = toupper( filename[i] );
	// allocate a buffer of memory the same size as a cluster
	clusterBuffer = (BYTE *)mm_kmalloc( mount->cluster_size );
	// point the curr_name pointer to the filename
	curr_name = filename;
	// point the curr_dir the the root directory where we begin the search
	curr_dir = mount->rootdir;
	// loop through the filename to find the file/directory
	// we search by decomposing the filename into its component parts of
	// directorys and optional ending file
	for( i=0 ; i<=length ; i++ )
	{
		if( filename[i] == '/' || filename[i] == '\0' )
		{
			// set the forward slash to a null charachter
			filename[i] = '\0';
			// get the index in the entry to the next part of the file name
			if( (index = fat_getIndex( curr_dir, curr_name )) == FAIL )
			{
				// break out of the loop
				break;
			}
			else 
			{
				// copy the current entry into the previous entry buffer
				memcpy( &prevEntry, (struct FAT_ENTRY *)&curr_dir[index], sizeof(struct FAT_ENTRY) );
				// load the next cluster to check
				if( fat_rwCluster( mount, curr_dir[index].start_cluster, clusterBuffer, FAT_READ ) == FAIL )
				{
					// free our cluster buffer
					mm_kfree( clusterBuffer );
					// return fail
					return FAIL;
				}
				// associate the current directry with the newly loaded cluster
				curr_dir = (struct FAT_ENTRY *)clusterBuffer;
			}
			// advance to the next part of the file name
			curr_name = &filename[i]+1;		
		}	
	}
	// test if we didnt find the file/directory
	if( index == FAIL && strlen( curr_name ) != 0 )
	{
		// free our cluster buffer
		mm_kfree( clusterBuffer );
		// return fail
		return FAIL;
	}
	// copy the file entry into the entry truct to return to the caller
	if( entry != NULL )
		memcpy( entry, &prevEntry, sizeof(struct FAT_ENTRY) );
	// free our cluster buffer
	mm_kfree( clusterBuffer );
	// return success
	return SUCCESS;
}

int fat_setName( struct FAT_ENTRY * entry, char * name )
{
	int i, x;
	
	for(i=0;i<strlen(name);i++)
		name[i] = toupper( name[i] );
		
	// clear the name and extension (first 11 bytes of the structure)
	memset( &entry->name[0], 0x20, 11 );
	
	for( i=0 ; i<8 ; i++ )
	{
		if( name[i] == 0x00 || name[i] == '.' )
			break;
		entry->name[i] = name[i];
	}

	if( name[i++] == '.' )
	{
		for( x=0 ; x<3 ; x++, i++ )
		{
			if( name[i] == 0x00 )
				break;
			entry->extention[x] = name[i];
		}
	}

	return SUCCESS;	
}

int fat_findEntry( struct FAT_MOUNTPOINT * mount, char * src_filename, char * dest_filename, processEntry foo )
{
	struct FAT_ENTRY * dir;
	char * dirname, * tmp;
	int dirIndex, ret=FAIL;
	int dir_cluster;
	// decompose the full filename into its directory path and file name components
	tmp = src_filename;
	dirname = strrchr( src_filename, '/' );
	if( dirname == NULL )
		return FAIL;
	src_filename = dirname + 1;
	*dirname = 0x00;
	dirname = tmp;
	// get to the destination filename
	if( dest_filename != NULL )
	{
		dest_filename = strrchr( dest_filename, '/' );
		if( dest_filename == NULL )
			return FAIL;
		dest_filename += 1;
	}
	// allcoate the directory structure
	dir = (struct FAT_ENTRY *)mm_kmalloc( mount->cluster_size );
	// try to read in the entry that points to this directory
	if( fat_file2entry( mount, dirname, dir ) < 0 )
	{
		mm_kfree( dir );
		return FAIL;
	}
	// save the directory cluster so we can write it back later
	dir_cluster = dir->start_cluster;
	// read in the directory data
	fat_rwCluster( mount, dir_cluster, (BYTE *)dir, FAT_READ );
	// loop through it
	for( dirIndex=0 ; dirIndex<(mount->cluster_size/sizeof(struct FAT_ENTRY)) ; dirIndex++ )
	{
		// skip it if it has negative size
		if( dir[dirIndex].file_size == -1 )
			continue;
		// process it
		ret = foo( mount, dir, dirIndex, src_filename, dest_filename );
		if( ret == FAT_PROCESS_BREAK ) {
			break;
		} else if( ret == FAT_PROCESS_SUCCESS ) {
			ret = SUCCESS;
			break;
		}
	}
	// only try to write it back if we have a success
	if( ret == SUCCESS )
	{
		// write back the directory to disk
		if( fat_rwCluster( mount, dir_cluster, (BYTE *)dir, FAT_WRITE ) != SUCCESS )
			ret = FAIL;
	}
	// free the directory data structure
	mm_kfree( dir );
	// return the success of the operation
	return ret;	
}

int fat_processCreate( struct FAT_MOUNTPOINT * mount, struct FAT_ENTRY * entry, int index, char * src_filename, char * dest_filename )
{
	struct FAT_DOSTIME time;
	struct FAT_DOSDATE date;
	// test if their are any more entries
	if( entry[index].name[0] == 0x00 || entry[index].name[0] == FAT_ENTRY_DELETED )
	{
		// if this is the end of the entries, advance the end by one
		if( entry[index].name[0] == 0x00 )
		{
			// make sure we dont go to far
			if( index+1>=(mount->cluster_size/sizeof(struct FAT_ENTRY)) )
				return FAT_PROCESS_BREAK;
			entry[index+1].name[0] = 0x00;
		}
		// clear it
		memset( &entry[index], 0x00, sizeof(struct FAT_ENTRY) );
		// set a default attribte
		entry[index].attribute.archive = 1;
		// set default time
		time.hours = 0;
		time.minutes = 0;
		time.twosecs = 0;
		memcpy( &entry[index].time, &time, sizeof(struct FAT_DOSTIME) );
		// set default date
		date.date = 0;
		date.month = 0;
		date.year = 0;
		memcpy( &entry[index].date, &date, sizeof(struct FAT_DOSTIME) );
		// set the file name and extension	
		fat_setName( &entry[index], src_filename );
		// set its initial size to zero
		entry[index].file_size = 0;
		// we have no start cluster yet
		entry[index].start_cluster = FAT_FREECLUSTER;
		// return success
		return FAT_PROCESS_SUCCESS;
	}
	// continue to try
	return FAT_PROCESS_CONTINUE;
}

int fat_processDelete( struct FAT_MOUNTPOINT * mount, struct FAT_ENTRY * entry, int index, char * src_filename, char * dest_filename )
{
	// test if their are any more entries
	if( entry[index].name[0] == 0x00 )
		return FAT_PROCESS_BREAK;
	// if the file is deleated, continue past it
	if( entry[index].name[0] == FAT_ENTRY_DELETED )
		return FAT_PROCESS_CONTINUE;
	// try and get a match
	if( fat_compareName( &entry[index], src_filename ) )
	{
// TO-DO: traverse the cluster chain and mark them all free
		//int cluster = entry[index].start_cluster;
		// set the first cluster to be free
/*
 * we need to do this backwards
		fat_setFATCluster( mount, cluster, FAT_FREECLUSTER, FALSE );
		while( cluster != FAT_FREECLUSTER )
		{
			// get the next cluster
			cluster = fat_getFATCluster( mount, cluster );
			if( cluster == FAT_FREECLUSTER || cluster == -1 )
				break;
			// set the next cluster to be free
			fat_setFATCluster( mount, cluster, FAT_FREECLUSTER, FALSE );
		}
		// commit the FAT to disk
		fat_setFATCluster( mount, cluster, FAT_FREECLUSTER, TRUE );
*/
		// mark it as deleated
		entry[index].name[0] = FAT_ENTRY_DELETED;
		// return success
		return FAT_PROCESS_SUCCESS;
	}
	// continue to try
	return FAT_PROCESS_CONTINUE;
}

int fat_processRename( struct FAT_MOUNTPOINT * mount, struct FAT_ENTRY * entry, int index, char * src_filename, char * dest_filename )
{
	// test if their are any more entries
	if( entry[index].name[0] == 0x00 )
		return FAT_PROCESS_BREAK;
	// if the file is deleated, continue past it
	if( entry[index].name[0] == FAT_ENTRY_DELETED )
		return FAT_PROCESS_CONTINUE;
	// try and get a match
	if( fat_compareName( &entry[index], src_filename ) )
	{
		// rename the entry
		fat_setName( &entry[index], dest_filename );
		// return success
		return FAT_PROCESS_SUCCESS;
	}
	// continue to try
	return FAT_PROCESS_CONTINUE;
}

int fat_processCopy( struct FAT_MOUNTPOINT * mount, struct FAT_ENTRY * entry, int index, char * src_filename, char * dest_filename )
{
	return FAT_PROCESS_BREAK;
}

void * fat_mount( char * device, char * mountpoint, int fstype )
{
	int root_dir_offset;
	struct FAT_MOUNTPOINT * mount = (struct FAT_MOUNTPOINT *)mm_kmalloc( sizeof(struct FAT_MOUNTPOINT) );
	if( mount == NULL )
		return NULL;
	//open the device we wish to mount
	mount->device = vfs_open( device, VFS_MODE_READWRITE );
	if( mount->device == NULL )
	{
		mm_kfree( mount );
		return NULL;
	}
	// read in the bootsector
	vfs_read( mount->device, (void *)&mount->bootsector, sizeof(struct FAT_BOOTSECTOR) );
	// make sure we have a valid bootsector
	if( mount->bootsector.magic != FAT_MAGIC )
	{
		vfs_close( mount->device );
		mm_kfree( mount );
		return NULL;
	}
	// determine if we have a FAT 12, 16 or 32 filesystem
	fat_determineType( mount );
	// calculate clster size
	mount->cluster_size = mount->bootsector.bytes_per_sector * mount->bootsector.sectors_per_cluster;
	// calculate the fat size
	mount->fat_size = mount->bootsector.sectors_per_fat * mount->bootsector.bytes_per_sector;
	// malloc some space for the fat
	mount->fat_data = (BYTE *)mm_kmalloc( mount->fat_size );
	// and clear it
	memset( mount->fat_data, 0x00, mount->fat_size );
	// read in the FAT
	vfs_read( mount->device, (void *)mount->fat_data, mount->fat_size );
	// read in root directory
	mount->rootdir = (struct FAT_ENTRY *)mm_kmalloc( mount->bootsector.num_root_dir_ents * sizeof( struct FAT_ENTRY ) );
	memset( mount->rootdir, 0x00, mount->bootsector.num_root_dir_ents * sizeof( struct FAT_ENTRY ) );
	// find and read in the root directory	
	root_dir_offset = (mount->bootsector.num_fats * mount->fat_size) + sizeof(struct FAT_BOOTSECTOR) + 1;
	vfs_seek( mount->device, root_dir_offset, VFS_SEEK_START );
	vfs_read( mount->device, (void *)(mount->rootdir), mount->bootsector.num_root_dir_ents * sizeof( struct FAT_ENTRY ) );
	// successfully return the new FAT mount
	return mount;
}

int fat_unmount( struct VFS_MOUNTPOINT * mount, char * mountpoint )
{
	struct FAT_MOUNTPOINT * fat_mount = (struct FAT_MOUNTPOINT *)mount->data_ptr;
	if( fat_mount == NULL )
		return FAIL;
	// close the device
	vfs_close( fat_mount->device );
	// free the rootdir structure
	mm_kfree( fat_mount->rootdir );
	// free the mount structure
	mm_kfree( fat_mount );
	// return
	return SUCCESS;	
}

struct VFS_HANDLE * fat_open( struct VFS_HANDLE * handle, char * filename )
{
	struct FAT_MOUNTPOINT * fat_mount;
	struct FAT_FILE * file;
	// retrieve the fat mount structure
	fat_mount = (struct FAT_MOUNTPOINT *)handle->mount->data_ptr;
	if( fat_mount == NULL )
		return NULL;
	file = (struct FAT_FILE *)mm_kmalloc( sizeof(struct FAT_FILE) );
	// try to find the file
	if( fat_file2entry( fat_mount, filename, &file->entry ) == FAIL )
	{
		// if we fail free the file entry structure
		mm_kfree( file );
		// return null
		return NULL;
	}
	// set the mountpoint this file is on
	file->mount = fat_mount;
	// set the current file position to zero
	file->current_pos = 0;
	// associate the handle with the file entry
	handle->data_ptr = file;
	// if we open the file in truncate mode we need to set the size to 0
	//if( (handle->mode & VFS_MODE_TRUNCATE) == VFS_MODE_TRUNCATE )
	// return success
	return handle;	
}

int fat_close( struct VFS_HANDLE * handle )
{
	// check we have a fat entry associated with this handle
	if( handle->data_ptr == NULL )
		return FAIL;
	// free the files fat entry
	mm_kfree( handle->data_ptr );
	// return success
	return SUCCESS;
}

int fat_clone( struct VFS_HANDLE * handle, struct VFS_HANDLE * clone )
{
	return FAIL;
}

int fat_rw( struct VFS_HANDLE * handle, BYTE * buffer, DWORD size, int mode )
{
	int bytes_to_rw=0, bytes_rw=0, cluster_offset=0;
	int cluster;
	struct FAT_FILE * file;
	BYTE * clusterBuffer;
	// retrieve the file structure
	file = (struct FAT_FILE *)handle->data_ptr;
	if( file == NULL )
		return FAIL;
	// reduce size if we are trying to read past the end of the file
	if( file->current_pos + size > file->entry.file_size )
	{
		// to-do: if we are writing we will need to expand the file size (alloc more clusters)
		size = file->entry.file_size - file->current_pos;
	}
	// initially set the cluster number to the first cluster as specified in the file entry
	cluster = file->entry.start_cluster;
	// get the correct cluster to begin reading/writing from
	int i = file->current_pos / file->mount->cluster_size;
	// we traverse the cluster chain i times
	while( i-- )
	{
		// get the next cluster in the file
		cluster = fat_getFATCluster( file->mount, cluster );
		// fail if we have gone beyond the files cluster chain
		if( cluster == FAT_FREECLUSTER || cluster == FAIL )
		{
			kernel_printf("fat_rw: cluster == 0x0000 || cluster == FAIL\n");
			return FAIL;
		}
	}
	// handle reads/writes that begin from some point inside a cluster
	cluster_offset = file->current_pos % file->mount->cluster_size;
	// allocate a buffer to read/write data into
	clusterBuffer = (BYTE *)mm_kmalloc( file->mount->cluster_size );
	// read/write the data
	while( TRUE )
	{
		// set the amount of data we want to read/write in this loop itteration
		if( size >= file->mount->cluster_size )
			bytes_to_rw = file->mount->cluster_size;
		else
			bytes_to_rw = size;
		
		// test if we are reading/writting accross 2 clusters. if we are, we can only read/write up to the end of the
		// first cluster in this itteration of the loop, the next itteration will take care of the rest
		// this solution is ugly, more then likely a much cleaner way of checking for this!
		if( (cluster_offset + bytes_to_rw) > (((file->current_pos / file->mount->cluster_size)+1)*file->mount->cluster_size) )
		{
			bytes_to_rw = (cluster_offset + bytes_to_rw) - (((file->current_pos / file->mount->cluster_size)+1)*file->mount->cluster_size);
			bytes_to_rw = size - bytes_to_rw;
			//kernel_printf("fat_rw: rw accross 2 clusters, bytes_to_read = %d\n", bytes_to_read );	
		}
		
		// setup the clusterBuffer if we are going to perform a write operation
		if( mode == FAT_WRITE )
		{
			// if we are writing from a point inside a cluster (as opposed to an entire cluster) we will need
			// to read in the origional cluster to preserve the data before the cluster_offset
			if( cluster_offset > 0 || bytes_to_rw < file->mount->cluster_size )
			{
				if( fat_rwCluster( file->mount, cluster, clusterBuffer, FAT_READ ) == FAIL )
				{
					kernel_printf("fat_rw: FAT_WRITE fat_rwCluster FAIL\n" );	
					break;
				}
			}
			else
			{
				memset( clusterBuffer, 0x00, file->mount->cluster_size );
			}
			// copy data from buffer into cluster
			memcpy( (clusterBuffer+cluster_offset), buffer, bytes_to_rw );
		}
		
		// read/write the next cluster of data
		if( fat_rwCluster( file->mount, cluster, clusterBuffer, mode ) == FAIL )
		{
			kernel_printf("fat_rw: fat_rwCluster() failed\n");
			// should we reset the files offset position if its changed?
			break;
		}
		
		// if we performed a read operation, copy the cluster data over to our buffer
		if( mode == FAT_READ )
			memcpy( buffer, (clusterBuffer+cluster_offset), bytes_to_rw );
		
		// advance the buffer pointer
		buffer += bytes_to_rw;
		// increase the bytes read/written
		bytes_rw += bytes_to_rw;
		// reduce the size
		size -= bytes_to_rw;
		// test if we have read/written enough
		if( size <= 0 )
			break;
		// get the next cluster to read/write from
		cluster = fat_getFATCluster( file->mount, cluster );
		// if the cluster = FAT_FREECLUSTER we have reached the end of the cluster chain
		if( cluster == FAT_FREECLUSTER || cluster == FAIL )
		{
			kernel_printf("fat_rw: bottom of loop, cluster == FAT_FREECLUSTER || cluster == FAIL\n");
			// if FAT_WRITE, alloc another cluster...
			break;
		}
		// we can now set the cluster offset to 0 if we are reading/writing from more then 1 cluster
		cluster_offset = 0;
	}
	// free the buffer
	mm_kfree( clusterBuffer );
	// update the files offset position
	file->current_pos += bytes_rw;
	// return the total bytes read/written
	return bytes_rw;	
}

int fat_read( struct VFS_HANDLE * handle, BYTE * buffer, DWORD size )
{
	return fat_rw( handle, buffer, size, FAT_READ );	
}

int fat_write( struct VFS_HANDLE * handle, BYTE * buffer, DWORD size )
{
	return fat_rw( handle, buffer, size, FAT_WRITE );
}

int fat_seek( struct VFS_HANDLE * handle, DWORD offset, BYTE origin )
{
	struct FAT_FILE * file;
	int saved_pos;
	// retrieve the file structure
	file = (struct FAT_FILE *)handle->data_ptr;
	if( file == NULL )
		return FAIL;
	// save the origional position in case we nee to roll back
	saved_pos = file->current_pos;
	// set the new position
	if( origin == VFS_SEEK_START )
		file->current_pos = offset;
	else if( origin == VFS_SEEK_CURRENT )
		file->current_pos += offset;
	else if( origin == VFS_SEEK_END )
		file->current_pos = file->entry.file_size - offset;
	else
		return FAIL;
	// reset if we have gone over the file size
	if( file->current_pos > file->entry.file_size || file->current_pos < 0 )
		file->current_pos = saved_pos;
	// return the current file position
	return file->current_pos;
}

int fat_control( struct VFS_HANDLE * handle, DWORD request, DWORD arg )
{
	// we dont need to support and control calls
	return FAIL;
}

int fat_create( struct VFS_MOUNTPOINT * mount, char * filename )
{
	char * name;
	// retrieve the fat mount structure
	struct FAT_MOUNTPOINT * fat_mount = (struct FAT_MOUNTPOINT *)mount->data_ptr;
	if( fat_mount == NULL )
		return FAIL;
	// create a copy of filename as it will get mangled in the fat_file2entry() call
	name = (char *)mm_kmalloc( strlen(filename)+1 );
	strcpy( name, filename );
	// try to find the file to see if it allready exists
	if( fat_file2entry( fat_mount, name, NULL ) == FAIL )
	{
		mm_kfree( name );
		return FAIL;
	}
	mm_kfree( name );
	return fat_findEntry( fat_mount, filename, NULL, fat_processCreate );
}

int fat_delete( struct VFS_MOUNTPOINT * mount, char * filename )
{
	// retrieve the fat mount structure
	struct FAT_MOUNTPOINT * fat_mount = (struct FAT_MOUNTPOINT *)mount->data_ptr;
	if( fat_mount == NULL )
		return FAIL;
	return fat_findEntry( fat_mount, filename, NULL, fat_processDelete );
}

int fat_rename( struct VFS_MOUNTPOINT * mount, char * src, char * dest )
{
	// retrieve the fat mount structure
	struct FAT_MOUNTPOINT * fat_mount = (struct FAT_MOUNTPOINT *)mount->data_ptr;
	if( fat_mount == NULL )
		return FAIL;
	return fat_findEntry( fat_mount, src, dest, fat_processRename );
}

int fat_copy( struct VFS_MOUNTPOINT * mount, char * src, char * dest )
{
	// retrieve the fat mount structure
	struct FAT_MOUNTPOINT * fat_mount = (struct FAT_MOUNTPOINT *)mount->data_ptr;
	if( fat_mount == NULL )
		return FAIL;
	return fat_findEntry( fat_mount, src, dest, fat_processCopy );
}

struct VFS_DIRLIST_ENTRY * fat_list( struct VFS_MOUNTPOINT * mount, char * dirname )
{
	int dirIndex, entryIndex, nameIndex;
	struct FAT_ENTRY * dir;
	struct VFS_DIRLIST_ENTRY * entry;
	// retrieve the fat mount structure
	struct FAT_MOUNTPOINT * fat_mount = (struct FAT_MOUNTPOINT *)mount->data_ptr;
	if( fat_mount == NULL )
		return NULL;
	if( strlen( dirname ) == 0 )
	{	
		dir = fat_mount->rootdir;
	}
	else
	{
		dir = (struct FAT_ENTRY *)mm_kmalloc( fat_mount->cluster_size );
		
		if( fat_file2entry( fat_mount, dirname, dir ) < 0 )
		{
			mm_kfree( dir );
			return NULL;
		}

		fat_rwCluster( fat_mount, dir->start_cluster, (BYTE *)dir, FAT_READ );
	}

	entry = (struct VFS_DIRLIST_ENTRY *)mm_kmalloc( sizeof(struct VFS_DIRLIST_ENTRY)*17 );
	// clear it
	memset( entry, 0x00, sizeof(struct VFS_DIRLIST_ENTRY)*17 );
	
	for(dirIndex=0,entryIndex=0;dirIndex<16;dirIndex++)
	{
		// test if their are any more entries
		if( dir[dirIndex].name[0] == 0x00 )
			break;
		// if the file is deleated, continue past it
		if( dir[dirIndex].name[0] == FAT_ENTRY_DELETED )
			continue;
		// skip it if it has negative size
		if( dir[dirIndex].file_size == -1 )
			continue;
		// fill in the name
		memset( entry[entryIndex].name, 0x00, 32 );
		for( nameIndex=0 ; nameIndex<8 ; nameIndex++ )
		{
			if( dir[dirIndex].name[nameIndex] == 0x20 )
				break;
			entry[entryIndex].name[nameIndex] = tolower( dir[dirIndex].name[nameIndex] );
		}
		// and the extension if their is one
		if( dir[dirIndex].extention[0] != 0x20 )
		{
			entry[entryIndex].name[nameIndex] = '.';
			entry[entryIndex].name[nameIndex+1] = ( dir[dirIndex].extention[0] == 0x20 ? 0x00 : tolower(dir[dirIndex].extention[0]) );
			entry[entryIndex].name[nameIndex+2] = ( dir[dirIndex].extention[1] == 0x20 ? 0x00 : tolower(dir[dirIndex].extention[1]) );
			entry[entryIndex].name[nameIndex+3] = ( dir[dirIndex].extention[2] == 0x20 ? 0x00 : tolower(dir[dirIndex].extention[2]) );
		}
		// fill in the attributes
		if( dir[dirIndex].attribute.directory )
			entry[entryIndex].attributes = VFS_DIRECTORY;
		else
			entry[entryIndex].attributes = VFS_FILE;
		// fill in the size
		entry[entryIndex].size = dir[dirIndex].file_size;

		entryIndex++;
	}
	// free
	mm_kfree( dir );
	// return to caller. caller *must* free this structure
	return entry;
}

int fat_init( void )
{
	struct VFS_FILESYSTEM * fs;
	// create the file system structure
	fs = (struct VFS_FILESYSTEM *)mm_kmalloc( sizeof(struct VFS_FILESYSTEM) );
	// set the file system type
	fs->fstype = FAT_TYPE;
	// setup the file system calltable
	fs->calltable.open    = fat_open;
	fs->calltable.close   = fat_close;
	fs->calltable.clone   = fat_clone;
	fs->calltable.read    = fat_read;
	fs->calltable.write   = fat_write;
	fs->calltable.seek    = fat_seek;
	fs->calltable.control = fat_control;
	fs->calltable.create  = fat_create;
	fs->calltable.delete  = fat_delete;
	fs->calltable.rename  = fat_rename;
	fs->calltable.copy    = fat_copy;
	fs->calltable.list    = fat_list;
	fs->calltable.mount   = fat_mount;
	fs->calltable.unmount = fat_unmount;
	// register the file system with the VFS
	return vfs_register( fs );
}
