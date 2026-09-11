# tkConfig.sh --
#
# This shell script (for sh) is generated automatically by Tk's
# configure script.  It will create shell variables for most of
# the configuration options discovered by the configure script.
# This script is intended to be included by the configure scripts
# for Tk extensions so that they don't have to figure this all
# out for themselves.  This file does not duplicate information
# already provided by tclConfig.sh, so you may need to use that
# file in addition to this one.
#
# The information in this file is specific to a single platform.

TK_DLL_FILE="tcl9tk90.dll"

# Tk's version number.
TK_VERSION='9.0'
TK_MAJOR_VERSION='9'
TK_MINOR_VERSION='0'
TK_PATCH_LEVEL='.4'

# -D flags for use with the C compiler.
TK_DEFS='-DPACKAGE_NAME=\"tk\" -DPACKAGE_TARNAME=\"tk\" -DPACKAGE_VERSION=\"9.0\" -DPACKAGE_STRING=\"tk\ 9.0\" -DPACKAGE_BUGREPORT=\"\" -DPACKAGE_URL=\"\" -DHAVE_STDIO_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_UNISTD_H=1 -DSTDC_HEADERS=1 -DSTATIC_BUILD=1 -DMODULE_SCOPE=extern -DTCL_CFG_DO64BIT=1 -DHAVE_NO_SEH=1 -DHAVE_CAST_TO_UNION=1 -DHAVE_INTPTR_T=1 -DHAVE_UINTPTR_T=1 -DHAVE_INTPTR_T=1 -DHAVE_UINTPTR_T=1 -DNDEBUG=1 -DTCL_CFG_OPTIMIZED=1 -DZIPFS_BUILD=2'

# Flag, 1: we built a shared lib, 0 we didn't
TK_SHARED_BUILD=0

# TK_DBGX used to be used to distinguish debug vs. non-debug builds.
# This was a righteous pain so the core doesn't do that any more.
TK_DBGX=

# The name of the Tk library (may be either a .a file or a shared library):
TK_LIB_FILE='libtcl9tk90.a'

# Additional libraries to use when linking Tk.
TK_LIBS='-lnetapi32 -lkernel32 -luser32 -ladvapi32 -luserenv -lws2_32 -lgdi32 -lcomdlg32 -limm32 -lcomctl32 -lshell32 -luuid -lole32 -loleaut32 -lwinspool'

# Top-level directory in which Tk's platform-independent files are
# installed.
TK_PREFIX='/usr/local'

# Top-level directory in which Tk's platform-specific files (e.g.
# executables) are installed.
TK_EXEC_PREFIX='/usr/local'

# -l flag to pass to the linker to pick up the Tk library
TK_LIB_FLAG='-ltcl9tk90'

# String to pass to linker to pick up the Tk library from its
# build directory.
TK_BUILD_LIB_SPEC='-LC:/Users/Mo/Pictures/Cob-v0.0.5/vendor/TK/win -ltcl9tk90'

# String to pass to linker to pick up the Tk library from its
# installed directory.
TK_LIB_SPEC='-L/usr/local/lib -ltcl9tk90'

# Location of the top-level source directory from which Tk was built.
# This is the directory that contains a README file as well as
# subdirectories such as generic, unix, etc.  If Tk was compiled in a
# different place than the directory containing the source files, this
# points to the location of the sources, not the location where Tk was
# compiled.
TK_SRC_DIR='C:/Users/Mo/Pictures/Cob-v0.0.5/vendor/TK'

# Needed if you want to make a 'fat' shared library library
# containing tk objects or link a different wish.
TK_CC_SEARCH_FLAGS=''
TK_LD_SEARCH_FLAGS=''

# The name of the Tk stub library (.a):
TK_STUB_LIB_FILE='libtkstub.a'

# -l flag to pass to the linker to pick up the Tk stub library
TK_STUB_LIB_FLAG='-ltkstub'

# String to pass to linker to pick up the Tk stub library from its
# build directory.
TK_BUILD_STUB_LIB_SPEC='-LC:/Users/Mo/Pictures/Cob-v0.0.5/vendor/TK/win -ltkstub'

# String to pass to linker to pick up the Tk stub library from its
# installed directory.
TK_STUB_LIB_SPEC='-L/usr/local/lib -ltkstub'

# Path to the Tk stub library in the build directory.
TK_BUILD_STUB_LIB_PATH='C:/Users/Mo/Pictures/Cob-v0.0.5/vendor/TK/win/libtkstub.a'

# Path to the Tk stub library in the install directory.
TK_STUB_LIB_PATH='/usr/local/lib/libtkstub.a'
