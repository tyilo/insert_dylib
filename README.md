insert_dylib
============

Command line utility for inserting a dylib load command into a Mach-O binary.

Does the following (to each arch if the binary is fat):

- Adds a `LC_LOAD_DYLIB` load command to the end of the load commands
- Increments the mach header's `ncmds` and adjusts its `sizeofcmds`

Usage
-----

```
Usage: insert_dylib [--inplace] dylib_path binary_path [new_binary_path]
```

`insert_dylib` inserts a load command to load the `dylib_path` in `binary_path`.

Unless `--inplace` option is specified, `insert_dylib` will produce a new binary at `new_binary_path`.  
If neither `--inplace` nor `new_binary_path` is specified, the output binary will be located at the same location as the input binary with `_patched` prepended to the name.

### Example

```
$ cat > test.c
int main(void) {
	printf("Testing\n");
	return 0;}
^D
$ clang test.c -o test &> /dev/null
$ insert_dylib /usr/lib/libfoo.dylib test
Added LC_LOAD_DYLIB command to test_patched
$ ./test
Testing
$ ./test_patched
dyld: Library not loaded: /usr/lib/libfoo.dylib
  Referenced from: /Users/Tyilo/./test_patched
  Reason: image not found
Trace/BPT trap: 5
```

```
$ diff -u <(otool -hl test) <(otool -hl test_patched)
--- /dev/fd/63	2014-07-30 04:08:40.000000000 +0200
+++ /dev/fd/62	2014-07-30 04:08:40.000000000 +0200
@@ -1,7 +1,7 @@
-test:
+test_patched:
 Mach header
       magic cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags
- 0xfeedfacf 16777223          3  0x80          2    16       1296 0x00200085
+ 0xfeedfacf 16777223          3  0x80          2    17       1344 0x00200085
 Load command 0
       cmd LC_SEGMENT_64
   cmdsize 72
@@ -231,3 +231,10 @@
   cmdsize 16
   dataoff 8296
  datasize 64
+Load command 16
+          cmd LC_LOAD_DYLIB
+      cmdsize 48
+         name /usr/lib/libfoo.dylib (offset 24)
+   time stamp 0 Thu Jan  1 01:00:00 1970
+      current version 0.0.0
+compatibility version 0.0.0
```

Todo
----

- Handle `LC_CODE_SIGNATURE` load command
- Improved checking for free space to insert the new load command