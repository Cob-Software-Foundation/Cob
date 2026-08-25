#include <stdio.h>
#if defined(_WIN32)
#include <fcntl.h>
#endif

/* Define architecture */
#if defined(__i386__) || defined _M_IX86
# define TRIPLET_ARCH "i386"
#elif defined(__x86_64__) || defined _M_AMD64
# define TRIPLET_ARCH "x86_64"
#elif defined(__arm__)
# define TRIPLET_ARCH "arm"
#elif defined(__aarch64__)
# define TRIPLET_ARCH "aarch64"
#elif defined(__riscv) && defined(__LP64__)
# define TRIPLET_ARCH "riscv64"
#else
# define TRIPLET_ARCH "unknown"
#endif

/* Define OS */
#if defined (__linux__)
# define TRIPLET_OS "linux"
#elif defined (__FreeBSD__) || defined (__FreeBSD_kernel__)
# define TRIPLET_OS "kfreebsd"
#elif defined(_WIN32)
# define TRIPLET_OS "win32"
#elif defined(__APPLE__)
# define TRIPLET_OS "darwin"
#elif !defined (__GNU__)
# define TRIPLET_OS "unknown"
#endif

/* Define calling convention and ABI */
#if defined (__ARM_EABI__)
# if defined (__ARM_PCS_VFP)
# define TRIPLET_ABI "gnueabihf"
# else
# define TRIPLET_ABI "gnueabi"
# endif
#else
# define TRIPLET_ABI "gnu"
#endif

#if defined _WIN32
# define TRIPLET TRIPLET_ARCH "-" TRIPLET_OS
#elif defined __GNU__
# define TRIPLET TRIPLET_ARCH "-" TRIPLET_ABI
#else
# define TRIPLET TRIPLET_ARCH "-" TRIPLET_OS "-" TRIPLET_ABI
#endif

#if defined(_WIN32)
int _CRT_glob = 0;
#endif

#ifdef C2STR
/* This build of conftest.c doubles as `c2str`: a tiny tool that turns
 * an arbitrary file into a C string literal, used by the Makefile to
 * turn include/tccdefs.h into tccdefs_.h (which tccpp.c #includes
 * directly inside a cstr_cat(...) call to embed TCC's built-in
 * predefined macros/headers as a string at compile time). Usage:
 *   c2str <input-file> <output.h>
 */
int main(int argc, char *argv[])
{
    FILE *fin, *fout;
    int c;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <input-file> <output.h>\n", argv[0]);
        return 1;
    }
    fin = fopen(argv[1], "rb");
    if (!fin) { perror(argv[1]); return 1; }
    fout = fopen(argv[2], "wb");
    if (!fout) { perror(argv[2]); fclose(fin); return 1; }

    fputc('"', fout);
    while ((c = fgetc(fin)) != EOF) {
        switch (c) {
            case '\n': fputs("\\n\"\n\"", fout); break;
            case '"':  fputs("\\\"", fout); break;
            case '\\': fputs("\\\\", fout); break;
            default:   fputc(c, fout); break;
        }
    }
    fputs("\"\n", fout);

    fclose(fin);
    fclose(fout);
    return 0;
}
#else

int main(int argc, char *argv[])
{
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY); /* don't translate \n to \r\n */
#endif
    switch(argc == 2 ? argv[1][0] : 0) {
    case 'b'://igendian
        {
            volatile unsigned foo = 0x01234567;
            puts(*(unsigned char*)&foo == 0x67 ? "no" : "yes");
            break;
        }
#if defined(__clang__)
    case 'm'://inor
        printf("%d\n", __clang_minor__);
        break;
    case 'v'://ersion
        printf("%d\n", __clang_major__);
        break;
#elif defined(__TINYC__)
    case 'v'://ersion
        puts("0");
        break;
    case 'm'://inor
        printf("%d\n", __TINYC__);
        break;
#elif defined(_MSC_VER)
    case 'v'://ersion
        puts("0");
        break;
    case 'm'://inor
        printf("%d\n", _MSC_VER);
        break;
#elif defined(__GNUC__) && defined(__GNUC_MINOR__)
    /* GNU comes last as other compilers may add 'GNU' compatibility */
    case 'm'://inor
        printf("%d\n", __GNUC_MINOR__);
        break;
    case 'v'://ersion
        printf("%d\n", __GNUC__);
        break;
#else
    case 'm'://inor
    case 'v'://ersion
        puts("0");
        break;
#endif
    case 't'://riplet
        puts(TRIPLET);
        break;
    case 'c'://ompiler
#if defined(__clang__)
        puts("clang");
#elif defined(__TINYC__)
        puts("tcc");
#elif defined(_MSC_VER)
        puts("msvc");
#elif defined(__GNUC__)
        puts("gcc");
#else
        puts("unknown");
#endif
        break;
    default:
        break;
    }
    return 0;
}
#endif /* C2STR */
