/*
 *
 * Copyright (c) 2013 Mikko Mononen memon@inside.org
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 */

/* vim: set ts=8 sts=4 sw=4 : */

#include <tcl.h>
#ifndef MODULE_SCOPE
#define MODULE_SCOPE extern
#endif
#include <stdio.h>
#include <string.h>
#include <float.h>
#define NANOSVG_malloc	ckalloc
#define NANOSVG_realloc	ckrealloc
#define NANOSVG_free	ckfree
#define NANOSVG_SCOPE MODULE_SCOPE
#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#include <tk.h>

static int		FileMatchSVG(Tcl_Channel chan, const char *fileName,
			    Tcl_Obj *format, int *widthPtr, int *heightPtr,
			    Tcl_Interp *interp);
static int		FileReadSVG(Tcl_Interp *interp, Tcl_Channel chan,
			    const char *fileName, Tcl_Obj *format,
			    Tk_PhotoHandle imageHandle, int destX, int destY,
			    int width, int height, int srcX, int srcY);
static int		StringMatchSVG(Tcl_Obj *dataObj, Tcl_Obj *format,
			    int *widthPtr, int *heightPtr, Tcl_Interp *interp);
static int		StringReadSVG(Tcl_Interp *interp, Tcl_Obj *dataObj,
			    Tcl_Obj *format, Tk_PhotoHandle imageHandle,
			    int destX, int destY, int width, int height,
			    int srcX, int srcY);
static NSVGimage *	ParseSVGWithOptions(Tcl_Interp *interp,
			    const char *input, int length, Tcl_Obj *format);
static int		RasterizeSVG(Tcl_Interp *interp,
			    Tk_PhotoHandle imageHandle, NSVGimage *nsvgImage,
			    int destX, int destY, int width, int height,
			    int srcX, int srcY);
static int		CacheSVG(Tcl_Interp *interp, ClientData dataOrChan,
			    Tcl_Obj *formatObj, NSVGimage *nsvgImage);
static NSVGimage *	GetCachedSVG(Tcl_Interp *interp, ClientData dataOrChan,
			    Tcl_Obj *formatObj);
static void		CleanCache(Tcl_Interp *interp);
static void		FreeCache(ClientData clientData, Tcl_Interp *interp);

static Tk_PhotoImageFormat tkImgFmtSVG = {
    "svg",			/* name */
    FileMatchSVG,		/* fileMatchProc */
    StringMatchSVG,		/* stringMatchProc */
    FileReadSVG,		/* fileReadProc */
    StringReadSVG,		/* stringReadProc */
    NULL,			/* fileWriteProc */
    NULL,			/* stringWriteProc */
    NULL
};

/*
 * Per interp cache of last NSVGimage which was matched to
 * be immediately rasterized after the match. This helps to
 * eliminate double parsing of the SVG file/string.
 */

typedef struct {
    ClientData dataOrChan;
    Tcl_DString formatString;
    NSVGimage *nsvgImage;
} NSVGcache;

static int
FileMatchSVG(
    Tcl_Channel chan,
    const char *fileName,
    Tcl_Obj *formatObj,
    int *widthPtr, int *heightPtr,
    Tcl_Interp *interp)
{
    int length;
    Tcl_Obj *dataObj = Tcl_NewObj();
    const char *data;
    NSVGimage *nsvgImage;

    CleanCache(interp);
    if (Tcl_ReadChars(chan, dataObj, -1, 0) == -1) {
	/* in case of an error reading the file */
	Tcl_DecrRefCount(dataObj);
	return 0;
    }
    data = Tcl_GetStringFromObj(dataObj, &length);
    nsvgImage = ParseSVGWithOptions(interp, data, length, formatObj);
    Tcl_DecrRefCount(dataObj);
    if (nsvgImage != NULL) {
	*widthPtr = nsvgImage->width;
	*heightPtr = nsvgImage->height;
	if (!CacheSVG(interp, chan, formatObj, nsvgImage)) {
	    nsvgDelete(nsvgImage);
	}
	return 1;
    }
    return 0;
}

static int
FileReadSVG(
    Tcl_Interp *interp,
    Tcl_Channel chan,
    const char *fileName,
    Tcl_Obj *formatObj,
    Tk_PhotoHandle imageHandle,
    int destX, int destY,
    int width, int height,
    int srcX, int srcY)
{
    int length;
    const char *data;
    NSVGimage *nsvgImage = GetCachedSVG(interp, chan, formatObj);

    if (nsvgImage == NULL) {
        Tcl_Obj *dataObj = Tcl_NewObj();

	if (Tcl_ReadChars(chan, dataObj, -1, 0) == -1) {
	    /* in case of an error reading the file */
	    Tcl_DecrRefCount(dataObj);
	    Tcl_SetResult(interp, "read error", TCL_STATIC);
	    Tcl_SetErrorCode(interp, "TK", "IMAGE", "SVG", "READ_ERROR", NULL);
	    return TCL_ERROR;
	}
        data = Tcl_GetStringFromObj(dataObj, &length);
	nsvgImage = ParseSVGWithOptions(interp, data, length, formatObj);
	Tcl_DecrRefCount(dataObj);
	if (nsvgImage == NULL) {
	    return TCL_ERROR;
	}
    }
    return RasterizeSVG(interp, imageHandle, nsvgImage, destX, destY,
		width, height, srcX, srcY);
}

static int
StringMatchSVG(
    Tcl_Obj *dataObj,
    Tcl_Obj *formatObj,
    int *widthPtr, int *heightPtr,
    Tcl_Interp *interp)
{
    int length;
    const char *data;
    NSVGimage *nsvgImage;

    CleanCache(interp);
    data = Tcl_GetStringFromObj(dataObj, &length);
    nsvgImage = ParseSVGWithOptions(interp, data, length, formatObj);
    if (nsvgImage != NULL) {
	*widthPtr = nsvgImage->width;
	*heightPtr = nsvgImage->height;
	if (!CacheSVG(interp, dataObj, formatObj, nsvgImage)) {
	    nsvgDelete(nsvgImage);
	}
	return 1;
    }
    return 0;
}

static int
StringReadSVG(
    Tcl_Interp *interp,
    Tcl_Obj *dataObj,
    Tcl_Obj *formatObj,
    Tk_PhotoHandle imageHandle,
    int destX, int destY,
    int width, int height,
    int srcX, int srcY)
{   
    int length;
    const char *data;
    NSVGimage *nsvgImage = GetCachedSVG(interp, dataObj, formatObj);

    if (nsvgImage == NULL) {
        data = Tcl_GetStringFromObj(dataObj, &length);
	nsvgImage = ParseSVGWithOptions(interp, data, length, formatObj);
    }
    if (nsvgImage == NULL) {
	return TCL_ERROR;
    }
    return RasterizeSVG(interp, imageHandle, nsvgImage, destX, destY,
		width, height, srcX, srcY);
}

static NSVGimage *
ParseSVGWithOptions(
    Tcl_Interp *interp,
    const char *input,
    int length,
    Tcl_Obj *formatObj)
{
    Tcl_Obj **objv = NULL;
    int objc = 0;
    double dpi = 96.0;
    char unit[3], *p;
    char *inputCopy = NULL;
    NSVGimage *nsvgImage;
    static const char *const fmtOptions[] = {
        "-dpi", "-unit", NULL
    };
    enum fmtOptions {
        OPT_DPI, OPT_UNIT
    };

    /*
     * The parser destroys the original input string,
     * therefore first duplicate.
     */

    inputCopy = attemptckalloc(length+1);
    if (inputCopy == NULL) {
	Tcl_SetResult(interp, "cannot alloc data buffer", TCL_STATIC);
	Tcl_SetErrorCode(interp, "TK", "IMAGE", "SVG", "OUT_OF_MEMORY", NULL);
	goto error;
    }
    memcpy(inputCopy, input, length);
    inputCopy[length] = '\0';

    /*
     * Process elements of format specification as a list.
     */

    strcpy(unit, "px");
    if ((formatObj != NULL) &&
	    Tcl_ListObjGetElements(interp, formatObj, &objc, &objv) != TCL_OK) {
        goto error;
    }
    for (; objc > 0 ; objc--, objv++) {
	int optIndex;

	/*
	 * Ignore the "svg" part of the format specification.
	 */

	if (!strcasecmp(Tcl_GetString(objv[0]), "svg")) {
	    continue;
	}

	if (Tcl_GetIndexFromObjStruct(interp, objv[0], fmtOptions,
		sizeof(char *), "option", 0, &optIndex) == TCL_ERROR) {
	    goto error;
	}

	if (objc < 2) {
	    ckfree(inputCopy);
	    Tcl_WrongNumArgs(interp, 1, objv, "value");
	    goto error;
	}

	objc--;
	objv++;

	switch ((enum fmtOptions) optIndex) {
	case OPT_DPI:
	    if (Tcl_GetDoubleFromObj(interp, objv[0], &dpi) == TCL_ERROR) {
	        goto error;
	    }
	    if (dpi < 0.0) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"-dpi value must be positive", -1));
		Tcl_SetErrorCode(interp, "TK", "IMAGE", "SVG", "BAD_DPI",
			NULL);
		goto error;
	    }
	    break;
	case OPT_UNIT:
	    p = Tcl_GetString(objv[0]);
	    if ((p != NULL) && (p[0])) {
	        strncpy(unit, p, 3);
		unit[2] = '\0';
	    }
	    break;
	}
    }

    nsvgImage = nsvgParse(inputCopy, unit, dpi);
    if (nsvgImage == NULL) {
        Tcl_SetResult(interp, "cannot parse SVG image", TCL_STATIC);
	Tcl_SetErrorCode(interp, "TK", "IMAGE", "SVG", "PARSE_ERROR", NULL);
	goto error;
    }
    ckfree(inputCopy);
    return nsvgImage;

error:
    if (inputCopy != NULL) {
        ckfree(inputCopy);
    }
    return NULL;
}

static int
RasterizeSVG(
    Tcl_Interp *interp,
    Tk_PhotoHandle imageHandle,
    NSVGimage *nsvgImage,
    int destX, int destY,
    int width, int height,
    int srcX, int srcY)
{   
    int w, h, c;
    NSVGrasterizer *rast;
    unsigned char *imgData;
    Tk_PhotoImageBlock svgblock;

    w = nsvgImage->width;
    h = nsvgImage->height; 
    rast = nsvgCreateRasterizer();
    if (rast == NULL) {
	Tcl_SetResult(interp, "cannot initialize rasterizer", TCL_STATIC);
	Tcl_SetErrorCode(interp, "TK", "IMAGE", "SVG", "RASTERIZER_ERROR",
		NULL);
	goto cleanAST;
    }
    imgData = attemptckalloc(w * h *4);
    if (imgData == NULL) {
	Tcl_SetResult(interp, "cannot alloc image buffer", TCL_STATIC);
	Tcl_SetErrorCode(interp, "TK", "IMAGE", "SVG", "OUT_OF_MEMORY", NULL);
	goto cleanRAST;
    }
    nsvgRasterize(rast, nsvgImage, 0, 0, 1, imgData, w, h, w * 4);
    /* transfer the data to a photo block */
    svgblock.pixelPtr = imgData;
    svgblock.width = w;
    svgblock.height = h;
    svgblock.pitch = w * 4;
    svgblock.pixelSize = 4;
    for (c = 0; c <= 3; c++) {
	svgblock.offset[c] = c;
    }
    if (Tk_PhotoExpand(interp, imageHandle,
		destX + width, destY + height) != TCL_OK) {
	goto cleanRAST;
    }
    if (Tk_PhotoPutBlock(interp, imageHandle, &svgblock, destX, destY,
		width, height, TK_PHOTO_COMPOSITE_SET) != TCL_OK) {
	goto cleanimg;
    }
    ckfree(imgData);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(nsvgImage);
    return TCL_OK;

cleanimg:
    ckfree(imgData);
    
cleanRAST:
    nsvgDeleteRasterizer(rast);

cleanAST:
    nsvgDelete(nsvgImage);
    return TCL_ERROR;
}

static int
CacheSVG(
    Tcl_Interp *interp,
    ClientData dataOrChan,
    Tcl_Obj *formatObj,
    NSVGimage *nsvgImage)
{
    int length;
    const char *data;
    NSVGcache *cachePtr = Tcl_GetAssocData(interp, "tksvg", NULL);

    if (cachePtr != NULL) {
        cachePtr->dataOrChan = dataOrChan;
	if (formatObj != NULL) {
	    data = Tcl_GetStringFromObj(formatObj, &length);
	    Tcl_DStringAppend(&cachePtr->formatString, data, length);
	}
	cachePtr->nsvgImage = nsvgImage;
	return 1;
    }
    return 0;
}

static NSVGimage *
GetCachedSVG(
    Tcl_Interp *interp,
    ClientData dataOrChan,
    Tcl_Obj *formatObj)
{
    int length;
    const char *data;
    NSVGcache *cachePtr = Tcl_GetAssocData(interp, "tksvg", NULL);
    NSVGimage *nsvgImage = NULL;

    if ((cachePtr != NULL) && (cachePtr->nsvgImage != NULL) &&
	(cachePtr->dataOrChan == dataOrChan)) {
        if (formatObj != NULL) {
	    data = Tcl_GetStringFromObj(formatObj, &length);
	    if (strcmp(data, Tcl_DStringValue(&cachePtr->formatString)) == 0) {
	        nsvgImage = cachePtr->nsvgImage;
		cachePtr->nsvgImage = NULL;
	    }
	} else if (Tcl_DStringLength(&cachePtr->formatString) == 0) {
	    nsvgImage = cachePtr->nsvgImage;
	    cachePtr->nsvgImage = NULL;
	}
    }
    CleanCache(interp);
    return nsvgImage;
}

static void
CleanCache(Tcl_Interp *interp)
{
    NSVGcache *cachePtr = Tcl_GetAssocData(interp, "tksvg", NULL);

    if (cachePtr != NULL) {
        cachePtr->dataOrChan = NULL;
        Tcl_DStringSetLength(&cachePtr->formatString, 0);
	if (cachePtr->nsvgImage != NULL) {
	    nsvgDelete(cachePtr->nsvgImage);
	    cachePtr->nsvgImage = NULL;
	}
    }
}

static void
FreeCache(ClientData clientData, Tcl_Interp *interp)
{
    NSVGcache *cachePtr = clientData;

    Tcl_DStringFree(&cachePtr->formatString);
    if (cachePtr->nsvgImage != NULL) {
        nsvgDelete(cachePtr->nsvgImage);
    }
    ckfree(cachePtr);
}

int
Tksvg_Init(Tcl_Interp *interp)
{
    NSVGcache *cachePtr;

    if (interp == NULL) {
        return TCL_ERROR;
    }
#ifdef USE_TCL_STUBS
    if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }
#else
    if (Tcl_PkgRequire(interp, "Tcl", TCL_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }
#endif
#ifdef USE_TK_STUBS
    if (Tk_InitStubs(interp, TCL_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }
#else
    if (Tcl_PkgRequire(interp, "Tk", TK_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }
#endif
    cachePtr = ckalloc(sizeof(NSVGcache));
    cachePtr->dataOrChan = NULL;
    Tcl_DStringInit(&cachePtr->formatString);
    cachePtr->nsvgImage = NULL;
    Tcl_SetAssocData(interp, "tksvg", FreeCache, cachePtr);
    Tk_CreatePhotoImageFormat(&tkImgFmtSVG);
    Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION);
    return TCL_OK;
}
