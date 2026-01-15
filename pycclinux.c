// boot_and_builder.c
// Single binary: builder (--build) and runtime (extract & run appended .pyc).
// Linux-compatible version with proper Python integration

#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#define SEP "/"

// Footer format:
// [payload bytes ...][footer]
// footer = "PYBND" (5 bytes) + uint64 payload_size (little-endian) = 13 bytes total
static const char FOOTER_MAGIC[] = "PYBND";
static const size_t FOOTER_MAGIC_LEN = 5;
static const size_t FOOTER_LEN = 5 + 8; // magic + 8-byte payload size

// Helper: write little-endian uint64
static void write_u64_le(FILE* f, uint64_t v) {
    unsigned char buf[8];
    for (int i = 0; i < 8; ++i) buf[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    fwrite(buf, 1, 8, f);
}

// Helper: read little-endian uint64
static uint64_t read_u64_le(FILE* f) {
    unsigned char buf[8];
    if (fread(buf, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)buf[i]) << (8 * i);
    return v;
}

// Get path to running executable
static int get_self_path(char *out, size_t out_size) {
    ssize_t n = readlink("/proc/self/exe", out, out_size - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
}

// Build .pyc using Python's py_compile.compile() into out_pyc path.
// Returns 0 on success, nonzero on failure.
static int build_pyc_with_python(const char *script_path, const char *out_pyc_path) {
    int ret = 1;
    Py_Initialize();

    // Build a small Python snippet to call py_compile.compile(script, cfile=out_pyc, doraise=True)
    PyObject *py_module_name = PyUnicode_FromString("py_compile");
    if (!py_module_name) goto cleanup;
    PyObject *py_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    if (!py_module) {
        PyErr_Print();
        goto cleanup;
    }

    PyObject *py_func = PyObject_GetAttrString(py_module, "compile");
    if (!py_func || !PyCallable_Check(py_func)) {
        PyErr_Print();
        Py_XDECREF(py_func);
        Py_DECREF(py_module);
        goto cleanup;
    }

    PyObject *py_script = PyUnicode_FromString(script_path);
    PyObject *py_out = PyUnicode_FromString(out_pyc_path);
    if (!py_script || !py_out) {
        Py_XDECREF(py_script); Py_XDECREF(py_out);
        Py_DECREF(py_func); Py_DECREF(py_module);
        goto cleanup;
    }

    PyObject *kwargs = PyDict_New();
    PyDict_SetItemString(kwargs, "cfile", py_out);
    PyDict_SetItemString(kwargs, "doraise", Py_True);

    PyObject *args = PyTuple_Pack(1, py_script);
    PyObject *res = PyObject_Call(py_func, args, kwargs);

    Py_DECREF(args);
    Py_DECREF(kwargs);
    Py_DECREF(py_script);
    Py_DECREF(py_out);
    Py_DECREF(py_func);
    Py_DECREF(py_module);

    if (!res) {
        // compile failed (raises SyntaxError etc.)
        PyErr_Print();
        ret = 2;
    } else {
        Py_DECREF(res);
        ret = 0; // success
    }

cleanup:
    if (Py_IsInitialized()) Py_FinalizeEx();
    return ret;
}

// Append the payload file (pyc) to a copy of stub_exe and create out_exe
// The stub_exe is a path to the bootloader binary we want to copy from (often the running exe).
// Returns 0 on success.
static int append_payload_to_stub(const char *stub_exe, const char *payload_pyc, const char *out_exe) {
    FILE *f_stub = NULL, *f_payload = NULL, *f_out = NULL;
    int rc = 1;

    f_stub = fopen(stub_exe, "rb");
    if (!f_stub) { fprintf(stderr, "Failed to open stub: %s\n", stub_exe); goto end; }

    f_payload = fopen(payload_pyc, "rb");
    if (!f_payload) { fprintf(stderr, "Failed to open payload: %s\n", payload_pyc); goto end; }

    f_out = fopen(out_exe, "wb");
    if (!f_out) { fprintf(stderr, "Failed to create output exe: %s\n", out_exe); goto end; }

    // copy stub
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f_stub)) > 0) {
        if (fwrite(buf, 1, n, f_out) != n) { fprintf(stderr, "Write error\n"); goto end; }
    }

    // copy payload
    uint64_t payload_size = 0;
    while ((n = fread(buf, 1, sizeof(buf), f_payload)) > 0) {
        payload_size += n;
        if (fwrite(buf, 1, n, f_out) != n) { fprintf(stderr, "Write error\n"); goto end; }
    }

    // write footer: magic + payload_size (u64 LE)
    if (fwrite(FOOTER_MAGIC, 1, FOOTER_MAGIC_LEN, f_out) != FOOTER_MAGIC_LEN) { fprintf(stderr,"Footer write failed\n"); goto end; }
    write_u64_le(f_out, payload_size);

    rc = 0; // success

end:
    if (f_stub) fclose(f_stub);
    if (f_payload) fclose(f_payload);
    if (f_out) fclose(f_out);
    
    // Set executable permissions on output file
    if (rc == 0) {
        chmod(out_exe, 0755);
    }
    
    return rc;
}

// Extract appended payload from self and run it with embedded Python
static int run_appended_payload() {
    char selfpath[4096];
    unsigned char buf[4096];

    if (!get_self_path(selfpath, sizeof(selfpath))) {
        fprintf(stderr, "Failed to get self path\n");
        return 1;
    }

    FILE *f = fopen(selfpath, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open self binary\n");
        return 2;
    }

    // find footer at end
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 3; }
    long endpos = ftell(f);
    if (endpos < (long)FOOTER_LEN) { fclose(f); return 4; }

    // Seek to footer start position
    if (fseek(f, endpos - FOOTER_LEN, SEEK_SET) != 0) { fclose(f); return 5; }

    char magic[6] = {0};
    if (fread(magic, 1, FOOTER_MAGIC_LEN, f) != FOOTER_MAGIC_LEN) { fclose(f); return 6; }

    if (memcmp(magic, FOOTER_MAGIC, FOOTER_MAGIC_LEN) != 0) {
        // no payload
        fclose(f);
        fprintf(stderr, "No embedded payload found in binary\n");
        return 7;
    }

    uint64_t payload_size = read_u64_le(f);
    if (payload_size == 0) {
        fclose(f);
        fprintf(stderr, "Embedded payload size is zero\n");
        return 8;
    }

    // Compute payload start
    long payload_start = endpos - (long)FOOTER_LEN - (long)payload_size;
    if (payload_start < 0) { fclose(f); fprintf(stderr, "Invalid payload start\n"); return 9; }

    // Read payload into memory or write temp file
    if (fseek(f, payload_start, SEEK_SET) != 0) { fclose(f); return 10; }

    // create temp filename
    char tmpname[PATH_MAX];
    const char *td = getenv("TMPDIR");
    if (!td) td = "/tmp";
    snprintf(tmpname, sizeof(tmpname), "%s/embedded_payload_%ld.pyc", td, (long)getpid());

    FILE *fp = fopen(tmpname, "wb");
    if (!fp) { fclose(f); fprintf(stderr, "Failed to create temp pyc\n"); return 11; }

    uint64_t remaining = payload_size;
    while (remaining > 0) {
        size_t toread = (remaining > sizeof(buf)) ? sizeof(buf) : (size_t)remaining;
        size_t got = fread(buf, 1, toread, f);
        if (got == 0) break;
        fwrite(buf, 1, got, fp);
        remaining -= got;
    }
    fclose(fp);
    fclose(f);

    // Now run the pyc using embedded Python
    Py_Initialize();

    // Create Python code to load and execute the pyc file
    char py_code_str[2048];
    snprintf(py_code_str, sizeof(py_code_str),
        "import marshal\n"
        "with open('%s', 'rb') as f:\n"
        "    data = f.read()\n"
        "# Python 3.7+ has 16-byte header for hash-based pyc\n"
        "# Try to extract code object from pyc\n"
        "try:\n"
        "    # Skip header (typically 16 bytes in Python 3.7+)\n"
        "    code_obj = marshal.loads(data[16:])\n"
        "    exec(code_obj)\n"
        "except Exception as e:\n"
        "    # If that fails, try other header sizes\n"
        "    try:\n"
        "        code_obj = marshal.loads(data[12:])\n"
        "        exec(code_obj)\n"
        "    except:\n"
        "        code_obj = marshal.loads(data[8:])\n"
        "        exec(code_obj)\n",
        tmpname);

    int result = PyRun_SimpleString(py_code_str);
    
    if (result != 0) {
        fprintf(stderr, "Failed to execute embedded Python code\n");
        Py_FinalizeEx();
        remove(tmpname);
        return 13;
    }

    // finalize
    Py_FinalizeEx();

    // Remove temp pyc
    remove(tmpname);

    return 0;
}

// Builder mode: create pyc and append to stub to produce output exe
static int builder_mode(const char *script_path, const char *out_exe_path) {
    char selfpath[4096];
    if (!get_self_path(selfpath, sizeof(selfpath))) {
        fprintf(stderr, "Cannot get self path for stub copy\n");
        return 1;
    }

    // temp pyc path
    char temp_pyc[4096];
    const char *td = getenv("TMPDIR");
    if (!td) td = "/tmp";
    snprintf(temp_pyc, sizeof(temp_pyc), "%s/temp_build_payload_%ld.pyc", td, (long)getpid());

    printf("[*] Compiling %s -> %s\n", script_path, temp_pyc);
    int r = build_pyc_with_python(script_path, temp_pyc);
    if (r != 0) {
        fprintf(stderr, "[!] py_compile failed (code %d)\n", r);
        return r;
    }

    printf("[*] Appending payload to stub and creating %s\n", out_exe_path);
    r = append_payload_to_stub(selfpath, temp_pyc, out_exe_path);
    if (r != 0) {
        fprintf(stderr, "[!] Failed to append payload\n");
        remove(temp_pyc);
        return r;
    }

    remove(temp_pyc);
    printf("[+] Built %s successfully\n", out_exe_path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--build") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s --build <script.py> <out_binary>\n", argv[0]);
            return 1;
        }
        const char *script = argv[2];
        const char *outexe = argv[3];
        return builder_mode(script, outexe);
    } else {
        // normal run: try to find appended payload and run it
        int r = run_appended_payload();
        if (r != 0) {
            fprintf(stderr, "Bootloader: no embedded payload or run failed (code %d)\n", r);
            fprintf(stderr, "Usage to build: %s --build <script.py> <out_binary>\n", argv[0]);
        }
        return r;
    }
}
