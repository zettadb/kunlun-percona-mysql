# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.5

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list


# Suppress display of executed commands.
$(VERBOSE).SILENT:


# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E remove -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server

# Include any dependencies generated for this target.
include storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/depend.make

# Include the progress variables for this target.
include storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/progress.make

# Include the compile flags for this target's objects.
include storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/flags.make

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o: storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/flags.make
storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o: storage/tokudb/PerconaFT/buildheader/make_tdb.cc
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o"
	cd /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader && /usr/bin/c++   $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/make_tdb.dir/make_tdb.cc.o -c /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader/make_tdb.cc

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/make_tdb.dir/make_tdb.cc.i"
	cd /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader && /usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader/make_tdb.cc > CMakeFiles/make_tdb.dir/make_tdb.cc.i

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/make_tdb.dir/make_tdb.cc.s"
	cd /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader && /usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader/make_tdb.cc -o CMakeFiles/make_tdb.dir/make_tdb.cc.s

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.requires:

.PHONY : storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.requires

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.provides: storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.requires
	$(MAKE) -f storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/build.make storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.provides.build
.PHONY : storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.provides

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.provides.build: storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o


# Object files for target make_tdb
make_tdb_OBJECTS = \
"CMakeFiles/make_tdb.dir/make_tdb.cc.o"

# External object files for target make_tdb
make_tdb_EXTERNAL_OBJECTS =

storage/tokudb/PerconaFT/buildheader/make_tdb: storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o
storage/tokudb/PerconaFT/buildheader/make_tdb: storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/build.make
storage/tokudb/PerconaFT/buildheader/make_tdb: storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking CXX executable make_tdb"
	cd /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/make_tdb.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/build: storage/tokudb/PerconaFT/buildheader/make_tdb

.PHONY : storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/build

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/requires: storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/make_tdb.cc.o.requires

.PHONY : storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/requires

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/clean:
	cd /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader && $(CMAKE_COMMAND) -P CMakeFiles/make_tdb.dir/cmake_clean.cmake
.PHONY : storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/clean

storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/depend:
	cd /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader /mnt/workspace/percona-server-8.0-source-tarballs/test/percona-server/storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : storage/tokudb/PerconaFT/buildheader/CMakeFiles/make_tdb.dir/depend
