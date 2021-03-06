#include "preload.h"
#include "argv_processor.h"

#include <crete/harness.h>
#include <crete/custom_instr.h>
#include <crete/harness_config.h>

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp> // Needed for text_iarchive (for some reason).
#include <boost/property_tree/xml_parser.hpp>
#include <boost/filesystem.hpp>

#include <dlfcn.h>
#include <cassert>
#include <cstdlib>

#include <iostream>
#include <string>
#include <stdexcept>
#include <stdio.h>

#if CRETE_HOST_ENV
#include "run_config.h"
#endif // CRETE_HOST_ENV

using namespace std;
using namespace crete;
namespace fs = boost::filesystem;

const char* const crete_config_file = "harness.config.serialized";
const std::string crete_proc_maps_file = "proc-maps.log";

void print_back_trace();

config::HarnessConfiguration crete_load_configuration()
{
    std::ifstream ifs(crete_config_file,
                      ios_base::in | ios_base::binary);

    if(!ifs.good())
    {
        throw std::runtime_error("failed to open file: " + std::string(crete_config_file));
    }

    boost::archive::text_iarchive ia(ifs);
    config::HarnessConfiguration config;
    ia >> config;

    return config;
}

// Support symbolic stdin operated by standard c functions (eg. fgets, fread, scanf, etc)
void crete_make_concolic_stdin_std(size_t size)
{
    //1. Allocate buffer for stdin_ramdisk and write symbolic value to it
    char* crete_stdin_buffer = (char *)malloc(size);
    assert(crete_stdin_buffer &&
            "malloc() failed in preload::crete_make_concolic_stdin_std().\n");
    memset(crete_stdin_buffer, 0, size);
    crete_make_concolic(crete_stdin_buffer, size, "crete-stdin");

    //2. Write symbolic value to file "crete_stdin_ramdisk"
    char stdin_ramdisk_file[512];
    memset(stdin_ramdisk_file, 0, 512);
    sprintf(stdin_ramdisk_file, "%s/crete_stdin_ramdisk", getenv("CRETE_RAMDISK_PATH"));

    //TODO: xxx shall I open it with flag "b" (binary)?
    FILE *crete_stdin_fd = fopen (stdin_ramdisk_file, "wb");
    if(crete_stdin_fd == NULL) {
      printf("Error: can't open file %s for writing\n", stdin_ramdisk_file);
      exit(-1);
    }

    size_t out_result = fwrite(crete_stdin_buffer, 1, size, crete_stdin_fd);

    if(out_result != size) {
      printf("Error: write symbolic content to %s failed\n", stdin_ramdisk_file);
      exit(-1);
    }

    fclose(crete_stdin_fd);

    //3. Redirect stdin to file "crete_stdin_ramdisk"
    //TODO: xxx shall I open it with flag "b" (binary)?
    if(freopen(stdin_ramdisk_file, "rb", stdin) == NULL) {
      printf("Error: Redirect stdin to %s by freopen() failed.\n",
              stdin_ramdisk_file);
    }
}

void crete_make_concolic_stdin_posix_blank(size_t size) {
    char* crete_stdin_buffer = (char *)malloc(size);
    assert(crete_stdin_buffer &&
            "malloc() failed in preload::crete_make_concolic_stdin_posix_blank().\n");
    memset(crete_stdin_buffer, 0, size);
    crete_make_concolic(crete_stdin_buffer, size, "crete-stdin-posix");
}

void crete_process_stdin(const config::HarnessConfiguration& hconfig)
{
    const config::STDStream stdin_config = hconfig.get_stdin();
    //FIXME: xxx add support for non-concolic stdin
    if(stdin_config.concolic && stdin_config.size > 0)
    {
        crete_make_concolic_stdin_std(stdin_config.size);
        crete_make_concolic_stdin_posix_blank(stdin_config.size);
    }
}

// replace the original crete_make_concolic_file_std() for supporting libc file operations
void crete_make_concolic_file_libc_std(const config::File& file)
{
    string filename = file.path.filename().string();

    assert(!filename.empty() && "[CRETE] file name must be valid");
    assert(file.size > 0 && "[CRETE] file size must be greater than zero");

    char* buffer = new char [file.size];
    memset(buffer, 0, file.size);

    crete_make_concolic(buffer, file.size, filename.c_str());

    // write symbolic value to ramdisk file
    string file_full_path = file.path.string();
    FILE *out_fd = fopen (file_full_path.c_str(), "wb");
    if(out_fd == NULL) {
      printf("Error: can't open file %s for writing\n", file_full_path.c_str());
      throw runtime_error("failed to open file in preload for making concolic file\n");
    }

    size_t out_result = fwrite(buffer, 1, file.size, out_fd);

    if(out_result != file.size) {
      throw runtime_error("wrong size of writing symbolic values in preload for making concolic file\n");
    }

    fclose(out_fd);
}

void crete_make_concolic_file_posix_blank(const config::File& file)
{
    string filename = file.path.filename().string();

    assert(!filename.empty() && "[CRETE] file name must be valid");
    assert(file.size > 0 && "[CRETE] file size must be greater than zero");

    filename = filename + "-posix";

    char* buffer = new char [file.size];
    memset(buffer, 0, file.size);

    string file_full_path = file.path.string();
    crete_make_concolic(buffer, file.size, filename.c_str());

    memset(buffer, 0, file.size);
}

void crete_make_concolic_file(const config::File& file)
{
    // Since we don't know what file routines will be used (e.g., open() vs fopen()),
    // initialize both kinds.
    crete_make_concolic_file_libc_std(file);
    crete_make_concolic_file_posix_blank(file);
}

void crete_process_files(const config::HarnessConfiguration& hconfig)
{
    const config::Files& files = hconfig.get_files();
    const size_t file_count = files.size();

    if(file_count == 0)
        return;

    for(config::Files::const_iterator it = files.begin();
        it != files.end();
        ++it)
    {
        if(!it->concolic)
            continue;

        crete_make_concolic_file(*it);
    }
}

void crete_process_configuration(const config::HarnessConfiguration& hconfig,
                                 int& argc, char**& argv)
{
    // Note: order matters.
    config::process_argv(hconfig.get_arguments(),
                         hconfig.is_first_iteration(),
                         argc, argv);
    crete_process_files(hconfig);
    crete_process_stdin(hconfig);
}

void crete_preload_initialize(int& argc, char**& argv)
{
    // Should exit program while being launched as prime
    // FIXME: xxx hconfig.is_first_iteration() is not being used
    crete_initialize(argc, argv);
    // Need to call crete_capture_begin before make_concolics, or they won't be captured.
    // crete_capture_end() is registered with atexit() in crete_initialize().
    crete_capture_begin();

    config::HarnessConfiguration hconfig = crete_load_configuration();
    crete_process_configuration(hconfig, argc, argv);
}

#if CRETE_HOST_ENV
bool crete_verify_executable_path_matches(const char* argv0)
{
    std::ifstream ifs(crete_config_file,
                      ios_base::in | ios_base::binary);

    if(!ifs.good())
    {
        throw std::runtime_error("failed to open file: " + std::string(crete_config_file));
    }

    boost::archive::text_iarchive ia(ifs);
    config::RunConfiguration config;
    ia >> config;

    return fs::equivalent(config.get_executable(), fs::path(argv0));
}
#endif // CRETE_HOST_ENV

int __libc_start_main(
        int *(main) (int, char **, char **),
        int argc,
        char ** ubp_av,
        void (*init) (void),
        void (*fini) (void),
        void (*rtld_fini) (void),
        void *stack_end) {

//    atexit(print_back_trace);

    __libc_start_main_t orig_libc_start_main;

    try
    {
        orig_libc_start_main = (__libc_start_main_t)dlsym(RTLD_NEXT, "__libc_start_main");
        if(orig_libc_start_main == 0)
            throw runtime_error("failed to find __libc_start_main");

#if CRETE_HOST_ENV
        // HACK (a bit of one)...
        // TODO: (crete-memcheck) research how to get around the problem of preloading
        // valgrind as well, in which case we check if the executable name matches.
        if(crete_verify_executable_path_matches(ubp_av[0]))
        {
            crete_preload_initialize(argc, ubp_av);
        }
#else
        crete_preload_initialize(argc, ubp_av);
#endif // CRETE_HOST_ENV
    }
    catch(exception& e)
    {
        cerr << "[CRETE] Exception: " << e.what() << endl;
        exit(1);
    }
    catch(...) // Non-standard exception
    {
        cerr << "[CRETE] Non-standard exception encountered!" << endl;
        assert(0);
    }

    (*orig_libc_start_main)(main, argc, ubp_av, init, fini, rtld_fini, stack_end);

    exit(1); // This is never reached. Doesn't matter, as atexit() is called upon returning from main().
}


#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BT_BUF_SIZE 100

void print_back_trace() {
    int j, nptrs;
    void *buffer[BT_BUF_SIZE];
    char **strings;

    nptrs = backtrace(buffer, BT_BUF_SIZE);
    fprintf(stderr, "=========================[preload]======================================\n");
    fprintf(stderr, "backtrace() returned %d addresses\n", nptrs);

    /* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
       would produce similar output to the following: */

    strings = backtrace_symbols(buffer, nptrs);
    if (strings == NULL) {
        perror("backtrace_symbols");
        exit(EXIT_FAILURE);
    }

    for (j = 0; j < nptrs; j++)
        fprintf(stderr, "%s\n", strings[j]);

    fprintf(stderr, "======================================================================\n");
    free(strings);
}
