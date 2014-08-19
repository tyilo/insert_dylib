#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <copyfile.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

#define IS_64_BIT(x) ((x) == MH_MAGIC_64 || (x) == MH_CIGAM_64)
#define IS_LITTLE_ENDIAN(x) ((x) == FAT_CIGAM || (x) == MH_CIGAM_64 || (x) == MH_CIGAM)
#define SWAP32(x, magic) (IS_LITTLE_ENDIAN(magic)? OSSwapInt32(x): (x))

int inplace_flag = false;
int weak_flag = false;
int overwrite_flag = false;
int codesig_flag = 0;

static struct option long_options[] = {
	{"inplace",          no_argument, &inplace_flag,   true},
	{"weak",             no_argument, &weak_flag,      true},
	{"overwrite",        no_argument, &overwrite_flag, true},
	{"strip-codesig",    no_argument, &codesig_flag,   1},
	{"no-strip-codesig", no_argument, &codesig_flag,   2},
	{NULL,               0,           NULL,            0}
};

__attribute__((noreturn)) void usage(void) {
	printf("Usage: insert_dylib dylib_path binary_path [new_binary_path]\n");
	
	printf("Option flags:");
	
	struct option *opt = long_options;
	while(opt->name != NULL) {
		printf(" --%s", opt->name);
		opt++;
	}
	
	printf("\n");
	
	exit(1);
}

__attribute__((format(printf, 1, 2))) bool ask(const char *format, ...) {
	char *question;
	asprintf(&question, "%s [y/n] ", format);
	
	va_list args;
	va_start(args, format);
	vprintf(question, args);
	va_end(args);
	
	free(question);
	
	while(true) {
		char *line = NULL;
		size_t size;
		getline(&line, &size, stdin);
		
		switch(line[0]) {
			case 'y':
			case 'Y':
				return true;
				break;
			case 'n':
			case 'N':
				return false;
				break;
			default:
				printf("Please enter y or n: ");
		}
	}
}

bool check_load_commands(FILE *f, struct mach_header *mh, size_t header_offset, size_t commands_offset, const char *dylib_path) {
	fseek(f, commands_offset, SEEK_SET);
	
	uint32_t ncmds = SWAP32(mh->ncmds, mh->magic);
	
	for(int i = 0; i < ncmds; i++) {
		struct load_command lc;
		fread(&lc, sizeof(lc), 1, f);
		
		uint32_t cmdsize = SWAP32(lc.cmdsize, mh->magic);
		
		switch(SWAP32(lc.cmd, mh->magic)) {
			case LC_CODE_SIGNATURE:
				if(i == ncmds - 1) {
					if(codesig_flag == 2) {
						return true;
					}
					
					if(codesig_flag == 0 && !ask("LC_CODE_SIGNATURE load command found. Remove it?")) {
						return true;
					}
					
					fseek(f, -((long)sizeof(lc)), SEEK_CUR);
					
					struct linkedit_data_command ldc;
					fread(&ldc, sizeof(ldc), 1, f);
					
					uint32_t dataoff = SWAP32(ldc.dataoff, mh->magic);
					uint32_t datasize = SWAP32(ldc.datasize, mh->magic);
					
					fseek(f, -((long)sizeof(ldc)), SEEK_CUR);
					
					char *zero = calloc(cmdsize, 1);
					fwrite(zero, cmdsize, 1, f);
					free(zero);
					
					fseek(f, header_offset + dataoff, SEEK_SET);
					
					zero = calloc(datasize, 1);
					fwrite(zero, datasize, 1, f);
					free(zero);
					
					mh->ncmds = SWAP32(ncmds - 1, mh->magic);
					mh->sizeofcmds = SWAP32(SWAP32(mh->sizeofcmds, mh->magic) - cmdsize, mh->magic);
					
					return true;
				} else {
					printf("LC_CODE_SIGNATURE is not the last load command, so couldn't remove.\n");
				}
				break;
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB: {
				struct dylib_command *dylib_command = malloc(cmdsize);
				
				fseek(f, -((long)sizeof(lc)), SEEK_CUR);
				fread(dylib_command, cmdsize, 1, f);
				
				union lc_str offset = dylib_command->dylib.name;
				char *name = &((char *)dylib_command)[SWAP32(offset.offset, mh->magic)];
				
				int cmp = strcmp(name, dylib_path);
				
				free(dylib_command);
				
				if(cmp == 0) {
					if(!ask("Binary already contains a load command for that dylib. Continue anyway?")) {
						return false;
					}
				}
				
				fseek(f, -((long)cmdsize) + sizeof(lc), SEEK_CUR);
				break;
			}
		}
		
		fseek(f, SWAP32(lc.cmdsize, mh->magic) - sizeof(lc), SEEK_CUR);
	}
	
	return true;
}

bool insert_dylib(FILE *f, size_t header_offset, const char *dylib_path) {
	fseek(f, header_offset, SEEK_SET);
	
	struct mach_header mh;
	fread(&mh, sizeof(struct mach_header), 1, f);
	
	if(mh.magic != MH_MAGIC_64 && mh.magic != MH_CIGAM_64 && mh.magic != MH_MAGIC && mh.magic != MH_CIGAM) {
		printf("Unknown magic: 0x%x\n", mh.magic);
		return false;
	}
	
	size_t commands_offset = header_offset + (IS_64_BIT(mh.magic)? sizeof(struct mach_header_64): sizeof(struct mach_header));
	
	bool cont = check_load_commands(f, &mh, header_offset, commands_offset, dylib_path);
	
	if(!cont) {
		return true;
	}
	
	size_t dylib_path_len = strlen(dylib_path);
	size_t dylib_path_size = (dylib_path_len & ~3) + 4;
	uint32_t cmdsize = (uint32_t)(sizeof(struct dylib_command) + dylib_path_size);
	
	struct dylib_command dylib_command = {
		.cmd = SWAP32(weak_flag? LC_LOAD_WEAK_DYLIB: LC_LOAD_DYLIB, mh.magic),
		.cmdsize = SWAP32(cmdsize, mh.magic),
		.dylib = {
			.name = SWAP32(sizeof(struct dylib_command), mh.magic),
			.timestamp = 0,
			.current_version = 0,
			.compatibility_version = 0
		}
	};
	
	uint32_t sizeofcmds = SWAP32(mh.sizeofcmds, mh.magic);
	
	fseek(f, commands_offset + sizeofcmds, SEEK_SET);
	char space[cmdsize];
	
	fread(&space, cmdsize, 1, f);
	
	bool empty = true;
	for(int i = 0; i < cmdsize; i++) {
		if(space[i] != 0) {
			empty = false;
			break;
		}
	}
	
	if(!empty) {
		if(!ask("It doesn't seem like there is enough empty space. Continue anyway?")) {
			return false;
		}
	}
	
	fseek(f, -((long)cmdsize), SEEK_CUR);
	
	char *dylib_path_padded = calloc(dylib_path_size, 1);
	memcpy(dylib_path_padded, dylib_path, dylib_path_len);
	
	fwrite(&dylib_command, sizeof(dylib_command), 1, f);
	fwrite(dylib_path_padded, dylib_path_size, 1, f);
	
	free(dylib_path_padded);
	
	mh.ncmds = SWAP32(SWAP32(mh.ncmds, mh.magic) + 1, mh.magic);
	sizeofcmds += cmdsize;
	mh.sizeofcmds = SWAP32(sizeofcmds, mh.magic);
	
	fseek(f, header_offset, SEEK_SET);
	fwrite(&mh, sizeof(mh), 1, f);
	
	return true;
}

int main(int argc, const char *argv[]) {
	while(true) {
		int option_index = 0;
		
		int c = getopt_long(argc, (char *const *)argv, "", long_options, &option_index);
		
		if(c == -1) {
			break;
		}
		
		switch(c) {
			case 0:
				break;
			case '?':
				usage();
				break;
			default:
				abort();
				break;
		}
	}
	
	argv = &argv[optind - 1];
	argc -= optind - 1;
	
	if(argc < 3 || argc > 4) {
		usage();
	}
	
	const char *lc_name = weak_flag? "LC_LOAD_WEAK_DYLIB": "LC_LOAD_DYLIB";
	
	const char *dylib_path = argv[1];
	const char *binary_path = argv[2];
	
	struct stat s;
	
	if(stat(binary_path, &s) != 0) {
		perror(binary_path);
		exit(1);
	}
	
	if(stat(dylib_path, &s) != 0) {
		if(!ask("The provided dylib path doesn't exist. Continue anyway?")) {
			exit(1);
		}
	}
	
	bool binary_path_was_malloced = false;
	if(!inplace_flag) {
		char *new_binary_path;
		if(argc == 4) {
			new_binary_path = (char *)argv[3];
		} else {
			asprintf(&new_binary_path, "%s_patched", binary_path);
			binary_path_was_malloced = true;
		}
		
		if(!overwrite_flag && stat(new_binary_path, &s) == 0) {
			if(!ask("%s already exists. Overwrite it?", new_binary_path)) {
				exit(1);
			}
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
	
	bool success = true;
		
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
			
			int fails = 0;
			
			for(int i = 0; i < nfat_arch; i++) {
				bool r = insert_dylib(f, SWAP32(archs[i].offset, magic), dylib_path);
				if(!r) {
					printf("Failed to add %s to arch #%d!\n", lc_name, i + 1);
					fails++;
				}
			}
			
			if(fails == 0) {
				printf("Added %s to all archs in %s\n", lc_name, binary_path);
			} else if(fails == nfat_arch) {
				printf("Failed to add %s to any archs.\n", lc_name);
				success = false;
			} else {
				printf("Added %s to %d/%d archs in %s\n", lc_name, nfat_arch - fails, nfat_arch, binary_path);
			}
			
			break;
		}
		case MH_MAGIC_64:
		case MH_CIGAM_64:
		case MH_MAGIC:
		case MH_CIGAM:
			if(insert_dylib(f, 0, dylib_path)) {
				printf("Added %s to %s\n", lc_name, binary_path);
			} else {
				printf("Failed to add %s!\n", lc_name);
				success = false;
			}
			break;
		default:
			printf("Unknown magic: 0x%x\n", magic);
			exit(1);
	}
	
	fclose(f);
	
	if(!success) {
		if(!inplace_flag) {
			unlink(binary_path);
		}
		exit(1);
	}
	
	if(binary_path_was_malloced) {
		free((void *)binary_path);
	}
	
    return 0;
}
