/*
 * Launch a python module from an archive.
 * Copyright (C) 2005-2011, Giovanni Bajo
 * Based on previous work under copyright (c) 2002 McMillan Enterprises, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU General Public License, the
 * authors give you unlimited permission to link or embed the compiled
 * version of this file into combinations with other programs, and to
 * distribute those combinations without any restriction coming from the
 * use of this file. (The General Public License restrictions do apply in
 * other respects; for example, they cover modification of the file, and
 * distribution when not linked into a combine executable.)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */
#include <stdio.h>
#ifdef WIN32
 #include <windows.h>
 #include <direct.h>
 #include <process.h>
 #include <io.h>
#else
 #include <unistd.h>
 #include <fcntl.h>
 #include <dlfcn.h>
 #include <dirent.h>
 #include <stdarg.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include "launch.h"
#include <string.h>
#include "zlib.h"

#include "pyi_global.h"
#include "pyi_utils.h"
#include "pyi_pythonlib.h"

#ifdef WIN32
#define snprintf _snprintf
#define vsnprintf _vsnprintf
#endif


unsigned char *extract(ARCHIVE_STATUS *status, TOC *ptoc);

/*
 * The functions in this file defined in reverse order so that forward
 * declarations are not necessary.
 */


#if defined(WIN32) && defined(WINDOWED)
/* The code duplication in the functions below are because
 * standard macros with variable numer of arguments (variadic macros) are
 * supported by Microsoft only starting from Visual C++ 2005.
 */

#define MBTXTLEN 200

void mbfatalerror(const char *fmt, ...)
{
	char msg[MBTXTLEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, MBTXTLEN, fmt, args);
	msg[MBTXTLEN-1] = '\0';
	va_end(args);

	MessageBox(NULL, msg, "Fatal Error!", MB_OK | MB_ICONEXCLAMATION);
}

void mbothererror(const char *fmt, ...)
{
	char msg[MBTXTLEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, MBTXTLEN, fmt, args);
	msg[MBTXTLEN-1] = '\0';
	va_end(args);

	MessageBox(NULL, msg, "Error!", MB_OK | MB_ICONWARNING);
}

void mbvs(const char *fmt, ...)
{
	char msg[MBTXTLEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, MBTXTLEN, fmt, args);
	msg[MBTXTLEN-1] = '\0';
	va_end(args);

	MessageBox(NULL, msg, "Tracing", MB_OK);
}

#endif /* WIN32 and WINDOWED */


static int checkFile(char *buf, const char *fmt, ...)
{
    va_list args;
    struct stat tmp;

    va_start(args, fmt);
    vsnprintf(buf, PATH_MAX, fmt, args);
    va_end(args);

    return stat(buf, &tmp);
}

int pyi_arch_check_cookie(ARCHIVE_STATUS *status, int filelen)
{
	if (fseek(status->fp, filelen-(int)sizeof(COOKIE), SEEK_SET))
		return -1;

	/* Read the Cookie, and check its MAGIC bytes */
	if (fread(&(status->cookie), sizeof(COOKIE), 1, status->fp) < 1)
	    return -1;
	if (strncmp(status->cookie.magic, MAGIC, strlen(MAGIC)))
		return -1;

  return 0;
}

int findDigitalSignature(ARCHIVE_STATUS * const status)
{
#ifdef WIN32
	/* There might be a digital signature attached. Let's see. */
	char buf[2];
	int offset = 0, signature_offset = 0;
	fseek(status->fp, 0, SEEK_SET);
	fread(buf, 1, 2, status->fp);
	if (!(buf[0] == 'M' && buf[1] == 'Z'))
		return -1;
	/* Skip MSDOS header */
	fseek(status->fp, 60, SEEK_SET);
	/* Read offset to PE header */
	fread(&offset, 4, 1, status->fp);
	fseek(status->fp, offset+24, SEEK_SET);
        fread(buf, 2, 1, status->fp);
        if (buf[0] == 0x0b && buf[1] == 0x01) {
          /* 32 bit binary */
          signature_offset = 152;
        }
        else if (buf[0] == 0x0b && buf[1] == 0x02) {
          /* 64 bit binary */
          signature_offset = 168;
        }
        else {
          /* Invalid magic value */
          VS("Could not find a valid magic value (was %x %x).\n", (unsigned int) buf[0], (unsigned int) buf[1]);
          return -1;
        }

	/* Jump to the fields that contain digital signature info */
	fseek(status->fp, offset+signature_offset, SEEK_SET);
	fread(&offset, 4, 1, status->fp);
	if (offset == 0)
		return -1;
  VS("%s contains a digital signature\n", status->archivename);
	return offset;
#else
	return -1;
#endif
}

/*
 * Open the archive
 * Sets f_archiveFile, f_pkgstart, f_tocbuff and f_cookie.
 */
int pyi_arch_open(ARCHIVE_STATUS *status)
{
#ifdef WIN32
	int i;
#endif
	int filelen;
    VS("archivename is %s\n", status->archivename);
	/* Physically open the file */
	status->fp = fopen(status->archivename, "rb");
	if (status->fp == NULL) {
		VS("Cannot open archive: %s\n", status->archivename);
		return -1;
	}

	/* Seek to the Cookie at the end of the file. */
	fseek(status->fp, 0, SEEK_END);
	filelen = ftell(status->fp);

	if (pyi_arch_check_cookie(status, filelen) < 0)
	{
		VS("%s does not contain an embedded package\n", status->archivename);
#ifndef WIN32
    return -1;
#else
		filelen = findDigitalSignature(status);
		if (filelen < 1)
			return -1;
		/* The digital signature has been aligned to 8-bytes boundary.
		   We need to look for our cookie taking into account some
		   padding. */
		for (i = 0; i < 8; ++i)
		{
			if (pyi_arch_check_cookie(status, filelen) >= 0)
				break;
			--filelen;
		}
		if (i == 8)
		{
			VS("%s does not contain an embedded package, even skipping the signature\n", status->archivename);
			return -1;
		}
		VS("package found skipping digital signature in %s\n", status->archivename);
#endif
	}

	/* From the cookie, calculate the archive start */
	status->pkgstart = filelen - ntohl(status->cookie.len);

	/* Read in in the table of contents */
	fseek(status->fp, status->pkgstart + ntohl(status->cookie.TOC), SEEK_SET);
	status->tocbuff = (TOC *) malloc(ntohl(status->cookie.TOClen));
	if (status->tocbuff == NULL)
	{
		FATALERROR("Could not allocate buffer for TOC.");
		return -1;
	}
	if (fread(status->tocbuff, ntohl(status->cookie.TOClen), 1, status->fp) < 1)
	{
	    FATALERROR("Could not read from file.");
	    return -1;
	}
	status->tocend = (TOC *) (((char *)status->tocbuff) + ntohl(status->cookie.TOClen));

	/* Check input file is still ok (should be). */
	if (ferror(status->fp))
	{
		FATALERROR("Error on file");
		return -1;
	}
	return 0;
}



/*
 * external API for iterating TOCs
 */
TOC *getFirstTocEntry(ARCHIVE_STATUS *status)
{
	return status->tocbuff;
}
TOC *getNextTocEntry(ARCHIVE_STATUS *status, TOC *entry)
{
	TOC *rslt = (TOC*)((char *)entry + ntohl(entry->structlen));
	if (rslt >= status->tocend)
		return NULL;
	return rslt;
}


/* decompress data in buff, described by ptoc
 * return in malloc'ed buffer (needs to be freed)
 */
unsigned char *decompress(unsigned char * buff, TOC *ptoc)
{
	const char *ver;
	unsigned char *out;
	z_stream zstream;
	int rc;

	ver = (zlibVersion)();
	out = (unsigned char *)malloc(ntohl(ptoc->ulen));
	if (out == NULL) {
		OTHERERROR("Error allocating decompression buffer\n");
		return NULL;
	}

	zstream.zalloc = NULL;
	zstream.zfree = NULL;
	zstream.opaque = NULL;
	zstream.next_in = buff;
	zstream.avail_in = ntohl(ptoc->len);
	zstream.next_out = out;
	zstream.avail_out = ntohl(ptoc->ulen);
	rc = inflateInit(&zstream);
	if (rc >= 0) {
		rc = (inflate)(&zstream, Z_FINISH);
		if (rc >= 0) {
			rc = (inflateEnd)(&zstream);
		}
		else {
			OTHERERROR("Error %d from inflate: %s\n", rc, zstream.msg);
			return NULL;
		}
	}
	else {
		OTHERERROR("Error %d from inflateInit: %s\n", rc, zstream.msg);
		return NULL;
	}

	return out;
}


/*
 * extract an archive entry
 * returns pointer to the data (must be freed)
 */
unsigned char *extract(ARCHIVE_STATUS *status, TOC *ptoc)
{
	unsigned char *data;
	unsigned char *tmp;

	fseek(status->fp, status->pkgstart + ntohl(ptoc->pos), SEEK_SET);
	data = (unsigned char *)malloc(ntohl(ptoc->len));
	if (data == NULL) {
		OTHERERROR("Could not allocate read buffer\n");
		return NULL;
	}
	if (fread(data, ntohl(ptoc->len), 1, status->fp) < 1) {
	    OTHERERROR("Could not read from file\n");
	    return NULL;
	}
	if (ptoc->cflag == '\2') {
        static PyObject *AES = NULL;
		PyObject *func_new;
		PyObject *aes_dict;
		PyObject *aes_obj;
		PyObject *ddata;
		long block_size;
		char *iv;

		if (!AES)
			AES = PI_PyImport_ImportModule("AES");
		aes_dict = PI_PyModule_GetDict(AES);
		func_new = PI_PyDict_GetItemString(aes_dict, "new");
		block_size = PI_PyInt_AsLong(PI_PyDict_GetItemString(aes_dict, "block_size"));
		iv = malloc(block_size);
		memset(iv, 0, block_size);

		aes_obj = PI_PyObject_CallFunction(func_new, "s#Os#",
			data, 32,
			PI_PyDict_GetItemString(aes_dict, "MODE_CFB"),
			iv, block_size);

		ddata = PI_PyObject_CallMethod(aes_obj, "decrypt", "s#", data+32, ntohl(ptoc->len)-32);
		memcpy(data, PI_PyString_AsString(ddata), ntohl(ptoc->len)-32);
		Py_DECREF(aes_obj);
		Py_DECREF(ddata);
		VS("decrypted %s\n", ptoc->name);
	}
	if (ptoc->cflag == '\1' || ptoc->cflag == '\2') {
		tmp = decompress(data, ptoc);
		free(data);
		data = tmp;
		if (data == NULL) {
			OTHERERROR("Error decompressing %s\n", ptoc->name);
			return NULL;
		}
	}
	return data;
}


/*
 * extract from the archive
 * and copy to the filesystem
 * relative to the directory the archive's in
 */
int extract2fs(ARCHIVE_STATUS *status, TOC *ptoc)
{
	FILE *out;
	unsigned char *data = extract(status, ptoc);

    if (pyi_create_temp_path(status) == -1){
        return -1;
    }

	out = pyi_open_target(status->temppath, ptoc->name);

	if (out == NULL)  {
		FATALERROR("%s could not be extracted!\n", ptoc->name);
		return -1;
	}
	else {
		fwrite(data, ntohl(ptoc->ulen), 1, out);
#ifndef WIN32
		fchmod(fileno(out), S_IRUSR | S_IWUSR | S_IXUSR);
#endif
		fclose(out);
	}
	free(data);
	return 0;
}

/* Splits the item in the form path:filename */
static int splitName(char *path, char *filename, const char *item)
{
    char name[PATH_MAX + 1];

    VS("Splitting item into path and filename\n");
    strcpy(name, item);
    strcpy(path, strtok(name, ":"));
    strcpy(filename, strtok(NULL, ":")) ;

    if (path[0] == 0 || filename[0] == 0)
        return -1;
    return 0;
}

/* Copy the dependencies file from a directory to the tempdir */
static int copyDependencyFromDir(ARCHIVE_STATUS *status, const char *srcpath, const char *filename)
{
    if (pyi_create_temp_path(status) == -1){
        return -1;
    }

    VS("Coping file %s to %s\n", srcpath, status->temppath);
    if (pyi_copy_file(srcpath, status->temppath, filename) == -1) {
        return -1;
    }
    return 0;
}

/* Look for the archive identified by path into the ARCHIVE_STATUS pool status_list.
 * If the archive is found, a pointer to the associated ARCHIVE_STATUS is returned
 * otherwise the needed archive is opened and added to the pool and then returned.
 * If an error occurs, returns NULL.
 */
static ARCHIVE_STATUS *get_archive(ARCHIVE_STATUS *status_list[], const char *path)
{
    ARCHIVE_STATUS *status = NULL;
    int i = 0;

    VS("Getting file from archive.\n");
    if (pyi_create_temp_path(status_list[SELF]) == -1){
        return NULL;
    }

    for (i = 1; status_list[i] != NULL; i++){
        if (strcmp(status_list[i]->archivename, path) == 0) {
            VS("Archive found: %s\n", path);
            return status_list[i];
        }
        VS("Checking next archive in the list...\n");
    }

    if ((status = (ARCHIVE_STATUS *) calloc(1, sizeof(ARCHIVE_STATUS))) == NULL) {
        FATALERROR("Error allocating memory for status\n");
        return NULL;
    }

    strcpy(status->archivename, path);
    strcpy(status->homepath, status_list[SELF]->homepath);
    strcpy(status->temppath, status_list[SELF]->temppath);
#ifdef WIN32
    strcpy(status->homepathraw, status_list[SELF]->homepathraw);
    strcpy(status->temppathraw, status_list[SELF]->temppathraw);
#endif

    if (pyi_arch_open(status)) {
        FATALERROR("Error openning archive %s\n", path);
        free(status);
        return NULL;
    }

    status_list[i] = status;
    return status;
}

/* Extract a file identifed by filename from the archive associated to status. */
static int extractDependencyFromArchive(ARCHIVE_STATUS *status, const char *filename)
{
	TOC * ptoc = status->tocbuff;
	VS("Extracting dependencies from archive\n");
	while (ptoc < status->tocend) {
		if (strcmp(ptoc->name, filename) == 0)
			if (extract2fs(status, ptoc))
				return -1;
		ptoc = incrementTocPtr(status, ptoc);
	}
	return 0;
}

/* Decide if the dependency identified by item is in a onedir or onfile archive
 * then call the appropriate function.
 */
static int extractDependency(ARCHIVE_STATUS *status_list[], const char *item)
{
    ARCHIVE_STATUS *status = NULL;
    char path[PATH_MAX + 1];
    char filename[PATH_MAX + 1];
    char srcpath[PATH_MAX + 1];
    char archive_path[PATH_MAX + 1];

    char *dirname = NULL;

    VS("Extracting dependencies\n");
    if (splitName(path, filename, item) == -1)
        return -1;

    dirname = pyi_path_dirname(path);
    if (dirname[0] == 0) {
        free(dirname);
        return -1;
    }

    /* We need to identify three situations: 1) dependecies are in a onedir archive
     * next to the current onefile archive, 2) dependencies are in a onedir/onefile
     * archive next to the current onedir archive, 3) dependencies are in a onefile
     * archive next to the current onefile archive.
     */
    VS("Checking if file exists\n");
    if (checkFile(srcpath, "%s/%s/%s", status_list[SELF]->homepath, dirname, filename) == 0) {
        VS("File %s found, assuming is onedir\n", srcpath);
        if (copyDependencyFromDir(status_list[SELF], srcpath, filename) == -1) {
            FATALERROR("Error coping %s\n", filename);
            free(dirname);
            return -1;
        }
    } else if (checkFile(srcpath, "%s../%s/%s", status_list[SELF]->homepath, dirname, filename) == 0) {
        VS("File %s found, assuming is onedir\n", srcpath);
        if (copyDependencyFromDir(status_list[SELF], srcpath, filename) == -1) {
            FATALERROR("Error coping %s\n", filename);
            free(dirname);
            return -1;
        }
    } else {
        VS("File %s not found, assuming is onefile.\n", srcpath);
        if ((checkFile(archive_path, "%s%s.pkg", status_list[SELF]->homepath, path) != 0) &&
            (checkFile(archive_path, "%s%s.exe", status_list[SELF]->homepath, path) != 0) &&
            (checkFile(archive_path, "%s%s", status_list[SELF]->homepath, path) != 0)) {
            FATALERROR("Archive not found: %s\n", archive_path);
            return -1;
        }

        if ((status = get_archive(status_list, archive_path)) == NULL) {
            FATALERROR("Archive not found: %s\n", archive_path);
            return -1;
        }
        if (extractDependencyFromArchive(status, filename) == -1) {
            FATALERROR("Error extracting %s\n", filename);
            free(status);
            return -1;
        }
    }
    free(dirname);

    return 0;
}


/*
 * check if binaries need to be extracted. If not, this is probably a onedir solution,
 * and a child process will not be required on windows.
 */
int needToExtractBinaries(ARCHIVE_STATUS *status_list[])
{
	TOC * ptoc = status_list[SELF]->tocbuff;
	while (ptoc < status_list[SELF]->tocend) {
		if (ptoc->typcd == ARCHIVE_ITEM_BINARY || ptoc->typcd == ARCHIVE_ITEM_DATA ||
                ptoc->typcd == ARCHIVE_ITEM_ZIPFILE)
            return true;
        if (ptoc->typcd == ARCHIVE_ITEM_DEPENDENCY) {
            return true;
        }
		ptoc = incrementTocPtr(status_list[SELF], ptoc);
	}
	return false;
}

/*
 * extract all binaries (type 'b') and all data files (type 'x') to the filesystem
 * and checks for dependencies (type 'd'). If dependencies are found, extract them.
 */
int extractBinaries(ARCHIVE_STATUS *status_list[])
{
	TOC * ptoc = status_list[SELF]->tocbuff;
	VS("Extracting binaries\n");
	while (ptoc < status_list[SELF]->tocend) {
		if (ptoc->typcd == ARCHIVE_ITEM_BINARY || ptoc->typcd == ARCHIVE_ITEM_DATA ||
                ptoc->typcd == ARCHIVE_ITEM_ZIPFILE)
			if (extract2fs(status_list[SELF], ptoc))
				return -1;

        if (ptoc->typcd == ARCHIVE_ITEM_DEPENDENCY) {
            if (extractDependency(status_list, ptoc->name) == -1)
                return -1;
        }
		ptoc = incrementTocPtr(status_list[SELF], ptoc);
	}
	return 0;
}

/*
 * Run scripts
 * Return non zero on failure
 */
int pyi_pylib_run_scripts(ARCHIVE_STATUS *status)
{
	unsigned char *data;
	char buf[PATH_MAX];
	int rc = 0;
	TOC * ptoc = status->tocbuff;
	PyObject *__main__ = PI_PyImport_AddModule("__main__");
	PyObject *__file__;
	VS("Running scripts\n");

	/* Iterate through toc looking for scripts (type 's') */
	while (ptoc < status->tocend) {
		if (ptoc->typcd == ARCHIVE_ITEM_PYSOURCE) {
			/* Get data out of the archive.  */
			data = extract(status, ptoc);
			/* Set the __file__ attribute within the __main__ module,
			   for full compatibility with normal execution. */
			strcpy(buf, ptoc->name);
			strcat(buf, ".py");
            __file__ = PI_PyString_FromStringAndSize(buf, strlen(buf));
            PI_PyObject_SetAttrString(__main__, "__file__", __file__);
            Py_DECREF(__file__);
			/* Run it */
			rc = PI_PyRun_SimpleString((char *) data);
			/* log errors and abort */
			if (rc != 0) {
				VS("RC: %d from %s\n", rc, ptoc->name);
				return rc;
			}
			free(data);
		}

		ptoc = incrementTocPtr(status, ptoc);
	}
	return 0;
}

/*
 * call a simple "int func(void)" entry point.  Assumes such a function
 * exists in the main namespace.
 * Return non zero on failure, with -2 if the specific error is
 * that the function does not exist in the namespace.
 */
int callSimpleEntryPoint(char *name, int *presult)
{
	int rc = -1;
	/* Objects with no ref. */
	PyObject *mod, *dict;
	/* Objects with refs to kill. */
	PyObject *func = NULL, *pyresult = NULL;

	mod = PI_PyImport_AddModule("__main__"); /* NO ref added */
	if (!mod) {
		VS("No __main__\n");
		goto done;
	}
	dict = PI_PyModule_GetDict(mod); /* NO ref added */
	if (!mod) {
		VS("No __dict__\n");
		goto done;
	}
	func = PI_PyDict_GetItemString(dict, name);
	if (func == NULL) { /* should explicitly check KeyError */
		VS("CallSimpleEntryPoint can't find the function name\n");
		rc = -2;
		goto done;
	}
	pyresult = PI_PyObject_CallFunction(func, "");
	if (pyresult==NULL) goto done;
	PI_PyErr_Clear();
	*presult = PI_PyInt_AsLong(pyresult);
	rc = PI_PyErr_Occurred() ? -1 : 0;
	VS( rc ? "Finished with failure\n" : "Finished OK\n");
	/* all done! */
done:
	Py_XDECREF(func);
	Py_XDECREF(pyresult);
	/* can't leave Python error set, else it may
	   cause failures in later async code */
	if (rc)
		/* But we will print them 'cos they may be useful */
		PI_PyErr_Print();
	PI_PyErr_Clear();
	return rc;
}

/* for finer grained control */
/*
 * initialize (this always needs to be done)
 */
int init(ARCHIVE_STATUS *status, char const * archivePath, char  const * archiveName)
{
	/* Set up paths */
	if (pyi_arch_set_paths(status, archivePath, archiveName))
		return -1;

	/* Open the archive */
	if (pyi_arch_open(status))
		return -1;

	return 0;
}

/*
 * Once init'ed, you might want to extractBinaries()
 * If you do, what comes after is very platform specific.
 * Once you've taken care of the platform specific details,
 * or if there are no binaries to extract, you go on
 * to doIt(), which is the important part.
 */
int doIt(ARCHIVE_STATUS *status, int argc, char *argv[])
{
	int rc = 0;
	/* Load Python DLL */
	if (pyi_pylib_load(status))
		return -1;

	/* Start Python. */
	if (pyi_pylib_start_python(status, argc, argv))
		return -1;

	/* Import modules from archive - bootstrap */
	if (pyi_pylib_import_modules(status))
		return -1;

	/* Install zlibs  - now all hooks in place */
	if (pyi_pylib_install_zlibs(status))
		return -1;

	/* Run scripts */
	rc = pyi_pylib_run_scripts(status);

	VS("OK.\n");

	return rc;
}

/*
 * Helpers for embedders
 */
int pyi_arch_get_pyversion(ARCHIVE_STATUS *status)
{
	return ntohl(status->cookie.pyvers);
}
