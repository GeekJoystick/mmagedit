#include "mmagedit.h"
#include "defer.h"

#include <type_traits>

#include <stdio.h>
#include <iostream>
#include <Python.h>

typedef PyObject PyObjectBorrowed;

#define defer_decref(pyobj) defer(Py_XDECREF(pyobj))
#define store_string(s) (g_static_string_out = s).c_str()

#define args_end nullptr

#define min_version 202103161639

namespace
{
	int g_log_level = 0;

	template<typename T>
	void log(T val, int level = LOG_TRIVIAL)
	{
		if (level <= g_log_level)
		{
			(level == LOG_ERROR ? std::cerr : std::cout)
				<< val << std::endl;
		}
	}

	PyObject* PyString(const char* s)
	{
		return PyUnicode_FromString(s);
	}

	template<typename... T>
	decltype(PyObject_CallMethodObjArgs(nullptr, nullptr))
	PyObject_CallMethodObjArgsString(
		PyObject* obj,
		const char* attr,
		T... args
	)
	{
		PyObject* attrstr = PyString(attr);
		defer_decref(attrstr);

		return PyObject_CallMethodObjArgs(obj, attrstr, args...);
	}

	std::string PyObject_AsString(PyObjectBorrowed* po)
	{
		if (po)
		{
			if (PyUnicode_Check(po))
			{
				PyObject* str = PyUnicode_AsEncodedString(po, "utf-8", "~E~");
				defer_decref(str);
				const char *bytes = PyBytes_AS_STRING(str);

				return bytes;
			}
			else
			{
				PyObject* repr = PyObject_Repr(po);
				defer_decref(repr);
				PyObject* str = PyUnicode_AsEncodedString(repr, "utf-8", "~E~");
				defer_decref(str);
				const char *bytes = PyBytes_AS_STRING(str);

				return bytes;
			}
		}
		else
		{
			return "null";
		}
	}

	// traceback module
	PyObject* g_traceback;

	// used to hold string references that are
	// returned by library functions.
	std::string g_static_string_out;

	std::string g_error = "";

	template <typename S=const char*, typename T=error_code_t>
	inline T error(S _error, T code)
	{
		g_error = _error;

		log("mmdata error: " + std::string(_error), LOG_ERROR);

		return code;
	}

	template <typename S=const char*, typename T=error_code_t>
	inline T error(S _error)
	{
		return error(_error, 1);
	}

	// return an error code if python has thrown an exception.
	#define check_error_python(errcode) \
		 if (check_error_python_impl()) return errcode;

	// check in advance of running a function if the python error is already triggered, and if so,
	// report that.
	#define precheck_error_python(errcode) \
		if (check_error_python_impl("A python error has occurred before entering the libmagedit function. This is an internal library error and should be reported to the developer.")) return errcode;

	#define postcheck_error_python(errcode) \
		if (check_error_python_impl("A python error has occurred before the libmagedit function exited. This is an internal library error and should be reported to the developer.")) return errcode;

	inline int check_error_python_impl(std::string message_prelude="")
	{
		PyObject* ptype = nullptr;
		PyObject* pvalue = nullptr;
		PyObject* pbt = nullptr;

		if (PyErr_Occurred())
		{
			PyErr_Fetch(&ptype, &pvalue, &pbt);
			// an error has occurred
			std::string s;
			s = message_prelude + "A python exception occurred.";
			
			if (g_traceback)
			{
				PyErr_NormalizeException(&ptype, &pvalue, &pbt);
				PyErr_SetExcInfo(ptype, pvalue, pbt);
				PyObject* exc_str = PyObject_CallMethodObjArgsString(g_traceback, "format_exc", args_end);
				defer_decref(exc_str);
				if (exc_str)
				{
					s += "\n" + PyObject_AsString(exc_str);
				}
				else
				{
					s += " (unable to decode exception details)";
				}
			}
			else
			{
				Py_XDECREF(ptype);
				Py_XDECREF(pvalue);
				Py_XDECREF(pbt);
			}

			PyErr_Clear();

			return error(s);
		}

		return 0;
	}

	PyObject *g_globals, *g_locals;

	// mmagedit modules
	PyObjectBorrowed *g_constants, *g_util;

	// MMData instance
	PyObject* g_data;

	PyObjectBorrowed* get_world(world_idx_t idx)
	{
		if (idx < 0) return nullptr;
		PyObjectBorrowed* world_array = PyObject_GetAttrString(g_data, "worlds");
		if (!world_array)
		{
			return nullptr;
		}

		if (idx < PyList_Size(world_array))
		{
			return PyList_GetItem(world_array, idx);
		}

		return nullptr;
	}
}

int
mmagedit_get_error_occurred()
{
	return g_error.length() > 0;
}

void mmagedit_clear_error()
{
	g_error = "";
}

const char*
mmagedit_get_error()
{
	defer(mmagedit_clear_error());
	return store_string(g_error);
}

void mmagedit_set_log_level(int l)
{
	g_log_level = l;
}

int mmagedit_init(const char* path_to_mmagedit)
{
	log("initializing libpython...");
	Py_Initialize();
	log("done.");

	g_globals = PyDict_New();
	g_locals = PyDict_New();

	{
		// set arg0 to path to mmagedit.py
		// set arg1 to "--as-lib"
		wchar_t* args[2] {
			Py_DecodeLocale(path_to_mmagedit, nullptr),
			Py_DecodeLocale("--as-lib", nullptr)
		};
		defer(PyMem_RawFree(args[0]));
		defer(PyMem_RawFree(args[1]));
		PySys_SetArgv(2, args);
	}

	if (!g_globals || !g_locals)
	{
		return error("Unable to create globals/locals dicts");
	}

	g_traceback = PyImport_ImportModule("traceback");
	check_error_python(-1);
	if (!g_traceback) return error("unable to access traceback module.");

	// run mmagedit.py
	FILE* f = fopen(path_to_mmagedit, "r");
	defer(fclose(f));

	log("loading mmagedit...");
	PyObject* run_rv = PyRun_File(f, path_to_mmagedit, Py_file_input, g_globals, g_locals);
	defer_decref(run_rv);
	check_error_python(-1);

	log("done.");

	if (!run_rv)
	{
		PyErr_Print();
		return error("A python exception occurred while loading mmagedit.");
	}

	// retrieve important locals
	g_constants = PyDict_GetItemString(g_locals, "constants");
	g_util = PyDict_GetItemString(g_locals, "util");

	if (!g_constants || !g_util) return error("unable to access src.constants or src.util modules");

	// create mmdata instance
	PyObjectBorrowed* MMData = PyDict_GetItemString(g_locals, "MMData");
	if (!MMData) return error("unable to access class src.mmdata.MMData");

	PyObject* args = PyTuple_New(0);
	defer_decref(args);

	g_data = PyObject_Call(MMData, args, nullptr);
	check_error_python(-1);
	if (!g_data) return error("unable to create MMData instance");

	auto version = mmagedit_get_version_int();
	if (version < min_version)
	{
		return error("libmmagedit requires a more recent version of the library. (format " + std::to_string(version) + " is installed, but libmmagedit needs at least format " + std::to_string(min_version) + ")");
	}

	postcheck_error_python(1);
	return 0;
}

int mmagedit_end()
{
	Py_XDECREF(g_globals);
	Py_XDECREF(g_locals);
	Py_XDECREF(g_data);

	Py_Finalize();

	return 0;
}

const char*
mmagedit_get_name_version_date()
{
	precheck_error_python("");

	PyObjectBorrowed* fn = PyObject_GetAttrString(g_constants, "get_version_and_date");

	PyObject* args = PyTuple_New(0);
	defer_decref(args);

	PyObject* result = PyObject_Call(fn, args, nullptr);
	check_error_python("");
	defer_decref(result);

	return store_string(PyObject_AsString(result));
}

unsigned long int
mmagedit_get_version_int()
{
	precheck_error_python(0);

	static_assert(
		sizeof(
			decltype(PyNumber_AsSsize_t(nullptr, nullptr))
		) == sizeof(unsigned long int)
	);

	return PyNumber_AsSsize_t(
		PyObject_GetAttrString(g_constants, "mmfmt"),
		nullptr
	);
}

// returns 1 andsets error to mmdata's errors if any occurred;
// otherwise, returns 0.
#define check_error_mmdata if (error_code_t e = _check_error_mmdata_impl()) return e;
#define check_error_mmdata_rval(rval) if (error_code_t e = [](){check_error_mmdata else return 0;}()) return rval;
static error_code_t _check_error_mmdata_impl()
{
	PyObject* result = PyObject_CallMethodObjArgsString(g_data, "errors_string", args_end);
	check_error_python(1);
	defer_decref(result);

	if (result && result != Py_None)
	{
		return error(PyObject_AsString(result));
	}

	return 0;
}

error_code_t
mmagedit_load_rom(const char* path_to_rom)
{
	precheck_error_python(1);

	PyObject* rompath = PyString(path_to_rom);
	defer_decref(rompath);

	PyObject* result = PyObject_CallMethodObjArgsString(g_data, "read", rompath, args_end);
	check_error_python(1);
	defer_decref(result);

	if (!result) return error("failure to invoke mmdata.read()");

	if (PyObject_Not(result))
	{
		check_error_mmdata else return error("unknown error");
	}

	return 0;
}

error_code_t
mmagedit_load_hack(const char* path_to_hack)
{
	precheck_error_python(1);

	PyObject* hackpath = PyString(path_to_hack);
	defer_decref(hackpath);

	PyObject* result = PyObject_CallMethodObjArgsString(g_data, "parse", hackpath, args_end);
	check_error_python(1);
	defer_decref(result);

	if (!result) return error("failure to invoke mmdata.parse()");

	if (PyObject_Not(result))
	{
		check_error_mmdata else return error("unknown error");
	}

	return 0;
}

error_code_t
mmagedit_write_rom(const char* path_to_rom)
{
	precheck_error_python(1);

	PyObject* rompath = PyString(path_to_rom);
	defer_decref(rompath);

	PyObject* result = PyObject_CallMethodObjArgsString(g_data, "write", rompath, args_end);
	check_error_python(1);
	defer_decref(result);

	if (!result) return error("failure to invoke mmdata.write()");

	if (PyObject_Not(result))
	{
		check_error_mmdata else return error("unknown error");
	}

	return 0;
}

error_code_t
mmagedit_write_hack(const char* path_to_hack, bool oall)
{
	precheck_error_python(1);

	PyObject* hackpath = PyString(path_to_hack);
	defer_decref(hackpath);

	PyObject* pyoall = PyBool_FromLong(oall);
	defer_decref(pyoall);

	PyObject* result = PyObject_CallMethodObjArgsString(g_data, "stat", hackpath, pyoall, args_end);
	check_error_python(1);
	defer_decref(result);

	if (!result) return error("failure to invoke mmdata.stat()");

	if (PyObject_Not(result))
	{
		check_error_mmdata else return error("unknown error");
	}

	return 0;
}

json_t
mmagedit_get_state()
{
	return mmagedit_get_state_select("");
}

json_t
mmagedit_get_state_select(const char* jsonpath)
{
	precheck_error_python("null");

	PyObject* pyjsonpath = PyString(jsonpath);
	defer_decref(pyjsonpath);

	PyObject* result = PyObject_CallMethodObjArgsString(g_data, "serialize_json_str", pyjsonpath, args_end);
	check_error_python("null");
	defer_decref(result);

	return store_string(PyObject_AsString(result));
}

error_code_t
mmagedit_apply_state(json_t json)
{
	precheck_error_python(1);
	
	PyObject* str = PyString(json);
	defer_decref(str);
	if (!str) return error("unable to create PyString");

	PyObject* result = PyObject_CallMethodObjArgsString(g_data, "deserialize_json_str", str, args_end);
	defer_decref(result);

	if (PyObject_Not(result))
	{
		check_error_mmdata else return error("unknown error");
	}

	return 0;
}

medtile_idx_t
mmagedit_get_mirror_tile_idx(world_idx_t idx, medtile_idx_t in)
{
	// validation
	if (in < 0) return error("negative medtile idx forbidden.", -1);

	precheck_error_python(-1);

	PyObjectBorrowed* world = get_world(idx);
	if (!world)
	{
		return error("No such world exists", -1);
	}

	PyObject* pyin = PyLong_FromLong(in);
	defer_decref(pyin);

	PyObject* retval = PyObject_CallMethodObjArgsString(world, "mirror_tile", pyin, args_end);
	defer_decref(retval);

	medtile_idx_t rv = PyLong_AsLong(retval);
	if (rv < 0) return error("Invalid mirror tile return", -1);

	return rv;
}

namespace
{
	int g_hello_world_int;
}

void
mmagedit_hw_set_int(int c) 
{
	g_hello_world_int = c;
}

int
mmagedit_hw_get_int()
{
	return g_hello_world_int;
}

const char*
mmagedit_hw_get_str()
{
	return "Hello, World!";
}

// just a simple test main function
// usage: mmagedit /path/to/mmagedit.py [base.nes] [hack.txt]
static int execmain(int argc, char** argv)
{
	if (argc == 0)
	{
		std::cout << "usage: mmagedit /path/to/mmagedit.py [base.nes] [hack.txt]" << std::endl;
		return 5;
	}
	mmagedit_set_log_level(5);
	if (mmagedit_init(argv[1])) return 1;
	std::cout << mmagedit_get_name_version_date() << std::endl;
	std::cout << mmagedit_get_version_int() << std::endl;
	if (argc > 2) if (!mmagedit_load_rom(argv[2])) std::cout << "successfully loaded rom" << std::endl; else return 2;
	if (argc > 3) if (!mmagedit_load_hack(argv[3])) std::cout << "successfully loaded hack" << std::endl; else return 3;
	if (argc > 2)
	{
		std::cout << mmagedit_get_state_select(".chr[0][0:2]") << std::endl;
	}
	mmagedit_end();
	return 0;
}

#ifdef MAIN
int main(int argc, char** argv)
{
	return execmain(argc, argv);
}
#endif