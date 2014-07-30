#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <copyfile.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

#define IS_64_BIT(x) ((x) == MH_MAGIC_64 || (x) == MH_CIGAM_64)
#define IS_LITTLE_ENDIAN(x) ((x) == FAT_CIGAM || (x) == MH_CIGAM_64 || (x) == MH_CIGAM)
#define SWAP32(x, magic) (IS_LITTLE_ENDIAN(magic)? OSSwapInt32(x): (x))

void usage(void) {
	printf("Usage: insert_dylib [--inplace] dylib_path binary_path [new_binary_path]\n");
	
	exit(1);
}

void insert_dylib(FILE *f, size_t header_offset, const char *dylib_path) {
	fseek(f, header_offset, SEEK_SET);
	
	struct mach_header mh;
	fread(&mh, sizeof(struct mach_header), 1, f);
	
	if(IS_64_BIT(mh.magic)) {
		fseek(f, sizeof(struct mach_header_64) - sizeof(struct mach_header), SEEK_CUR);
	}
	
	size_t dylib_path_len = strlen(dylib_path);
	size_t dylib_path_size = (dylib_path_len & ~3) + 4;
	uint32_t cmdsize = (uint32_t)(sizeof(struct dylib_command) + dylib_path_size);
	
	char *dylib_path_padded = calloc(dylib_path_size, 1);
	memcpy(dylib_path_padded, dylib_path, dylib_path_len);
	
	struct dylib_command dylib_command = {
		.cmd = SWAP32(LC_LOAD_DYLIB, mh.magic),
		.cmdsize = SWAP32(cmdsize, mh.magic),
		.dylib = {
			.name = SWAP32(sizeof(struct dylib_command), mh.magic),
			.timestamp = 0,
			.current_version = 0,
			.compatibility_version = 0
		}
	};
	
	uint32_t sizeofcmds = SWAP32(mh.sizeofcmds, mh.magic);
	
	fseek(f, sizeofcmds, SEEK_CUR);
	fwrite(&dylib_command, sizeof(dylib_command), 1, f);
	fwrite(dylib_path_padded, dylib_path_size, 1, f);
	
	free(dylib_path_padded);
	
	mh.ncmds = SWAP32(SWAP32(mh.ncmds, mh.magic) + 1, mh.magic);
	sizeofcmds += cmdsize;
	mh.sizeofcmds = SWAP32(sizeofcmds, mh.magic);
	
	fseek(f, header_offset, SEEK_SET);
	fwrite(&mh, sizeof(mh), 1, f);
}

int main(int argc, const char *argv[]) {
	bool inplace = false;
	
	if(argc >= 2 && strcmp(argv[1], "--inplace") == 0) {
		inplace = true;
		
		argv = &argv[1];
		argc--;
	}
	
	if(argc < 3 || argc > 4) {
		usage();
	}
	
	const char *dylib_path = argv[1];
	const char *binary_path = argv[2];
	
	if(!inplace) {
		char *new_binary_path;
		if(argc == 4) {
			new_binary_path = (char *)argv[3];
		} else {
			asprintf(&new_binary_path, "%s_patched", binary_path);
		}
		
		if(copyfile(binary_path, new_binary_path, NULL, COPYFILE_DATA | COPYFILE_UNLINK)) {
			printf("Failed to create %s\n", new_binary_path);
			exit(1);
		}
		
		binary_path = new_binary_path;
	}
	
	FILE *f = fopen(binary_path, "r+");
	
	if(!f) {
		printf("Couldn't open file %s\n", argv[1]);
		exit(1);
	}
	
	uint32_t magic;
	fread(&magic, sizeof(uint32_t), 1, f);
	
	switch(magic) {
		case FAT_MAGIC:
		case FAT_CIGAM: {
			fseek(f, 0, SEEK_SET);
			
			struct fat_header fh;
			fread(&fh, sizeof(struct fat_header), 1, f);
			
			uint32_t nfat_arch = SWAP32(fh.nfat_arch, magic);
			
			printf("Binary is a fat binary with %d archs.\n", nfat_arch);
			
			struct fat_arch archs[nfat_arch];
			fread(&archs, sizeof(archs), 1, f);
			
			for(int i = 0; i < nfat_arch; i++) {
				insert_dylib(f, SWAP32(archs[i].offset, magic), dylib_path);
			}
			
			printf("Added CL_LOAD_DYLIB command to all archs in %s\n", binary_path);
			
			break;
		}
		case MH_MAGIC_64:
		case MH_CIGAM_64:
		case MH_MAGIC:
		case MH_CIGAM:
			insert_dylib(f, 0, dylib_path);
			printf("Added LC_LOAD_DYLIB command to %s\n", binary_path);
			break;
		default: {
			printf("Unknown magic: 0x%x\n", magic);
			exit(1);
		}
	}
	
	fclose(f);
	
    return 0;
}

